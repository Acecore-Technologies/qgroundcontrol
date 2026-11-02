/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMissionCheck.h"
#include "GeoFenceController.h"
#include "GeoZone.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneCatalog.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneFenceImporter.h"
#include "GeoZoneFenceTags.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionReader.h"
#include "GeoZoneMissionWorker.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZonePlanning.h"
#include "GeoZoneRemoteId.h"
#include "GeoZoneSource.h"
#include "GeoZoneSourceRepository.h"
#include "GeoZoneSourceVersion.h"
#include "MissionController.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "RallyPoint.h"
#include "RallyPointController.h"
#include "RemoteIDManager.h"
#include "RemoteIDSettings.h"
#include "SettingsManager.h"
#include "SimpleMissionItem.h"
#include "Vehicle.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

/// The checks alive, on the GUI thread
QList<const GeoZoneMissionCheck*> &checks()
{
    static QList<const GeoZoneMissionCheck*> list;
    return list;
}

} // namespace

GeoZoneMissionCheck::GeoZoneMissionCheck(QObject *parent)
    : GeoZoneMissionCheck(GeoZoneManager::instance(), parent)
{
}

GeoZoneMissionCheck::GeoZoneMissionCheck(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _worker(new GeoZoneMissionWorker(_updateDelayMSecs, this))
{
    checks().append(this);
    (void) connect(_worker, &GeoZoneMissionWorker::triggered, this, &GeoZoneMissionCheck::_updateConcurrently);

    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMissionCheck::_clearZones);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, _worker, &GeoZoneMissionWorker::schedule);
    (void) connect(_manager->planning(), &GeoZonePlanning::plannedStartChanged, _worker, &GeoZoneMissionWorker::schedule);
    (void) connect(_manager->planning()->authorisations(), &GeoZoneAuthorisations::changed, _worker, &GeoZoneMissionWorker::schedule);
    (void) connect(_manager->planning(), &GeoZonePlanning::planAuditChanged, this, [this]() {
        // Running evaluations may be of the previous plan
        _worker->invalidate();
        _worker->schedule();
    });
    // The audit waits for the sources to load
    (void) connect(_manager->repository(), &GeoZoneSourceRepository::busyChanged, this, [this](bool busy) {
        if (!busy) {
            _worker->schedule();
        }
    });
    // U-space airspace entered needs the Remote ID ready
    (void) connect(this, &GeoZoneMissionCheck::zonesChanged, this, &GeoZoneMissionCheck::remoteIdChanged);
    RemoteIDSettings *const remoteIdSettings = SettingsManager::instance()->remoteIDSettings();
    for (Fact *const fact : { remoteIdSettings->operatorID(), remoteIdSettings->operatorIDValid(), remoteIdSettings->region() }) {
        (void) connect(fact, &Fact::rawValueChanged, this, &GeoZoneMissionCheck::remoteIdChanged);
    }
    // The mission no longer makes the area of interest
    (void) connect(this, &QObject::destroyed, _manager, [manager = _manager, owner = static_cast<const QObject*>(this)]() {
        manager->repository()->setMissionArea(owner, QGeoRectangle());
        manager->planning()->setMissionArea(QGeoRectangle());
        manager->planning()->setPlannedEnd(QDateTime());
    });
}

GeoZoneMissionCheck::~GeoZoneMissionCheck()
{
    (void) checks().removeOne(this);
}

bool GeoZoneMissionCheck::settled() const
{
    return _worker->settled();
}

const GeoZoneMissionCheck *GeoZoneMissionCheck::settledCheck(const GeoZoneManager *manager, const MissionController *missionController)
{
    for (const GeoZoneMissionCheck *check : std::as_const(checks())) {
        if ((check->_manager == manager) && (check->_worker->missionController() == missionController) && check->settled()) {
            return check;
        }
    }
    return nullptr;
}

MissionController *GeoZoneMissionCheck::missionController() const
{
    return _worker->missionController();
}

