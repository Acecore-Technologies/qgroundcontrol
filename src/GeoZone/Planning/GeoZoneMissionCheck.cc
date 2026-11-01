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
#include "GeoZoneManager.h"
#include "GeoZoneMissionWorker.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZonePlanning.h"
#include "GeoZoneSource.h"
#include "GeoZoneSourceRepository.h"
#include "GeoZoneSourceVersion.h"
#include "MissionController.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "RallyPoint.h"
#include "RallyPointController.h"
#include "SimpleMissionItem.h"
#include "Vehicle.h"

#include <algorithm>
#include <cmath>

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
    (void) connect(_manager, &GeoZoneManager::busyChanged, this, [this](bool busy) {
        if (!busy) {
            _worker->schedule();
        }
    });
    // The mission no longer makes the area of interest
    (void) connect(this, &QObject::destroyed, _manager, [manager = _manager, owner = static_cast<const QObject*>(this)]() {
        manager->setMissionArea(owner, QGeoRectangle());
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
        if ((check->_manager == manager) && (check->_worker->missionController() == missionController) && !check->_flightNow && check->settled()) {
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
    if (!_vehicle) {
        return;
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

void GeoZoneMissionCheck::setFlightNow(bool flightNow)
{
    if (flightNow != _flightNow) {
        _flightNow = flightNow;
        emit flightNowChanged();
        _worker->schedule();
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

QString GeoZoneMissionCheck::fenceText() const
{
    return _unfencedZones.isEmpty() ? QString() : tr("The GeoFence doesn't exclude prohibited zones on the path: %1").arg(GeoZonePathEvaluator::zoneNames(_unfencedZones, maxListedZones, false));
}

bool GeoZoneMissionCheck::fenceExcludes(GeoFenceController *geoFenceController, const GeoZone *zone)
{
    return GeoZoneFences::fencesCover(GeoZoneFenceImporter::exclusionFences(geoFenceController), zone);
}

QString GeoZoneMissionCheck::warningText() const
{
    if (_crossedZones.isEmpty()) {
        return QString();
    }
    QStringList lines = { tr("The mission path enters restricted geozones: %1").arg(GeoZonePathEvaluator::zoneNames(_crossedZones, maxListedZones, true)) };
    // Zones entered besides the drawn flight path, like by a loiter circle or the return to launch
    QStringList labels;
    QHash<QString, QList<const GeoZone*>> labelZones;
    for (const GeoZone *zone : _crossedZones) {
        for (const QString &label : _pathLabels.value(zone)) {
            if (!labels.contains(label)) {
                labels.append(label);
            }
            labelZones[label].append(zone);
        }
    }
    for (const QString &label : std::as_const(labels)) {
        lines.append(tr("Only %1 enters %2.").arg(label, GeoZonePathEvaluator::zoneNames(labelZones.value(label), maxListedZones, false)));
    }
    GeoZonePathEvaluator::Approximations approximations;
    for (const GeoZone *zone : _crossedZones) {
        approximations |= _approximations.value(zone);
    }
    if (approximations) {
        lines.append(tr("Approximate: %1.").arg(GeoZonePathEvaluator::approximationTexts(approximations).join(QStringLiteral("; "))));
    }
    return lines.join(QStringLiteral("\n"));
}

QString GeoZoneMissionCheck::contingencyText() const
{
    // A line per contingency path and the items it starts from, with the zones it enters
    QStringList labels;
    QHash<QString, QList<const GeoZone*>> labelZones;
    for (const GeoZone *zone : _contingencyZones) {
        for (const QString &label : _contingencyLabels.value(zone)) {
            if (!labels.contains(label)) {
                labels.append(label);
            }
            labelZones[label].append(zone);
        }
    }
    QStringList lines;
    for (const QString &label : std::as_const(labels)) {
        if (lines.count() == maxListedZones) {
            lines.append(tr("%n more contingency path(s) enter restricted geozones.", nullptr, static_cast<int>(labels.count() - maxListedZones)));
            break;
        }
        QString line = tr("%1 would enter %2.").arg(label, GeoZonePathEvaluator::zoneNames(labelZones.value(label), maxListedZones, true));
        line[0] = line[0].toUpper();
        lines.append(line);
    }
    return lines.join(QStringLiteral("\n"));
}

QString GeoZoneMissionCheck::infoText() const
{
    QStringList lines;
    if (!_uSpaceZones.isEmpty()) {
        lines.append(tr("U-space airspace: %1. A flight authorisation from a U-space service provider is required.").arg(GeoZonePathEvaluator::zoneNames(_uSpaceZones, maxListedZones, false)));
    }
    if (!_authorisedZones.isEmpty()) {
        lines.append(tr("Authorised: %1.").arg(GeoZonePathEvaluator::zoneNames(_authorisedZones, maxListedZones, false)));
    }
    if (!_permittedZones.isEmpty()) {
        lines.append(tr("Areas without restriction: %1. Check their conditions.").arg(GeoZonePathEvaluator::zoneNames(_permittedZones, maxListedZones, false)));
    }
    if ((_inactiveZoneCount > 0) && !_flightNow) {
        lines.append(tr("%n zone(s) on the path are not active at the planned flight time.", nullptr, _inactiveZoneCount));
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
    for (const GeoZone *zone : _crossedZones) {
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
    (void) _pathLabels.removeIf([&fromSource](const QHash<const GeoZone*, QStringList>::iterator &it) { return fromSource(it.key()); });
    (void) _approximations.removeIf([&fromSource](const QHash<const GeoZone*, GeoZonePathEvaluator::Approximations>::iterator &it) { return fromSource(it.key()); });
    (void) _contingencyLabels.removeIf([&fromSource](const QHash<const GeoZone*, QStringList>::iterator &it) { return fromSource(it.key()); });
    (void) _relevantZones.removeIf(fromSource);
    const qsizetype removed = _crossedZones.removeIf(fromSource) + _uSpaceZones.removeIf(fromSource) + _permittedZones.removeIf(fromSource) +
                              _authorisedZones.removeIf(fromSource) +
                              _unfencedZones.removeIf(fromSource) + _contingencyZones.removeIf(fromSource);
    if (removed > 0) {
        emit crossedZonesChanged();
    }
}

void GeoZoneMissionCheck::_clearResults()
{
    _relevantZones.clear();
    _area = GeoZoneBox();
    const bool listed = !_crossedZones.isEmpty() || !_uSpaceZones.isEmpty() || !_permittedZones.isEmpty() || !_authorisedZones.isEmpty() ||
                        !_unfencedZones.isEmpty() || !_contingencyZones.isEmpty() || !_pathLabels.isEmpty() || !_contingencyLabels.isEmpty() ||
                        (_inactiveZoneCount != 0);
    _crossedZones.clear();
    _uSpaceZones.clear();
    _permittedZones.clear();
    _authorisedZones.clear();
    _unfencedZones.clear();
    _contingencyZones.clear();
    _pathLabels.clear();
    _contingencyLabels.clear();
    _inactiveZoneCount = 0;
    if (listed) {
        emit crossedZonesChanged();
    }
    if (!_auditText.isEmpty() || !_auditDetails.isEmpty()) {
        _auditText.clear();
        _auditDetails.clear();
        emit auditChanged();
    }
}

std::pair<QDateTime, QDateTime> GeoZoneMissionCheck::_flightTimes() const
{
    if (_flightNow) {
        const QDateTime start = GeoZoneEnvironment::currentDateTimeUtc();
        MissionController *const missionController = _worker->missionController();
        return { start, start.addSecs(std::llround((std::max)(missionController ? missionController->missionTime() : 0.0, 0.0))) };
    }
    return plannedFlightTimes(_manager, _worker->missionController());
}

void GeoZoneMissionCheck::update()
{
    _worker->runNow([this]() {
        const auto [start, end] = _flightTimes();
        const GeoZoneMissionPath path = GeoZoneMissionPath::fromMission(_worker->missionController(), true);
        _manager->setMissionArea(this, missionArea(path));
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
    const GeoZoneMissionPath path = GeoZoneMissionPath::fromMission(_worker->missionController(), true);
    // The services taking the area of interest download the zones around the mission
    _manager->setMissionArea(this, missionArea(path));
    const auto [start, end] = _flightTimes();
    // Zones of an outdated evaluation may be gone
    _worker->run([catalog, path, start, end]() { return evaluate(catalog, path, start, end); },
                 [this](const Evaluation &evaluation) { _apply(evaluation); });
}

GeoZoneMissionCheck::Evaluation GeoZoneMissionCheck::evaluate(const GeoZoneCatalog &catalog, const GeoZoneMissionPath &path, const QDateTime &start,
                                                              const QDateTime &end)
{
    // DO_JUMP items may repeat parts of the mission past its estimated end
    const QDateTime flightEnd = path.flightEnd(start, end);
    const GeoZonePathEvaluator evaluator(catalog, GeoZonePolicy::mission(start, flightEnd), start);
    Evaluation evaluation;
    evaluation.start = start;
    evaluation.end = flightEnd;
    evaluation.area = path.box();
    for (const GeoZonePathEvaluator::ZoneResult &result : evaluator.evaluate(path.evaluatorPaths(start, end), path.areas()).zones) {
        if (!result.entered()) {
            continue;
        }
        evaluation.zones.append(result.zone);
        evaluation.relevantZones.append(result.zone);
        if (const GeoZonePathEvaluator::Approximations approximations = result.approximations()) {
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
    return evaluate(manager->catalog(), GeoZoneMissionPath::fromMission(missionController, true), start, end);
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

GeoZoneMissionCheck::Classification GeoZoneMissionCheck::classify(const Evaluation &evaluation, const GeoZoneAuthorisations *authorisations)
{
    Classification classification;
    QList<const GeoZone*> zones = evaluation.zones;
    // Zones which aren't active while the mission flies through them don't apply
    classification.inactiveZoneCount = static_cast<int>(zones.removeIf([&evaluation](const GeoZone *zone) { return evaluation.inactiveZones.contains(zone); }));
    // Zones authorised during the whole flight are listed for information
    const GeoZonePolicy policy = GeoZonePolicy::mission(evaluation.start, evaluation.end).excludingAuthorised(authorisations);

    // Restricted zones only the returns from along the mission enter, unless authorised
    for (const GeoZone *zone : evaluation.contingencyZones) {
        if (GeoZonePolicy::isRestricted(zone->restriction()) && !zones.contains(zone) && !policy.authorised(zone)) {
            classification.contingencyZones.append(zone);
            classification.contingencyLabels.insert(zone, evaluation.contingencyLabels.value(zone));
        }
    }

    for (const GeoZone *zone : std::as_const(zones)) {
        if (policy.authorised(zone)) {
            classification.authorisedZones.append(zone);
        } else if (GeoZonePolicy::isRestricted(zone->restriction())) {
            classification.crossedZones.append(zone);
        } else if (zone->restriction() == GeoZone::USpace) {
            classification.uSpaceZones.append(zone);
        } else {
            classification.permittedZones.append(zone);
        }
    }
    return classification;
}

void GeoZoneMissionCheck::_apply(const Evaluation &evaluation)
{
    const Classification classification = classify(evaluation, _manager->planning()->authorisations());

    // Vehicles which support a fence can be stopped at prohibited zones by it
    QList<const GeoZone*> unfenced;
    if (_geoFenceController && _geoFenceController->supported()) {
        for (const GeoZone *zone : classification.crossedZones) {
            if ((zone->restriction() == GeoZone::Prohibited) && !fenceExcludes(_geoFenceController, zone)) {
                unfenced.append(zone);
            }
        }
    }

    if ((classification.crossedZones != _crossedZones) || (classification.uSpaceZones != _uSpaceZones) || (classification.permittedZones != _permittedZones) ||
        (classification.authorisedZones != _authorisedZones) || (classification.inactiveZoneCount != _inactiveZoneCount) || (unfenced != _unfencedZones) ||
        (evaluation.pathLabels != _pathLabels) || (classification.contingencyZones != _contingencyZones) || (classification.contingencyLabels != _contingencyLabels) ||
        (evaluation.approximations != _approximations)) {
        _pathLabels = evaluation.pathLabels;
        _approximations = evaluation.approximations;
        _contingencyZones = classification.contingencyZones;
        _contingencyLabels = classification.contingencyLabels;
        _unfencedZones = unfenced;
        _crossedZones = classification.crossedZones;
        _uSpaceZones = classification.uSpaceZones;
        _permittedZones = classification.permittedZones;
        _authorisedZones = classification.authorisedZones;
        _inactiveZoneCount = classification.inactiveZoneCount;
        emit crossedZonesChanged();
    }
    _relevantZones = evaluation.relevantZones;
    _area = evaluation.area;
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
    QList<const GeoZone*> prohibited = _crossedZones;
    (void) prohibited.removeIf([](const GeoZone *zone) { return zone->restriction() != GeoZone::Prohibited; });
    if (!prohibited.isEmpty()) {
        QString reason = tr("The mission path enters prohibited geozones: %1.").arg(GeoZonePathEvaluator::zoneNames(prohibited, maxListedZones, false));
        const auto unfenced = std::count_if(prohibited.cbegin(), prohibited.cend(), [this](const GeoZone *zone) { return _unfencedZones.contains(zone); });
        if (unfenced > 0) {
            reason += QStringLiteral(" ") + tr("The GeoFence doesn't exclude %n of them, use 'Add Exclusion Fences' above the map so the vehicle stops at them.",
                                               nullptr, static_cast<int>(unfenced));
        }
        reasons.append(reason);
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
    if (!_flightNow && record) {
        // Sources still loading would look removed
        if (_manager->busy()) {
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
