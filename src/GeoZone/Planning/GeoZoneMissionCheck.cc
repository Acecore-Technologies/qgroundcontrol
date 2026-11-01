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
#include "GeoZonePathEvaluator.h"
#include "GeoZonePlanning.h"
#include "GeoZoneSource.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "SimpleMissionItem.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFutureWatcher>

#include <algorithm>
#include <cmath>

GeoZoneMissionCheck::GeoZoneMissionCheck(QObject *parent)
    : GeoZoneMissionCheck(GeoZoneManager::instance(), parent)
{
}

GeoZoneMissionCheck::GeoZoneMissionCheck(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
{
    _updateTimer.setSingleShot(true);
    _updateTimer.setInterval(_updateDelayMSecs);
    (void) connect(&_updateTimer, &QTimer::timeout, this, &GeoZoneMissionCheck::_updateConcurrently);

    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMissionCheck::_clearZones);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
    (void) connect(_manager->planning(), &GeoZonePlanning::plannedStartChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
    (void) connect(_manager->planning()->authorisations(), &GeoZoneAuthorisations::changed, this, &GeoZoneMissionCheck::_scheduleUpdate);
}

MissionController *GeoZoneMissionCheck::missionController() const
{
    return _missionController;
}

void GeoZoneMissionCheck::setMissionController(MissionController *missionController)
{
    if (missionController == _missionController) {
        return;
    }

    if (_missionController) {
        (void) disconnect(_missionController, nullptr, this, nullptr);
    }
    _missionController = missionController;
    if (_missionController) {
        // Emitted after the flight path or any item coordinate changes
        (void) connect(_missionController, &MissionController::recalcTerrainProfile, this, &GeoZoneMissionCheck::_scheduleUpdate);
        (void) connect(_missionController, &MissionController::visualItemsChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
        (void) connect(_missionController, &MissionController::missionTimeChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
        (void) connect(_missionController, &MissionController::plannedHomePositionChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
        (void) connect(_missionController, &MissionController::visualItemsChanged, this, &GeoZoneMissionCheck::_connectMissionItems);
        // The return altitude is a vehicle parameter
        if (PlanMasterController *const masterController = _missionController->masterController()) {
            (void) connect(masterController, &PlanMasterController::managerVehicleChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
        }
        _connectMissionItems();
    }

    emit missionControllerChanged();
    _scheduleUpdate();
}

void GeoZoneMissionCheck::_connectMissionItems()
{
    QmlObjectListModel *const visualItems = _missionController ? _missionController->visualItems() : nullptr;
    if (!visualItems) {
        return;
    }
    // Loiter radii, commands and transitions change the paths checked besides the flight path
    (void) connect(visualItems, &QmlObjectListModel::countChanged, this, &GeoZoneMissionCheck::_connectMissionItems, Qt::UniqueConnection);
    for (int i = 0; i < visualItems->count(); i++) {
        if (SimpleMissionItem *const item = visualItems->value<SimpleMissionItem*>(i)) {
            (void) connect(item, &SimpleMissionItem::loiterRadiusChanged, this, &GeoZoneMissionCheck::_scheduleUpdate, Qt::UniqueConnection);
            (void) connect(item, &SimpleMissionItem::commandChanged, this, &GeoZoneMissionCheck::_scheduleUpdate, Qt::UniqueConnection);
            (void) connect(item, &VisualMissionItem::currentVTOLModeChanged, this, &GeoZoneMissionCheck::_scheduleUpdate, Qt::UniqueConnection);
        }
    }
}

void GeoZoneMissionCheck::setFlightNow(bool flightNow)
{
    if (flightNow != _flightNow) {
        _flightNow = flightNow;
        emit flightNowChanged();
        _scheduleUpdate();
    }
}

void GeoZoneMissionCheck::setGeoFenceController(GeoFenceController *geoFenceController)
{
    if (geoFenceController == _geoFenceController) {
        return;
    }
    if (_geoFenceController) {
        (void) disconnect(_geoFenceController, nullptr, this, nullptr);
        (void) disconnect(_geoFenceController->polygons(), nullptr, this, nullptr);
        (void) disconnect(_geoFenceController->circles(), nullptr, this, nullptr);
    }
    _geoFenceController = geoFenceController;
    if (_geoFenceController) {
        // Fence edits mark the lists dirty, additions and deletions change their counts
        for (QmlObjectListModel *fences : { _geoFenceController->polygons(), _geoFenceController->circles() }) {
            (void) connect(fences, &QmlObjectListModel::countChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
            (void) connect(fences, &QmlObjectListModel::dirtyChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
        }
        (void) connect(_geoFenceController, &GeoFenceController::supportedChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
    }
    emit geoFenceControllerChanged();
    _scheduleUpdate();
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

void GeoZoneMissionCheck::_scheduleUpdate()
{
    _updateTimer.start();
}

void GeoZoneMissionCheck::_clearZones(GeoZoneSource *source)
{
    _evaluation++;
    // The other zones stay listed until the check runs again
    const auto fromSource = [source](const GeoZone *zone) { return zone->sourceKey() == source->key(); };
    (void) _pathLabels.removeIf([&fromSource](const QHash<const GeoZone*, QStringList>::iterator &it) { return fromSource(it.key()); });
    const qsizetype removed = _crossedZones.removeIf(fromSource) + _uSpaceZones.removeIf(fromSource) + _permittedZones.removeIf(fromSource) +
                              _authorisedZones.removeIf(fromSource) +
                              _unfencedZones.removeIf(fromSource);
    if (removed > 0) {
        emit crossedZonesChanged();
    }
}

std::pair<QDateTime, QDateTime> GeoZoneMissionCheck::_flightTimes() const
{
    if (_flightNow) {
        const QDateTime start = GeoZoneEnvironment::currentDateTimeUtc();
        return { start, start.addSecs(std::llround((std::max)(_missionController ? _missionController->missionTime() : 0.0, 0.0))) };
    }
    return plannedFlightTimes(_manager, _missionController);
}

void GeoZoneMissionCheck::update()
{
    _updateTimer.stop();
    _evaluation++;
    const auto [start, end] = _flightTimes();
    _apply(evaluate(_manager->catalog(), GeoZoneMissionPath::fromMission(_missionController), start, end));
}

void GeoZoneMissionCheck::_updateConcurrently()
{
    const quint64 evaluation = ++_evaluation;
    const GeoZoneCatalog catalog = _manager->catalog();
    const GeoZoneMissionPath path = GeoZoneMissionPath::fromMission(_missionController);
    const auto [start, end] = _flightTimes();
    auto *const watcher = new QFutureWatcher<Evaluation>(this);
    (void) connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, evaluation]() {
        watcher->deleteLater();
        // Zones of an outdated evaluation may be gone
        if (evaluation == _evaluation) {
            _apply(watcher->result());
        }
    });
    watcher->setFuture(QtConcurrent::run([catalog, path, start, end]() { return evaluate(catalog, path, start, end); }));
}

GeoZoneMissionCheck::Evaluation GeoZoneMissionCheck::evaluate(const GeoZoneCatalog &catalog, const GeoZoneMissionPath &path, const QDateTime &start,
                                                              const QDateTime &end)
{
    const GeoZonePathEvaluator evaluator(catalog, GeoZonePolicy::mission(start, end), start);
    Evaluation evaluation;
    evaluation.start = start;
    evaluation.end = end;
    for (const GeoZonePathEvaluator::ZoneResult &result : evaluator.evaluate(path.evaluatorPaths(start, end), path.areas()).zones) {
        if (!result.entered()) {
            continue;
        }
        evaluation.zones.append(result.zone);
        if (!result.active()) {
            evaluation.inactiveZones.append(result.zone);
        } else if (!result.labels.isEmpty()) {
            evaluation.pathLabels.insert(result.zone, result.labels);
        }
    }
    GeoZonePathEvaluator::sortBySeverity(evaluation.zones);
    return evaluation;
}

void GeoZoneMissionCheck::_apply(const Evaluation &evaluation)
{
    QList<const GeoZone*> zones = evaluation.zones;
    // Zones which aren't active while the mission flies through them don't apply
    const int inactiveZoneCount = static_cast<int>(zones.removeIf([&evaluation](const GeoZone *zone) { return evaluation.inactiveZones.contains(zone); }));
    // Zones authorised during the whole flight are listed for information
    const GeoZonePolicy policy = GeoZonePolicy::mission(evaluation.start, evaluation.end).excludingAuthorised(_manager->planning()->authorisations());

    QList<const GeoZone*> authorised;
    for (const GeoZone *zone : std::as_const(zones)) {
        if (policy.authorised(zone)) {
            authorised.append(zone);
        }
    }
    (void) zones.removeIf([&policy](const GeoZone *zone) { return policy.authorised(zone); });

    QList<const GeoZone*> crossed;
    QList<const GeoZone*> uSpace;
    QList<const GeoZone*> permitted;
    for (const GeoZone *zone : std::as_const(zones)) {
        if (GeoZonePolicy::isRestricted(zone->restriction())) {
            crossed.append(zone);
        } else if (zone->restriction() == GeoZone::USpace) {
            uSpace.append(zone);
        } else {
            permitted.append(zone);
        }
    }

    // Vehicles which support a fence can be stopped at prohibited zones by it
    QList<const GeoZone*> unfenced;
    if (_geoFenceController && _geoFenceController->supported()) {
        for (const GeoZone *zone : std::as_const(crossed)) {
            if ((zone->restriction() == GeoZone::Prohibited) && !fenceExcludes(_geoFenceController, zone)) {
                unfenced.append(zone);
            }
        }
    }

    if ((crossed != _crossedZones) || (uSpace != _uSpaceZones) || (permitted != _permittedZones) || (authorised != _authorisedZones) || (inactiveZoneCount != _inactiveZoneCount) ||
        (unfenced != _unfencedZones) || (evaluation.pathLabels != _pathLabels)) {
        _pathLabels = evaluation.pathLabels;
        _unfencedZones = unfenced;
        _crossedZones = crossed;
        _uSpaceZones = uSpace;
        _permittedZones = permitted;
        _authorisedZones = authorised;
        _inactiveZoneCount = inactiveZoneCount;
        emit crossedZonesChanged();
    }
}