void GeoZoneMissionCheck::setMissionController(MissionController *missionController)
{
    if (missionController == _worker->missionController()) {
        return;
    }

    if (_worker->missionController()) {
        (void) disconnect(_worker->missionController(), nullptr, this, nullptr);
    }
    if (_masterController) {
        (void) disconnect(_masterController, nullptr, this, nullptr);
        (void) disconnect(_masterController, nullptr, _worker, nullptr);
    }
    (void) _worker->setMissionController(missionController);
    // The zones of the previous mission don't apply to this one
    _clearResults();
    _masterController = missionController ? missionController->masterController() : nullptr;
    if (missionController) {
        (void) connect(missionController, &MissionController::visualItemsChanged, this, &GeoZoneMissionCheck::_connectMissionItems);
        // The returns depend on the vehicle's parameters
        if (_masterController) {
            (void) connect(_masterController, &PlanMasterController::managerVehicleChanged, _worker, &GeoZoneMissionWorker::schedule);
            (void) connect(_masterController, &PlanMasterController::managerVehicleChanged, this, &GeoZoneMissionCheck::_connectVehicle);
        }
        _connectMissionItems();
    }
    _connectPlan();
    _connectVehicle();

    emit missionControllerChanged();
    _worker->schedule();
}

void GeoZoneMissionCheck::_connectPlan()
{
    delete _planConnections;
    _planConnections = new QObject(this);
    if (!_masterController) {
        _connectRallyPoints();
        return;
    }
    // The returns may go to the rally points, the fence breach action to the breach return point
    GeoFenceController *const geoFenceController = _masterController->geoFenceController();
    (void) connect(geoFenceController, &GeoFenceController::breachReturnPointChanged, _planConnections, [this]() { _worker->schedule(); });
    (void) connect(geoFenceController->breachReturnAltitude(), &Fact::rawValueChanged, _planConnections, [this]() { _worker->schedule(); });
    (void) connect(_masterController->rallyPointController()->points(), &QmlObjectListModel::countChanged, _planConnections, [this]() {
        _connectRallyPoints();
        _worker->schedule();
    });
    _connectRallyPoints();
}

void GeoZoneMissionCheck::_connectRallyPoints()
{
    delete _rallyConnections;
    _rallyConnections = new QObject(this);
    if (!_masterController) {
        return;
    }
    const QmlObjectListModel *const rallyPoints = _masterController->rallyPointController()->points();
    for (int i = 0; i < rallyPoints->count(); i++) {
        if (RallyPoint *const rallyPoint = rallyPoints->value<RallyPoint*>(i)) {
            (void) connect(rallyPoint, &RallyPoint::coordinateChanged, _rallyConnections, [this]() { _worker->schedule(); });
        }
    }
}

void GeoZoneMissionCheck::_connectVehicle()
{
    delete _vehicleConnections;
    _vehicleConnections = new QObject(this);
    Vehicle *const vehicle = _masterController ? _masterController->managerVehicle() : nullptr;
    if (_vehicle && (_vehicle != vehicle) && _vehicle->parameterManager()) {
        (void) disconnect(_vehicle->parameterManager(), nullptr, this, nullptr);
        (void) disconnect(_vehicle->parameterManager(), nullptr, _worker, nullptr);
    }
    _vehicle = vehicle;
    emit remoteIdChanged();
    if (!_vehicle) {
        return;
    }
    if (RemoteIDManager *const remoteId = _vehicle->remoteIDManager()) {
        for (const auto signal : { &RemoteIDManager::availableChanged, &RemoteIDManager::armStatusGoodChanged, &RemoteIDManager::armStatusErrorChanged,
                                   &RemoteIDManager::commsGoodChanged, &RemoteIDManager::operatorIDGoodChanged }) {
            (void) connect(remoteId, signal, _vehicleConnections, [this]() { emit remoteIdChanged(); });
        }
    }
    // The defaults of the firmware and vehicle class apply until the parameters are known
    (void) connect(_vehicle, &Vehicle::firmwareTypeChanged, _vehicleConnections, [this]() { _worker->schedule(); });
    (void) connect(_vehicle, &Vehicle::vehicleTypeChanged, _vehicleConnections, [this]() { _worker->schedule(); });
    if (_vehicle->isOfflineEditingVehicle() || !_vehicle->parameterManager()) {
        return;
    }
    // The parameters arrive after the vehicle, each change of those the returns depend on is checked
    ParameterManager *const parameterManager = _vehicle->parameterManager();
    (void) connect(parameterManager, &ParameterManager::parametersReadyChanged, this, &GeoZoneMissionCheck::_connectVehicle, Qt::UniqueConnection);
    (void) connect(parameterManager, &ParameterManager::parametersReadyChanged, _worker, &GeoZoneMissionWorker::schedule, Qt::UniqueConnection);
    if (!parameterManager->parametersReady()) {
        return;
    }
    for (const QString &name : GeoZoneReturnModel::parameterNames()) {
        if (parameterManager->parameterExists(ParameterManager::defaultComponentId, name)) {
            (void) connect(parameterManager->getParameter(ParameterManager::defaultComponentId, name), &Fact::rawValueChanged, _vehicleConnections, [this]() {
                _worker->schedule();
            });
        }
    }
}

void GeoZoneMissionCheck::_connectMissionItems()
{
    MissionController *const missionController = _worker->missionController();
    QmlObjectListModel *const visualItems = missionController ? missionController->visualItems() : nullptr;
    if (!visualItems) {
        return;
    }
    // Loiter radii, commands, transitions and jumps change the paths checked besides the flight path
    (void) connect(visualItems, &QmlObjectListModel::countChanged, this, &GeoZoneMissionCheck::_connectMissionItems, Qt::UniqueConnection);
    for (int i = 0; i < visualItems->count(); i++) {
        if (SimpleMissionItem *const item = visualItems->value<SimpleMissionItem*>(i)) {
            (void) connect(item, &SimpleMissionItem::loiterRadiusChanged, _worker, &GeoZoneMissionWorker::schedule, Qt::UniqueConnection);
            (void) connect(item, &SimpleMissionItem::commandChanged, _worker, &GeoZoneMissionWorker::schedule, Qt::UniqueConnection);
            (void) connect(item, &SimpleMissionItem::commandChanged, this, &GeoZoneMissionCheck::_connectMissionItems, Qt::UniqueConnection);
            (void) connect(item, &VisualMissionItem::currentVTOLModeChanged, _worker, &GeoZoneMissionWorker::schedule, Qt::UniqueConnection);
            if (item->command() == MAV_CMD_DO_JUMP) {
                // The item jumped to and the repeat count
                const QmlObjectListModel *const facts = item->textFieldFacts();
                for (int j = 0; j < facts->count(); j++) {
                    if (Fact *const fact = facts->value<Fact*>(j)) {
                        (void) connect(fact, &Fact::rawValueChanged, _worker, &GeoZoneMissionWorker::schedule, Qt::UniqueConnection);
                    }
                }
            }
        }
    }
}

void GeoZoneMissionCheck::setGeoFenceController(GeoFenceController *geoFenceController)
{
    if (geoFenceController == _geoFenceController) {
        return;
    }
    if (_geoFenceController) {
        (void) disconnect(_geoFenceController, nullptr, _worker, nullptr);
        (void) disconnect(_geoFenceController->polygons(), nullptr, _worker, nullptr);
        (void) disconnect(_geoFenceController->circles(), nullptr, _worker, nullptr);
    }
    _geoFenceController = geoFenceController;
    if (_geoFenceController) {
        // Fence edits mark the lists dirty, additions and deletions change their counts
        for (QmlObjectListModel *fences : { _geoFenceController->polygons(), _geoFenceController->circles() }) {
            (void) connect(fences, &QmlObjectListModel::countChanged, _worker, &GeoZoneMissionWorker::schedule);
            (void) connect(fences, &QmlObjectListModel::dirtyChanged, _worker, &GeoZoneMissionWorker::schedule);
        }
        (void) connect(_geoFenceController, &GeoFenceController::supportedChanged, _worker, &GeoZoneMissionWorker::schedule);
    }
    emit geoFenceControllerChanged();
    _worker->schedule();
}

GeoFenceController *GeoZoneMissionCheck::geoFenceController() const
{
    return _geoFenceController;
}

QString GeoZoneMissionCheck::fenceText(const QList<Entry> &entries)
{
    const QList<const GeoZone*> unfenced = zones(entries, Unfenced);
    return unfenced.isEmpty() ? QString() : tr("The GeoFence doesn't exclude prohibited zones on the path: %1").arg(GeoZonePathEvaluator::zoneNames(unfenced, maxListedZones, false));
}

bool GeoZoneMissionCheck::fenceExcludes(GeoFenceController *geoFenceController, const GeoZone *zone)
{
    return GeoZoneFences::fencesCover(GeoZoneFenceImporter::exclusionFences(geoFenceController), zone);
}

QVariantList GeoZoneMissionCheck::zonesInfo() const
{
    QVariantList list;
    for (const Entry &entry : _entries) {
        list.append(QVariantMap{
            { QStringLiteral("zone"), QVariant::fromValue(GeoZoneInfo(entry.zone)) },
            { QStringLiteral("kind"), entry.kinds.toInt() },
            { QStringLiteral("labels"), entry.labels },
            { QStringLiteral("approximationTexts"), GeoZonePathEvaluator::approximationTexts(entry.approximations) },
        });
    }
    return list;
}

QList<const GeoZone*> GeoZoneMissionCheck::zones(const QList<Entry> &entries, Kind kind)
{
    QList<const GeoZone*> result;
    for (const Entry &entry : entries) {
        if (entry.kinds.testFlag(kind)) {
            result.append(entry.zone);
        }
    }
    return result;
}

const GeoZoneMissionCheck::Entry *GeoZoneMissionCheck::entry(const GeoZone *zone) const
{
    const auto it = std::find_if(_entries.cbegin(), _entries.cend(), [zone](const Entry &entry) { return entry.zone == zone; });
    return (it != _entries.cend()) ? &*it : nullptr;
}

namespace {

/// @return The labels of the entries of the kind in the order they come, with the zones of each
QList<std::pair<QString, QList<const GeoZone*>>> zonesByLabel(const QList<GeoZoneMissionCheck::Entry> &entries, GeoZoneMissionCheck::Kind kind)
{
    QList<std::pair<QString, QList<const GeoZone*>>> result;
    for (const GeoZoneMissionCheck::Entry &entry : entries) {
        for (const QString &label : (entry.kinds.testFlag(kind) ? entry.labels : QStringList())) {
            auto it = std::find_if(result.begin(), result.end(), [&label](const auto &labelZones) { return labelZones.first == label; });
            if (it == result.end()) {
                it = result.insert(result.end(), { label, {} });
            }
            it->second.append(entry.zone);
        }
    }
    return result;
}

} // namespace

QString GeoZoneMissionCheck::warningText(const QList<Entry> &entries)
{
    QStringList lines = crossedText(entries);
    // Notices which can't be given in time any more
    for (const Entry &entry : entries) {
        if (entry.notificationOverdue) {
            lines.append(tr("%1: %2, the notice period has passed.").arg(entry.zone->displayName(), entry.notificationNote));
        }
    }
    return lines.join(QStringLiteral("\n"));
}

QStringList GeoZoneMissionCheck::crossedText(const QList<Entry> &entries)
{
    const QList<const GeoZone*> crossed = zones(entries, Crossed);
    if (crossed.isEmpty()) {
        return {};
    }
    QStringList lines = { tr("The mission path enters restricted geozones: %1").arg(GeoZonePathEvaluator::zoneNames(crossed, maxListedZones, true)) };
    // Zones entered besides the drawn flight path, like by a loiter circle or the return to launch
    for (const auto &[label, labelZones] : zonesByLabel(entries, Crossed)) {
        lines.append(tr("Only %1 enters %2.").arg(label, GeoZonePathEvaluator::zoneNames(labelZones, maxListedZones, false)));
    }
    GeoZonePathEvaluator::Approximations approximations;
    for (const Entry &entry : entries) {
        if (entry.kinds.testFlag(Crossed)) {
            approximations |= entry.approximations;
        }
    }
    if (approximations) {
        lines.append(tr("Approximate: %1.").arg(GeoZonePathEvaluator::approximationTexts(approximations).join(QStringLiteral("; "))));
    }
    lines.append(authorisationNotes(entries, Crossed));
    return lines;
}

QStringList GeoZoneMissionCheck::authorisationNotes(const QList<Entry> &entries, Kind kind)
{
    QStringList notes;
    for (const Entry &entry : entries) {
        if (entry.kinds.testFlag(kind) && !entry.authorisationNote.isEmpty()) {
            notes.append(tr("%1: %2.").arg(entry.zone->displayName(), entry.authorisationNote));
        }
    }
    return notes;
}

QString GeoZoneMissionCheck::contingencyText() const
{
    // A line per contingency path and the items it starts from, with the zones it enters
    const QList<std::pair<QString, QList<const GeoZone*>>> labels = zonesByLabel(_entries, Contingency);
    QStringList lines;
    for (const auto &[label, labelZones] : labels) {
        if (lines.count() == maxListedZones) {
            lines.append(tr("%n more contingency path(s) enter restricted geozones.", nullptr, static_cast<int>(labels.count() - maxListedZones)));
            break;
        }
        QString line = tr("%1 would enter %2.").arg(label, GeoZonePathEvaluator::zoneNames(labelZones, maxListedZones, true));
        line[0] = line[0].toUpper();
        lines.append(line);
    }
    return lines.join(QStringLiteral("\n"));
}

QString GeoZoneMissionCheck::remoteIdText() const
{
    if (zones(USpace).isEmpty()) {
        return QString();
    }
    const QString problem = GeoZoneRemoteId::problem(GeoZoneRemoteId::status(_vehicle));
    return problem.isEmpty() ? QString() : tr("The mission enters U-space airspace, which requires Remote ID. %1").arg(problem);
}

QString GeoZoneMissionCheck::infoText() const
{
    QStringList lines;
    if (const QList<const GeoZone*> uSpace = zones(USpace); !uSpace.isEmpty()) {
        lines.append(tr("U-space airspace: %1. A flight authorisation from a U-space service provider is required.").arg(GeoZonePathEvaluator::zoneNames(uSpace, maxListedZones, false)));
    }
    if (const QList<const GeoZone*> authorised = zones(Authorised); !authorised.isEmpty()) {
        lines.append(tr("Authorised: %1.").arg(GeoZonePathEvaluator::zoneNames(authorised, maxListedZones, false)));
    }
    if (const QList<const GeoZone*> permitted = zones(Permitted); !permitted.isEmpty()) {
        lines.append(tr("Areas without restriction: %1. Check their conditions.").arg(GeoZonePathEvaluator::zoneNames(permitted, maxListedZones, false)));
    }
    const int inactive = static_cast<int>(zones(Inactive).count());
    if (inactive > 0) {
        lines.append(tr("%n zone(s) on the path are not active at the planned flight time.", nullptr, inactive));
    }
    for (const Entry &entry : _entries) {
        if (!entry.notificationNote.isEmpty() && !entry.notificationOverdue) {
            lines.append(tr("%1: %2.").arg(entry.zone->displayName(), entry.notificationNote));
        }
    }
    return lines.join(QStringLiteral("\n"));
}

std::pair<QDateTime, QDateTime> GeoZoneMissionCheck::plannedFlightTimes(const GeoZoneManager *manager, const MissionController *missionController)
{
    const QDateTime start = manager->planning()->plannedStart();
    if (!start.isValid()) {
        return {};
    }
    const double missionSeconds = missionController ? missionController->missionTime() : 0;
    return { start, start.addSecs(std::llround((std::max)(missionSeconds, 0.0))) };
}

QGeoRectangle GeoZoneMissionCheck::crossedZonesBoundingBox() const
{
    GeoZoneBox box;
    for (const GeoZone *zone : zones(Crossed)) {
        box = box.united(zone->box());
    }
    return box.toRectangle();
}

void GeoZoneMissionCheck::setUpdateDelay(int msecs)
{
    _worker->setDelay(msecs);
}

void GeoZoneMissionCheck::_clearZones(GeoZoneSource *source)
{
    _worker->invalidate();
    // The other zones stay listed until the check runs again
    const auto fromSource = [source](const GeoZone *zone) { return zone->sourceKey() == source->key(); };
    (void) _relevantZones.removeIf(fromSource);
    if (_entries.removeIf([&fromSource](const Entry &entry) { return fromSource(entry.zone); }) > 0) {
        emit zonesChanged();
    }
}

void GeoZoneMissionCheck::_clearResults()
{
    _relevantZones.clear();
    _area = GeoZoneBox();
    _manager->planning()->setMissionArea(QGeoRectangle());
    _manager->planning()->setPlannedEnd(QDateTime());
    if (!_entries.isEmpty()) {
        _entries.clear();
        emit zonesChanged();
    }
    if (!_auditText.isEmpty() || !_auditDetails.isEmpty()) {
        _auditText.clear();
        _auditDetails.clear();
        emit auditChanged();
    }
}

void GeoZoneMissionCheck::update()
{
    _worker->runNow([this]() {
        const auto [start, end] = plannedFlightTimes(_manager, _worker->missionController());
        const GeoZoneMissionPath path = GeoZoneMissionReader::path(_worker->missionController(), true);
        _manager->repository()->setMissionArea(this, missionArea(path));
        return evaluate(_manager->catalog(), path, start, end);
    }, [this](const Evaluation &evaluation) { _apply(evaluation); });
}

QGeoRectangle GeoZoneMissionCheck::missionArea(const GeoZoneMissionPath &path)
{
    QList<QGeoCoordinate> coordinates;
    for (const QList<GeoZone::Position> &positions : path.positionLists()) {
        for (const GeoZone::Position &position : positions) {
            if (position.coordinate.isValid()) {
                coordinates.append(position.coordinate);
            }
        }
    }
    for (const GeoZone::Area &area : path.areas()) {
        for (const QGeoCoordinate &vertex : area.polygon) {
            coordinates.append(vertex);
        }
    }
    return coordinates.isEmpty() ? QGeoRectangle() : QGeoRectangle(coordinates);
}

void GeoZoneMissionCheck::_updateConcurrently()
{
    const GeoZoneCatalog catalog = _manager->catalog();
    const GeoZoneMissionPath path = GeoZoneMissionReader::path(_worker->missionController(), true);
    // The services taking the area of interest download the zones around the mission
    _manager->repository()->setMissionArea(this, missionArea(path));
    const auto [start, end] = plannedFlightTimes(_manager, _worker->missionController());
    // A copy of the authorisations for the worker thread, since they change on the GUI thread
    const std::shared_ptr<GeoZoneAuthorisations> authorisations(new GeoZoneAuthorisations(QString()), [](GeoZoneAuthorisations *copy) { copy->deleteLater(); });
    authorisations->merge(_manager->planning()->authorisations()->authorisations());
    // Zones of an outdated evaluation may be gone
    _worker->run([catalog, path, start, end, authorisations]() { return evaluate(catalog, path, start, end, authorisations.get()); },
                 [this](const Evaluation &evaluation) { _apply(evaluation); });
}

GeoZoneMissionCheck::Evaluation GeoZoneMissionCheck::evaluate(const GeoZoneCatalog &catalog, const GeoZoneMissionPath &path, const QDateTime &start,
                                                              const QDateTime &end, const GeoZoneAuthorisations *authorisations)
{
    // DO_JUMP items may repeat parts of the mission past its estimated end
    const QDateTime flightEnd = path.flightEnd(start, end);
    const GeoZonePathEvaluator evaluator(catalog, GeoZonePolicy::mission(start, flightEnd), start);
    const GeoZonePolicy authorisationPolicy = GeoZonePolicy::mission(start, flightEnd).excludingAuthorised(authorisations);
    Evaluation evaluation;
    evaluation.start = start;
    evaluation.end = flightEnd;
    evaluation.area = path.box();
    const QList<GeoZonePathEvaluator::Path> paths = path.evaluatorPaths(start, end);
    for (const GeoZonePathEvaluator::ZoneResult &result : evaluator.evaluate(paths, path.areas()).zones) {
        if (!result.entered()) {
            continue;
        }
        if (const GeoZoneAuthorisation *const authorisation = authorisationPolicy.authorisation(result.zone); authorisation && authorisation->limitsPath()) {
            evaluation.scopeIssues.insert(result.zone, evaluator.scopeIssue(result, *authorisation, paths, path.areas()));
        }
        evaluation.zones.append(result.zone);
        evaluation.relevantZones.append(result.zone);
        if (const GeoZonePathEvaluator::Approximations approximations = result.approximations) {
            evaluation.approximations.insert(result.zone, approximations);
        }
        if (!result.active()) {
            evaluation.inactiveZones.append(result.zone);
        } else if (!result.labels.isEmpty()) {
            evaluation.pathLabels.insert(result.zone, result.labels);
        }
    }
    GeoZonePathEvaluator::sortBySeverity(evaluation.zones);

    // Zones the returns from along the mission enter while active
    const QList<GeoZonePathEvaluator::Path> contingencyPaths = path.contingencyEvaluatorPaths(start, end);
    if (!contingencyPaths.isEmpty()) {
        const GeoZonePathEvaluator::Result result = evaluator.evaluate(contingencyPaths);
        evaluation.contingencyLabels = groupContingencies(path, result);
        for (const GeoZonePathEvaluator::ZoneResult &zoneResult : result.zones) {
            if (evaluation.contingencyLabels.contains(zoneResult.zone)) {
                evaluation.contingencyZones.append(zoneResult.zone);
            }
            if (!zoneResult.crossings.isEmpty() && !evaluation.relevantZones.contains(zoneResult.zone)) {
                evaluation.relevantZones.append(zoneResult.zone);
            }
        }
        GeoZonePathEvaluator::sortBySeverity(evaluation.contingencyZones);
    }
    return evaluation;
}

GeoZoneMissionCheck::Evaluation GeoZoneMissionCheck::evaluatePlan(const GeoZoneManager *manager, MissionController *missionController)
{
    const auto [start, end] = plannedFlightTimes(manager, missionController);
    return evaluate(manager->catalog(), GeoZoneMissionReader::path(missionController, true), start, end, manager->planning()->authorisations());
}

QHash<const GeoZone*, QStringList> GeoZoneMissionCheck::groupContingencies(const GeoZoneMissionPath &path, const GeoZonePathEvaluator::Result &result)
{
    QHash<const GeoZone*, QStringList> labels;
    const QList<GeoZoneMissionPath::ContingencyPath> &contingencyPaths = path.contingencyPaths();
    for (const GeoZonePathEvaluator::ZoneResult &zoneResult : result.zones) {
        // The starts of the paths by label, in the order the paths enter the zone
        QStringList order;
        QHash<QString, QList<int>> starts;
        for (const GeoZonePathEvaluator::Crossing &crossing : zoneResult.crossings) {
            if (!crossing.active || (crossing.entry.path < 0) || (crossing.entry.path >= contingencyPaths.count())) {
                continue;
            }
            const GeoZoneMissionPath::ContingencyPath &contingency = contingencyPaths[crossing.entry.path];
            if (!starts.contains(contingency.path.label)) {
                order.append(contingency.path.label);
            }
            starts[contingency.path.label].append(contingency.start);
        }
        QStringList zoneLabels;
        for (const QString &label : std::as_const(order)) {
            zoneLabels.append(tr("%1 from %2").arg(label, path.startsText(starts.value(label))));
        }
        if (!zoneLabels.isEmpty()) {
            labels.insert(zoneResult.zone, zoneLabels);
        }
    }
    return labels;
}

QList<GeoZoneMissionCheck::Entry> GeoZoneMissionCheck::classify(const Evaluation &evaluation, const GeoZoneAuthorisations *authorisations,
                                                                 GeoFenceController *geoFenceController)
{
    // Zones authorised during the whole flight are listed for information
    const GeoZonePolicy policy = GeoZonePolicy::mission(evaluation.start, evaluation.end).excludingAuthorised(authorisations);
    // Vehicles which support a fence can be stopped at prohibited zones by it
    const bool fenced = geoFenceController && geoFenceController->supported();
    QList<Entry> entries;
    for (const GeoZone *zone : evaluation.zones) {
        Kinds kinds;
        QString authorisationNote;
        // Zones which aren't active while the mission flies through them don't apply
        // Authorisations limiting the area or altitude cover the zone when the flight stays within them
        const GeoZoneAuthorisation *const covering = policy.authorisation(zone);
        const QString scopeIssue = evaluation.scopeIssues.value(zone, tr("the flight wasn't checked against the authorisation's scope yet"));
        if (evaluation.inactiveZones.contains(zone)) {
            kinds = Inactive;
        } else if (covering && (!covering->limitsPath() || scopeIssue.isEmpty())) {
            kinds = Authorised;
        } else if (GeoZonePolicy::isRestricted(zone->restriction())) {
            const bool unfenced = fenced && (zone->restriction() == GeoZone::Prohibited) && !fenceExcludes(geoFenceController, zone);
            kinds = unfenced ? (Crossed | Unfenced) : Kinds(Crossed);
            // A permit for another time or another part of the zone still leaves the zone to warn about
            if (covering) {
                authorisationNote = scopeIssue;
            } else if (const GeoZoneAuthorisation *const authorisation = authorisations ? authorisations->find(zone) : nullptr) {
                const auto [start, end] = policy.authorisationPeriod();
                authorisationNote = GeoZoneAuthorisations::uncoveredText(*authorisation, start, end);
            }
        } else {
            kinds = (zone->restriction() == GeoZone::USpace) ? USpace : Permitted;
        }
        Entry entry{ zone, kinds, evaluation.pathLabels.value(zone), evaluation.approximations.value(zone), authorisationNote };
        // The authority to notify before flights in the zone, unless notified for the flight
        if (!kinds.testFlag(Inactive)) {
            const GeoZoneAuthorisation *const notification = authorisations ? authorisations->findNotification(zone) : nullptr;
            const auto [start, end] = policy.authorisationPeriod();
            if (!notification || !notification->covers(start, end)) {
                entry.notificationNote = GeoZoneAuthorisations::notificationText(zone, evaluation.start, GeoZoneEnvironment::currentDateTimeUtc(),
                                                                                 &entry.notificationOverdue);
            }
        }
        entries.append(entry);
    }

    // Restricted zones only the returns from along the mission enter, unless authorised
    for (const GeoZone *zone : evaluation.contingencyZones) {
        const bool entered = evaluation.zones.contains(zone) && !evaluation.inactiveZones.contains(zone);
        if (GeoZonePolicy::isRestricted(zone->restriction()) && !entered && !policy.authorised(zone)) {
            entries.append(Entry{ zone, Contingency, evaluation.contingencyLabels.value(zone) });
        }
    }
    return entries;
}

void GeoZoneMissionCheck::_apply(const Evaluation &evaluation)
{
    const QList<Entry> entries = classify(evaluation, _manager->planning()->authorisations(), _geoFenceController);
    if (entries != _entries) {
        _entries = entries;
        emit zonesChanged();
    }
    _relevantZones = evaluation.relevantZones;
    _area = evaluation.area;
    // Authorisations can be limited to the area of the mission, and zones shown or fenced for the planned flight
    _manager->planning()->setMissionArea(_area.toRectangle());
    _manager->planning()->setPlannedEnd(evaluation.start.isValid() ? evaluation.end : QDateTime());
    _updateAudit();
}

GeoZonePlanAudit::Record GeoZoneMissionCheck::auditRecord() const
{
    return auditRecord(_manager, _relevantZones, _area);
}

GeoZonePlanAudit::Record GeoZoneMissionCheck::auditRecord(const GeoZoneManager *manager, const QList<const GeoZone*> &relevantZones, const GeoZoneBox &area)
{
    return GeoZonePlanAudit::Record::make(relevantZones, GeoZoneSourceVersion::covering(manager->repository()->sourceList(), area),
                                          GeoZoneEnvironment::currentDateTimeUtc());
}

void GeoZoneMissionCheck::acknowledgeAudit()
{
    if (!_auditText.isEmpty()) {
        _manager->planning()->setPlanAudit(auditRecord());
    }
}

QStringList GeoZoneMissionCheck::uploadReasons() const
{
    QStringList reasons;
    // Authorised zones aren't crossed zones
    QList<const GeoZone*> prohibited = zones(Crossed);
    (void) prohibited.removeIf([](const GeoZone *zone) { return zone->restriction() != GeoZone::Prohibited; });
    if (!prohibited.isEmpty()) {
        QString reason = tr("The mission path enters prohibited geozones: %1.").arg(GeoZonePathEvaluator::zoneNames(prohibited, maxListedZones, false));
        const qsizetype unfenced = zones(Unfenced).count();
        if (unfenced > 0) {
            reason += QStringLiteral(" ") + tr("The GeoFence doesn't exclude %n of them, use 'Add Exclusion Fences' above the map so the vehicle stops at them.",
                                               nullptr, static_cast<int>(unfenced));
        }
        for (const Entry &entry : _entries) {
            if (prohibited.contains(entry.zone) && !entry.authorisationNote.isEmpty()) {
                reason += QStringLiteral(" ") + tr("%1: %2.").arg(entry.zone->displayName(), entry.authorisationNote);
            }
        }
        reasons.append(reason);
    }
    // Zones of expired data still apply, but current data may have more or other zones
    if (_manager->repository()->dataInvalid() && missionController() && missionController()->containsItems()) {
        reasons.append(tr("The geozone data is outdated and treated as invalid, zones may be missing or have changed:\n%1")
                           .arg(_manager->repository()->validityWarning()));
    }
    // Fences from zones which changed since, which the vehicle would enforce as they were
    if (_geoFenceController && !_manager->repository()->busy()) {
        const QString stale = GeoZoneFenceTags::staleText(GeoZoneFenceTags::staleZones(_geoFenceController, _manager->catalog().zones(),
                                                                                       GeoZoneEnvironment::currentDateTimeUtc()));
        if (!stale.isEmpty()) {
            reasons.append(stale);
        }
    }
    if (!_auditText.isEmpty()) {
        QStringList lines = { _auditText };
        lines.append(_auditDetails.first((std::min)(_auditDetails.count(), static_cast<qsizetype>(maxListedChanges))));
        if (_auditDetails.count() > maxListedChanges) {
            lines.append(tr("%n more change(s).", nullptr, static_cast<int>(_auditDetails.count() - maxListedChanges)));
        }
        reasons.append(lines.join(QStringLiteral("\n")));
    }
    return reasons;
}

void GeoZoneMissionCheck::_updateAudit()
{
    QString text;
    QStringList details;
    const std::optional<GeoZonePlanAudit::Record> &record = _manager->planning()->planAudit();
    if (record) {
        // Sources still loading would look removed
        if (_manager->repository()->busy()) {
            return;
        }
        const GeoZonePlanAudit::Changes changes = GeoZonePlanAudit::compare(*record, _relevantZones, _manager->catalog(),
                                                                            GeoZoneSourceVersion::covering(_manager->repository()->sourceList(), GeoZoneBox()));
        text = changes.summary();
        details = changes.details();
    }
    if ((text != _auditText) || (details != _auditDetails)) {
        _auditText = text;
        _auditDetails = details;
        emit auditChanged();
    }
}
