/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFenceImporter.h"
#include "GeoZoneInfo.h"
#include "GeoFenceController.h"
#include "GeoFenceManager.h"
#include "GeoZone.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneFenceTags.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionReader.h"
#include "GeoZoneMissionWorker.h"
#include "GeoZonePlanning.h"
#include "GeoZoneSourceRepository.h"
#include "GeoZoneText.h"
#include "MissionController.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QSet>

#include <algorithm>
#include <cmath>

GeoZoneFenceImporter::GeoZoneFenceImporter(QObject *parent)
    : QObject(parent)
    , _worker(new GeoZoneMissionWorker(0, this))
    , _choices(new GeoZoneFenceChoiceModel(this))
    , _upload(new GeoZoneFenceUpload(this))
{
    _fenceNoteTimer.setSingleShot(true);
    _fenceNoteTimer.setInterval(0);
    (void) connect(&_fenceNoteTimer, &QTimer::timeout, this, &GeoZoneFenceImporter::_updateFenceNote);
    (void) connect(_upload, &GeoZoneFenceUpload::stateChanged, this, [this]() {
        if (_upload->state() == GeoZoneFenceUpload::Sending) {
            _rejectionHandled = false;
        }
    });
    (void) connect(GeoZoneManager::instance(), &GeoZoneManager::zonesChanged, this, [this]() {
        // The zones of the last selection may be gone
        _worker->invalidate();
        _setBusy(false);
        _zones.clear();
        _prepared = Prepared();
        _selection = GeoZoneFences::Selection();
        _selectedCount = 0;
        _notFenced.clear();
        _choices->setRows({});
        emit selectionChanged();
        _updateStale();
    });
    // Sources still loading would make the fences of their zones look removed
    (void) connect(GeoZoneManager::instance()->repository(), &GeoZoneSourceRepository::busyChanged, this, [this](bool busy) {
        if (!busy) {
            _updateStale();
        }
    });
}

void GeoZoneFenceImporter::setGeoFenceController(GeoFenceController *geoFenceController)
{
    if (geoFenceController != _geoFenceController) {
        _geoFenceController = geoFenceController;
        _upload->setGeoFenceController(geoFenceController);
        _connectVehicle();
        delete _fenceConnections;
        _fenceConnections = new QObject(this);
        if (_geoFenceController) {
            // Plans set the tags of their fences after loading them
            for (QmlObjectListModel *fences : { _geoFenceController->polygons(), _geoFenceController->circles() }) {
                (void) connect(fences, &QmlObjectListModel::countChanged, _fenceConnections, [this]() {
                    _updateStale();
                    _connectFenceEdits();
                    _updateFenceNote();
                }, Qt::QueuedConnection);
            }
            // Like loading a plan, which sets the import settings after the fences
            (void) connect(_geoFenceController, &GeoFenceController::dirtyChanged, _fenceConnections, [this]() { _fenceNoteTimer.start(); });
        }
        _updateStale();
        _connectFenceEdits();
        _updateFenceNote();
        // The proximity of the zones depends on the plan's mission
        if (_query || !_zones.isEmpty()) {
            _prepareSelection(!_query);
        } else {
            _select();
        }
        emit geoFenceControllerChanged();
    }
}

Vehicle *GeoZoneFenceImporter::_vehicle() const
{
    if (!_geoFenceController || !_geoFenceController->masterController()) {
        return nullptr;
    }
    return _geoFenceController->masterController()->managerVehicle();
}

void GeoZoneFenceImporter::_connectVehicle()
{
    if (_vehicleConnections) {
        // Possibly from one of its connections
        (void) QObject::disconnect(nullptr, nullptr, _vehicleConnections, nullptr);
        _vehicleConnections->deleteLater();
    }
    _vehicleConnections = new QObject(this);
    if (!_geoFenceController || !_geoFenceController->masterController()) {
        return;
    }
    PlanMasterController *const masterController = _geoFenceController->masterController();
    (void) connect(masterController, &PlanMasterController::managerVehicleChanged, _vehicleConnections, [this]() {
        _connectVehicle();
        _updateCapacity();
    }, Qt::QueuedConnection);
    if (Vehicle *const vehicle = _vehicle()) {
        // The capacity and warnings depend on the parameters
        (void) connect(vehicle->parameterManager(), &ParameterManager::parametersReadyChanged, _vehicleConnections, [this]() { _updateCapacity(); });
        // ... and for PX4 on the firmware version
        (void) connect(vehicle, &Vehicle::firmwareVersionChanged, _vehicleConnections, [this]() { _updateCapacity(); });
        // The fence the vehicle stores and its record change with its transfers, once those following them are done
        GeoFenceManager *const manager = vehicle->geoFenceManager();
        const auto transferred = [this]() { _fenceNoteTimer.start(); };
        (void) connect(manager, &GeoFenceManager::sendComplete, _vehicleConnections, transferred);
        (void) connect(manager, &GeoFenceManager::loadComplete, _vehicleConnections, transferred);
        (void) connect(manager, &GeoFenceManager::removeAllComplete, _vehicleConnections, transferred);
    }
    _fenceNoteTimer.start();
}

namespace {

/// The capacities lowered after a rejected fence, of the vehicles connected
QHash<const Vehicle*, GeoZoneFences::Capacity> &_reducedCapacities()
{
    static QHash<const Vehicle*, GeoZoneFences::Capacity> capacities;
    return capacities;
}

GeoZoneFences::Capacity _firmwareCapacity(Vehicle *vehicle)
{
    if (vehicle->px4Firmware()) {
        // 64 items from v1.15, 63 before
        return GeoZoneFences::Capacity::px4(vehicle->firmwareMajorVersion(), vehicle->firmwareMinorVersion());
    }
    if (vehicle->apmFirmware()) {
        int sdCardKB = 0;
        // Flight controllers which can keep the fence on the SD card have 16 KB of storage or more. The others are
        // assumed to have the smallest storage in common use, since the vehicle doesn't report it.
        GeoZoneFences::Capacity::ArduPilotStorage storage = GeoZoneFences::Capacity::ArduPilotStorage::Common;
        ParameterManager *const parameters = vehicle->parameterManager();
        if (parameters->parametersReady() && parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("BRD_SD_FENCE"))) {
            sdCardKB = parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("BRD_SD_FENCE"))->rawValue().toInt();
            storage = GeoZoneFences::Capacity::ArduPilotStorage::Large;
        }
        return GeoZoneFences::Capacity::arduPilot(vehicle->multiRotor(), sdCardKB, storage);
    }
    return GeoZoneFences::Capacity::conservative();
}

} // namespace

GeoZoneFences::Capacity GeoZoneFenceImporter::vehicleCapacity(Vehicle *vehicle)
{
    if (!vehicle || vehicle->isOfflineEditingVehicle()) {
        return GeoZoneFences::Capacity::conservative();
    }
    if (vehicle->capabilitiesKnown() && !(vehicle->capabilityBits() & MAV_PROTOCOL_CAPABILITY_MISSION_FENCE)) {
        return GeoZoneFences::Capacity::unsupported();
    }
    const GeoZoneFences::Capacity capacity = _firmwareCapacity(vehicle);
    const auto reduced = _reducedCapacities().constFind(vehicle);
    if (reduced == _reducedCapacities().cend()) {
        return capacity;
    }
    // What the vehicle rejected stays rejected, whatever is learned about it since
    const bool lower = (reduced->storageBytes > 0) ? ((capacity.storageBytes <= 0) || (reduced->storageBytes < capacity.storageBytes))
                                                   : ((capacity.maxItems <= 0) || (reduced->maxItems < capacity.maxItems));
    return lower ? *reduced : capacity;
}

void GeoZoneFenceImporter::reduceCapacity(Vehicle *vehicle, const GeoZoneFences::Usage &rejected)
{
    if (!vehicle || vehicle->isOfflineEditingVehicle()) {
        return;
    }
    const GeoZoneFences::Capacity reduced = vehicleCapacity(vehicle).reducedBelow(rejected);
    if (!_reducedCapacities().contains(vehicle)) {
        // The address of a destroyed vehicle may be reused by the next one
        (void) connect(vehicle, &QObject::destroyed, vehicle, [vehicle]() { (void) _reducedCapacities().remove(vehicle); });
    }
    _reducedCapacities().insert(vehicle, reduced);
}

void GeoZoneFenceImporter::retryWithFewerZones()
{
    Vehicle *const vehicle = _vehicle();
    if (!vehicle || !_geoFenceController || !_upload->rejectedForRoom()) {
        return;
    }
    // Once for each fence rejected, also when the question which follows is cancelled and asked again
    if (!_rejectionHandled) {
        _rejectionHandled = true;
        reduceCapacity(vehicle, _upload->sentUsage());
    }
    // Selects again for the smaller storage, then asks about the zones no longer fenced
    _updateCapacity();
    requestUpload();
}

GeoZoneFences::Usage GeoZoneFenceImporter::keptUsage(GeoFenceController *geoFenceController)
{
    GeoZoneFences::Usage usage;
    QmlObjectListModel *const polygons = geoFenceController->polygons();
    for (int i = 0; i < polygons->count(); i++) {
        const QGCFencePolygon *const polygon = polygons->value<QGCFencePolygon*>(i);
        if (!GeoZoneFenceTag::isGeoZone(polygon)) {
            usage.addPolygon(polygon->count());
        }
    }
    QmlObjectListModel *const circles = geoFenceController->circles();
    for (int i = 0; i < circles->count(); i++) {
        if (!GeoZoneFenceTag::isGeoZone(circles->get(i))) {
            usage.addCircle();
        }
    }
    if (geoFenceController->breachReturnPoint().isValid()) {
        usage.addReturnPoint();
    }
    return usage;
}

void GeoZoneFenceImporter::updateSelection(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours, double maxSimplifyTolerance)
{
    QList<GeoZone::Restriction> restrictionList;
    for (const QVariant &restriction : restrictions) {
        restrictionList.append(static_cast<GeoZone::Restriction>(restriction.toInt()));
    }

    // Fences can't follow schedules: the zones fenced must be active from the start to the end
    const auto [activeFrom, activeTo] = _activeTimes(activeWithinHours);
    _query = AreaQuery{ GeoZoneBox(area), GeoZonePolicy(restrictionList).activeDuring(activeFrom, activeTo), activeFrom, activeTo };
    _settings = QJsonObject{
        { QStringLiteral("restrictions"), QJsonArray::fromVariantList(restrictions) },
        { QStringLiteral("activeWithinHours"), activeWithinHours },
        { QStringLiteral("maxSimplifyTolerance"), maxSimplifyTolerance },
    };
    if (area.isValid()) {
        _settings.insert(QStringLiteral("area"), QJsonArray{ area.topLeft().latitude(), area.topLeft().longitude(), area.bottomRight().latitude(), area.bottomRight().longitude() });
    }
    _zones.clear();
    _setSimplifyTolerances(maxSimplifyTolerance);
    _prepareSelection(false);
}

std::pair<QDateTime, QDateTime> GeoZoneFenceImporter::_activeTimes(int activeWithinHours) const
{
    if (activeWithinHours == AnyTime) {
        return {};
    }
    const GeoZonePlanning *const planning = GeoZoneManager::instance()->planning();
    if ((activeWithinHours == AtFlightTime) && planning->plannedStart().isValid()) {
        // Widened like the mission check, since the flight may start early or late
        constexpr int tolerance = GeoZonePathEvaluator::defaultScheduleToleranceSecs;
        return { planning->plannedStart().addSecs(-tolerance), planning->plannedEnd().addSecs(tolerance) };
    }
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    if ((activeWithinHours > 0) && (activeWithinHours <= maxActiveHours)) {
        return { now, now.addSecs(qint64{ activeWithinHours } * 3600) };
    }
    // Now, also for the flight time of a plan whose planned start was cleared and for hours out of range: a flight
    // starting now, which takes the mission's estimated time when that is longer. Never any time, which would fence
    // zones while they are inactive.
    QDateTime end = now.addSecs(nowFlightSecs);
    MissionController *const missionController = (_geoFenceController && _geoFenceController->masterController()) ? _geoFenceController->masterController()->missionController() : nullptr;
    if (missionController && (missionController->missionTime() > 0)) {
        const QDateTime missionEnd = now.addSecs(std::llround(missionController->missionTime()));
        // With the repeats of DO_JUMP items
        end = (std::max)(end, GeoZoneMissionReader::path(missionController).flightEnd(now, missionEnd));
    }
    return { now, end };
}

void GeoZoneFenceImporter::select(const QList<const GeoZone*> &zones, double maxSimplifyTolerance)
{
    _query.reset();
    _settings = QJsonObject();
    _zones = zones;
    _setSimplifyTolerances(maxSimplifyTolerance);
    _prepareSelection(true);
}

void GeoZoneFenceImporter::_setSimplifyTolerances(double maxSimplifyTolerance)
{
    _simplifyTolerances.clear();
    for (const double tolerance : _simplifySteps) {
        if (tolerance <= maxSimplifyTolerance) {
            _simplifyTolerances.append(tolerance);
        }
    }
    if (_simplifyTolerances.last() < maxSimplifyTolerance) {
        _simplifyTolerances.append(maxSimplifyTolerance);
    }
}

void GeoZoneFenceImporter::_prepareSelection(bool now)
{
    if (!_geoFenceController) {
        return;
    }
    _capacity = vehicleCapacity(_vehicle());

    PrepareInput input;
    input.catalog = GeoZoneManager::instance()->catalog();
    input.query = _query;
    input.zones = _zones;
    input.now = GeoZoneEnvironment::currentDateTimeUtc();
    input.simplifyTolerances = _simplifyTolerances;
    input.circlesAsPolygons = !_capacity.circles;

    // Proximity to the planned flight: its paths and the areas of its patterns, or the planned home without them
    MissionController *const missionController = _geoFenceController->masterController() ? _geoFenceController->masterController()->missionController() : nullptr;
    if (missionController && (_query || !_zones.isEmpty())) {
        const GeoZoneMissionPath missionPath = GeoZoneMissionReader::path(missionController);
        input.paths = missionPath.evaluatorPaths();
        input.areas = missionPath.areas();
        for (const GeoZoneMissionPath::Segment &segment : missionPath.segments()) {
            if (segment.isValid()) {
                input.lines.append({ segment.from, segment.to });
            }
        }
        for (const GeoZone::Area &area : std::as_const(input.areas)) {
            if (!area.polygon.isEmpty()) {
                input.lines.append(area.polygon);
                input.lines.last().append(area.polygon.first());
            }
        }
        if (input.lines.isEmpty() && missionController->plannedHomePosition().isValid()) {
            input.lines.append({ missionController->plannedHomePosition() });
        }
    }

    const auto apply = [this](const Prepared &prepared) {
        _setBusy(false);
        _applyPrepared(prepared);
    };
    if (now) {
        _worker->runNow([input]() { return _prepare(input); }, apply);
        _setBusy(false);
    } else {
        _worker->run([input]() { return _prepare(input); }, apply);
        _setBusy(true);
    }
}

GeoZoneFenceImporter::Prepared GeoZoneFenceImporter::_prepare(const PrepareInput &input)
{
    const QList<const GeoZone*> zones = input.query ? input.catalog.zonesInArea(input.query->area, input.query->policy) : input.zones;

    QList<GeoZone::Restriction> restrictions;
    for (const GeoZone *zone : zones) {
        if (!restrictions.contains(zone->restriction())) {
            restrictions.append(zone->restriction());
        }
    }
    GeoZonePathEvaluator::Result evaluation;
    if (!zones.isEmpty() && (!input.paths.isEmpty() || !input.areas.isEmpty())) {
        const GeoZonePathEvaluator evaluator(input.catalog, GeoZonePolicy(restrictions), input.now);
        evaluation = evaluator.evaluate(input.paths, input.areas);
    }

    QList<GeoZoneFences::Candidate> candidates = GeoZoneFences::candidates(zones, evaluation, input.lines);
    if (input.query && input.query->activeFrom.isValid()) {
        for (GeoZoneFences::Candidate &candidate : candidates) {
            candidate.partTime = !candidate.zone->isActiveThroughout(input.query->activeFrom, input.query->activeTo);
        }
    }
    Prepared prepared{ input.catalog, GeoZoneFences::prepare(candidates, input.simplifyTolerances, input.circlesAsPolygons) };
    prepared.indexes.reserve(prepared.fences.entries.count());
    for (qsizetype i = 0; i < prepared.fences.entries.count(); i++) {
        prepared.indexes.insert(prepared.fences.entries[i].candidate.zone->key(), i);
    }
    return prepared;
}

void GeoZoneFenceImporter::_applyPrepared(const Prepared &prepared)
{
    _prepared = prepared;
    for (auto it = _included.cbegin(); it != _included.cend(); ++it) {
        const qsizetype index = _prepared.indexes.value(it.key(), -1);
        if (index >= 0) {
            _prepared.fences.entries[index].candidate.excluded = !it.value();
        }
    }
    _select();
}

void GeoZoneFenceImporter::_finishSelection()
{
    if ((_query || !_zones.isEmpty()) && (!_worker->settled() || (_prepared.fences.circlesAsPolygons != !vehicleCapacity(_vehicle()).circles))) {
        _prepareSelection(true);
    }
}

void GeoZoneFenceImporter::_updateCapacity()
{
    // Circles are prepared as polygons for vehicles without them
    if ((_query || !_zones.isEmpty()) && (_prepared.fences.circlesAsPolygons != !vehicleCapacity(_vehicle()).circles)) {
        _prepareSelection(!_query);
    } else {
        _select();
    }
}

void GeoZoneFenceImporter::_select()
{
    if (!_geoFenceController) {
        return;
    }

    _capacity = vehicleCapacity(_vehicle());
    _selection = GeoZoneFences::select(_prepared.fences, _capacity, keptUsage(_geoFenceController));

    _notFenced.clear();
    for (const GeoZoneFences::Choice &choice : _selection.notFenced()) {
        _notFenced.append(GeoZoneFenceTags::NotFenced{ choice.candidate.zone->displayName(), GeoZoneFences::statusText(choice) });
    }

    // All of them, so that each zone left out can be seen, in priority order rather than selected first, so that the
    // row of a zone changing lists is updated in place
    QList<GeoZoneFenceChoiceModel::Row> rows(_selection.choices.count());
    QList<bool> placed(rows.count(), false);
    QList<GeoZoneFenceChoiceModel::Row> sameKey;
    _selectedCount = 0;
    for (const GeoZoneFences::Choice &choice : std::as_const(_selection.choices)) {
        _selectedCount += (choice.status == GeoZoneFences::Status::Selected) ? 1 : 0;
        const GeoZoneFenceChoiceModel::Row row = _row(choice);
        const qsizetype index = _prepared.indexes.value(row.key, -1);
        if ((index >= 0) && (index < rows.count()) && !placed[index]) {
            rows[index] = row;
            placed[index] = true;
        } else {
            sameKey.append(row);
        }
    }
    // Zones sharing a key have one place in the priority order: the others follow the list
    if (!sameKey.isEmpty()) {
        QList<GeoZoneFenceChoiceModel::Row> listed;
        for (qsizetype i = 0; i < rows.count(); i++) {
            if (placed[i]) {
                listed.append(rows[i]);
            }
        }
        rows = listed + sameKey;
    }
    _choices->setRows(rows);
    emit selectionChanged();
}

void GeoZoneFenceImporter::_setBusy(bool busy)
{
    if (busy != _busy) {
        _busy = busy;
        emit busyChanged();
    }
}

void GeoZoneFenceImporter::setZoneIncluded(const QString &key, bool included)
{
    _included.insert(key, included);
    const qsizetype index = _prepared.indexes.value(key, -1);
    if (index >= 0) {
        _prepared.fences.entries[index].candidate.excluded = !included;
    }
    _select();
}

int GeoZoneFenceImporter::applySelection()
{
    if (!_geoFenceController) {
        return 0;
    }
    _finishSelection();
    GeoZoneFenceTags::deleteFences(_geoFenceController, true /* geoZoneOnly */);

    Fences fences;
    for (const GeoZoneFences::Choice &choice : std::as_const(_selection.choices)) {
        if (choice.status != GeoZoneFences::Status::Selected) {
            continue;
        }
        for (const QList<QGeoCoordinate> &polygon : choice.fences.polygons) {
            fences.polygons.append(polygon);
            fences.polygonZones.append(choice.candidate.zone);
        }
        for (const QGeoCircle &circle : choice.fences.circles) {
            fences.circles.append(circle);
            fences.circleZones.append(choice.candidate.zone);
        }
    }
    const int added = _addFences(fences);
    qCDebug(GeoZoneManagerLog) << "applySelection fences" << added;

    // Kept with the plan to select the fences again when their zones change
    QJsonObject settings = _settings;
    if (!settings.isEmpty()) {
        QJsonObject included;
        for (auto it = _included.cbegin(); it != _included.cend(); ++it) {
            included.insert(it.key(), it.value());
        }
        settings.insert(QStringLiteral("included"), included);
    }
    GeoZoneFenceTags::setImportSettings(_geoFenceController, settings);
    // Kept with the fences added, so the plan tells which zones aren't fenced until they change
    GeoZoneFenceTags::setNotFenced(_geoFenceController, _notFenced, static_cast<int>(_notFenced.count()));
    _updateStale();
    _updateFenceNote();
    return added;
}

int GeoZoneFenceImporter::rebuild()
{
    if (!_geoFenceController) {
        return 0;
    }
    const QJsonObject settings = GeoZoneFenceTags::importSettings(_geoFenceController);
    _included.clear();
    if (!settings.contains(QStringLiteral("restrictions"))) {
        // Plans saved before the settings were kept: the zones of their fences still loaded
        QSet<QString> keys;
        for (QmlObjectListModel *fences : { _geoFenceController->polygons(), _geoFenceController->circles() }) {
            for (int i = 0; i < fences->count(); i++) {
                const GeoZoneFenceTag *const tag = GeoZoneFenceTag::find(fences->get(i));
                if (tag && tag->geoZone()) {
                    keys.insert(tag->zoneKey());
                }
            }
        }
        QList<const GeoZone*> zones;
        for (const GeoZone *zone : GeoZoneManager::instance()->catalog().zones()) {
            if (keys.contains(zone->key())) {
                zones.append(zone);
            }
        }
        select(zones, rebuildSimplifyTolerance);
    } else {
        const QJsonArray bounds = settings.value(QStringLiteral("area")).toArray();
        const QGeoRectangle area = (bounds.count() == 4) ? QGeoRectangle(QGeoCoordinate(bounds[0].toDouble(), bounds[1].toDouble()),
                                                                         QGeoCoordinate(bounds[2].toDouble(), bounds[3].toDouble())) : QGeoRectangle();
        const QJsonObject included = settings.value(QStringLiteral("included")).toObject();
        for (auto it = included.constBegin(); it != included.constEnd(); ++it) {
            _included.insert(it.key(), it.value().toBool());
        }
        updateSelection(settings.value(QStringLiteral("restrictions")).toArray().toVariantList(), area,
                        settings.value(QStringLiteral("activeWithinHours")).toInt(Now), settings.value(QStringLiteral("maxSimplifyTolerance")).toDouble());
    }
    return applySelection();
}

void GeoZoneFenceImporter::_updateStale()
{
    GeoZoneManager *const manager = GeoZoneManager::instance();
    if (manager->repository()->busy()) {
        return;
    }
    const QString text = GeoZoneFenceTags::staleText(GeoZoneFenceTags::staleZones(_geoFenceController, manager->catalog().zones(),
                                                                                  GeoZoneEnvironment::currentDateTimeUtc()));
    if (text != _staleText) {
        _staleText = text;
        emit staleChanged();
    }
}

void GeoZoneFenceImporter::_connectFenceEdits()
{
    delete _fenceEditConnections;
    _fenceEditConnections = new QObject(this);
    if (!_geoFenceController) {
        return;
    }
    // Fences from geozones are few, the vehicle stores them
    const auto edited = [this]() { _fenceNoteTimer.start(); };
    QmlObjectListModel *const polygons = _geoFenceController->polygons();
    for (int i = 0; i < polygons->count(); i++) {
        QGCFencePolygon *const polygon = polygons->value<QGCFencePolygon*>(i);
        if (GeoZoneFenceTag::isGeoZone(polygon)) {
            (void) connect(polygon, &QGCMapPolygon::pathChanged, _fenceEditConnections, edited);
        }
    }
    QmlObjectListModel *const circles = _geoFenceController->circles();
    for (int i = 0; i < circles->count(); i++) {
        QGCFenceCircle *const circle = circles->value<QGCFenceCircle*>(i);
        if (GeoZoneFenceTag::isGeoZone(circle)) {
            (void) connect(circle, &QGCMapCircle::centerChanged, _fenceEditConnections, edited);
            (void) connect(circle->radius(), &Fact::rawValueChanged, _fenceEditConnections, edited);
        }
    }
}

void GeoZoneFenceImporter::_updateFenceNote()
{
    int count = 0;
    const QList<GeoZoneFenceTags::NotFenced> zones = GeoZoneFenceTags::notFenced(_geoFenceController, &count);
    QStringList zoneTexts = _zoneTexts(zones);
    if (count > zoneTexts.count()) {
        // Plans saved before all zones were kept
        zoneTexts.append(tr("%n more which this plan doesn't name: add the fences from geozones again to list them", nullptr, count - static_cast<int>(zoneTexts.count())));
    }
    const QString note = GeoZoneFenceTags::notFencedText(zones, count);
    if ((note != _fenceNote) || (count != _fenceNoteCount) || (zoneTexts != _fenceNoteZones)) {
        _fenceNote = note;
        _fenceNoteCount = count;
        _fenceNoteZones = zoneTexts;
        emit fenceNoteChanged();
    }
    _updateVehicleFenceNote();
}

void GeoZoneFenceImporter::_updateVehicleFenceNote()
{
    QString note;
    QStringList zoneTexts;
    Vehicle *const vehicle = _vehicle();
    if (vehicle && !vehicle->isOfflineEditingVehicle()) {
        const std::optional<GeoZoneFenceUpload::Verification> record = GeoZoneFenceUpload::verification(vehicle);
        GeoFenceManager *const manager = vehicle->geoFenceManager();
        // Of the fence the vehicle stores now
        if (record && (record->notFencedCount > 0) &&
            GeoZoneFenceUpload::compareFences(record->fence, GeoZoneFenceUpload::snapshot(manager->polygons(), manager->circles(), manager->breachReturnPoint())).isEmpty()) {
            note = tr("On the vehicle: %1").arg(GeoZoneFenceTags::notFencedText(record->notFenced, record->notFencedCount));
            zoneTexts = _zoneTexts(record->notFenced);
            if (record->notFencedCount > zoneTexts.count()) {
                zoneTexts.append(tr("%n more which weren't recorded by name", nullptr, record->notFencedCount - static_cast<int>(zoneTexts.count())));
            }
        }
    }
    if ((note != _vehicleFenceNote) || (zoneTexts != _vehicleFenceNoteZones)) {
        _vehicleFenceNote = note;
        _vehicleFenceNoteZones = zoneTexts;
        emit vehicleFenceNoteChanged();
    }
}

QStringList GeoZoneFenceImporter::_zoneTexts(const QList<GeoZoneFenceTags::NotFenced> &zones)
{
    QStringList texts;
    texts.reserve(zones.count());
    for (const GeoZoneFenceTags::NotFenced &zone : zones) {
        texts.append(zone.reason.isEmpty() ? zone.name : tr("%1: %2").arg(zone.name, zone.reason));
    }
    return texts;
}

QString GeoZoneFenceImporter::notFencedText() const
{
    return GeoZoneFenceTags::notFencedText(_notFenced, static_cast<int>(_notFenced.count()));
}

QString GeoZoneFenceImporter::_capacityNote() const
{
    const Vehicle *const vehicle = _vehicle();
    if (!_capacity.exclusions) {
        return tr("The vehicle doesn't support fences through the mission protocol, like ArduPilot before 4.0.");
    }
    if (!vehicle || vehicle->isOfflineEditingVehicle() || (_geoFenceController->masterController() && _geoFenceController->masterController()->offline())) {
        return tr("No vehicle connected: estimated limits which PX4 and ArduPilot copters with 8 KB of storage or more both meet, with circles as polygons.");
    }
    const QString retry = tr("The vehicle rejects a fence which doesn't fit: the upload then offers to retry with fewer zones.");
    if (_capacity.reduced) {
        return tr("The vehicle rejected a larger fence for lack of room, so less storage is assumed until it reconnects.") + QLatin1Char(' ') + retry;
    }
    if (vehicle->px4Firmware()) {
        const QString constrained = tr("Flight controllers with constrained memory only store %1, which the vehicle doesn't report.").arg(GeoZoneFences::Capacity::px4ConstrainedMaxItems);
        if (_capacity.estimated) {
            return tr("PX4 stores %1 fence points from v%2.%3 and %4 before. The firmware version isn't known, so %4 are assumed.")
                .arg(GeoZoneFences::Capacity::px4MaxItems).arg(GeoZoneFences::Capacity::px4FullItemsMajor).arg(GeoZoneFences::Capacity::px4FullItemsMinor)
                .arg(GeoZoneFences::Capacity::px4LegacyMaxItems) + QLatin1Char(' ') + constrained + QLatin1Char(' ') + retry;
        }
        return tr("PX4 %1.%2 stores at most %3 fence points, a firmware limit.").arg(vehicle->firmwareMajorVersion()).arg(vehicle->firmwareMinorVersion()).arg(_capacity.maxItems) +
               QLatin1Char(' ') + constrained + QLatin1Char(' ') + retry;
    }
    if (vehicle->apmFirmware()) {
        if (!_capacity.estimated) {
            return tr("ArduPilot fence stored on the SD card (BRD_SD_FENCE), a firmware limit. Without the card the flight controller stores less.") + QLatin1Char(' ') + retry;
        }
        const bool large = _capacity.storageBytes == GeoZoneFences::Capacity::arduPilotBytes(vehicle->multiRotor(), GeoZoneFences::Capacity::ArduPilotStorage::Large);
        return (large ? tr("ArduPilot doesn't report its fence storage: estimated for flight controllers with 16 KB of storage or more, like those which can store the fence on the SD card.")
                      : tr("ArduPilot doesn't report its fence storage: estimated for flight controllers with 8 KB of storage. Those with 16 KB store more, those with 4 KB fewer points.")) +
               QLatin1Char(' ') + retry;
    }
    return tr("Estimated, the autopilot doesn't report its fence storage. Circles are added as polygons.");
}

QString GeoZoneFenceImporter::_parameterWarning() const
{
    Vehicle *const vehicle = _vehicle();
    if (!vehicle || vehicle->isOfflineEditingVehicle() || !vehicle->parameterManager()->parametersReady()) {
        return QString();
    }
    ParameterManager *const parameters = vehicle->parameterManager();
    const auto value = [parameters](const QString &name) -> QVariant {
        if (!parameters->parameterExists(ParameterManager::defaultComponentId, name)) {
            return QVariant();
        }
        return parameters->getParameter(ParameterManager::defaultComponentId, name)->rawValue();
    };

    QStringList warnings;
    if (vehicle->apmFirmware()) {
        const QVariant type = value(QStringLiteral("FENCE_TYPE"));
        if (type.isValid() && !(type.toInt() & 4)) {
            warnings.append(tr("FENCE_TYPE doesn't include polygon fences, so the vehicle ignores exclusion fences."));
        }
        const QVariant enabled = value(QStringLiteral("FENCE_ENABLE"));
        if (enabled.isValid() && (enabled.toInt() == 0)) {
            warnings.append(tr("FENCE_ENABLE is 0, the vehicle only enforces the fence once it is enabled."));
        }
    } else if (vehicle->px4Firmware()) {
        const QVariant action = value(QStringLiteral("GF_ACTION"));
        if (action.isValid() && (action.toInt() == 0)) {
            warnings.append(tr("GF_ACTION is None, the vehicle doesn't act on fence breaches."));
        }
    }
    return warnings.join(QLatin1Char(' '));
}

QVariantMap GeoZoneFenceImporter::capacityInfo() const
{
    if (!_geoFenceController) {
        return QVariantMap();
    }
    const GeoZoneFences::Usage &usage = _selection.usage;
    QString text;
    if (!_capacity.exclusions) {
        text = tr("Vehicle fence: not supported");
    } else if (_capacity.maxItems > 0) {
        // An estimate isn't shown like a limit the vehicle is known to have
        const int usedBytes = (usage.items > 0) ? (usage.bytes + GeoZoneFences::Capacity::arduPilotFixedBytes) : 0;
        if (_capacity.estimated) {
            text = tr("Vehicle fence: %1 of about %n points", nullptr, _capacity.maxItems).arg(usage.items);
            if (_capacity.storageBytes > 0) {
                text += tr(", %1 of about %n bytes", nullptr, _capacity.storageBytes).arg(usedBytes);
            }
            text += tr(" (estimated)");
        } else {
            text = tr("Vehicle fence: %1 of %n points", nullptr, _capacity.maxItems).arg(usage.items);
            if (_capacity.storageBytes > 0) {
                text += tr(", %1 of %n bytes", nullptr, _capacity.storageBytes).arg(usedBytes);
            }
        }
    } else {
        text = tr("Vehicle fence: %n point(s)", nullptr, usage.items);
    }

    return QVariantMap{
        { QStringLiteral("text"),           text },
        { QStringLiteral("note"),           _capacityNote() },
        { QStringLiteral("warning"),        _parameterWarning() },
        { QStringLiteral("usedItems"),      usage.items },
        { QStringLiteral("maxItems"),       _capacity.maxItems },
        { QStringLiteral("usedBytes"),      usage.bytes },
        { QStringLiteral("storageBytes"),   _capacity.storageBytes },
        { QStringLiteral("keptItems"),      _selection.reserved.items },
        { QStringLiteral("circles"),        _capacity.circles },
        { QStringLiteral("supported"),      _capacity.exclusions },
        { QStringLiteral("estimated"),      _capacity.estimated },
        { QStringLiteral("reduced"),        _capacity.reduced },
    };
}

GeoZoneFenceChoiceModel::Row GeoZoneFenceImporter::_row(const GeoZoneFences::Choice &choice) const
{
    const GeoZone *const zone = choice.candidate.zone;
    QString proximity;
    switch (choice.candidate.proximity) {
    case GeoZoneFences::Proximity::Entered:
        proximity = tr("Entered by the flight");
        break;
    case GeoZoneFences::Proximity::Overflown:
        proximity = tr("Flown above or below");
        break;
    case GeoZoneFences::Proximity::Near:
    case GeoZoneFences::Proximity::Far:
        if (std::isfinite(choice.candidate.distance)) {
            proximity = (choice.candidate.distance < 1000) ? tr("%1 m from the flight").arg(qRound(choice.candidate.distance))
                                                           : tr("%1 km from the flight").arg(choice.candidate.distance / 1000, 0, 'f', 1);
        }
        break;
    }

    GeoZoneFenceChoiceModel::Row row;
    row.key = zone->key();
    row.name = zone->displayName();
    row.restriction = static_cast<int>(zone->restriction());
    row.restrictionText = GeoZoneText::restriction(zone->restriction());
    row.proximityText = proximity;
    row.entered = choice.candidate.proximity == GeoZoneFences::Proximity::Entered;
    row.points = choice.usage.items;
    row.simplifyTolerance = choice.simplifyTolerance;
    row.selected = choice.status == GeoZoneFences::Status::Selected;
    row.included = !choice.candidate.excluded;
    row.includable = choice.includable();
    row.reason = GeoZoneFences::statusText(choice);
    return row;
}

namespace {

size_t _polygonHash(const QList<QGeoCoordinate> &polygon)
{
    size_t hash = 0;
    for (const QGeoCoordinate &coordinate : polygon) {
        hash = qHashMulti(hash, coordinate.latitude(), coordinate.longitude());
    }
    return hash;
}

size_t _circleHash(const QGeoCircle &circle)
{
    return qHashMulti(0, circle.center().latitude(), circle.center().longitude(), circle.radius());
}

/// Fences already in the fence or added, found by hash instead of comparing with each
template<typename Shape, size_t (*hashShape)(const Shape &)>
class ShapeSet
{
public:
    /// @return false if an equal shape is in the set
    bool insert(const Shape &shape)
    {
        const size_t hash = hashShape(shape);
        for (auto it = _shapes.constFind(hash); (it != _shapes.cend()) && (it.key() == hash); ++it) {
            if (it.value() == shape) {
                return false;
            }
        }
        _shapes.insert(hash, shape);
        return true;
    }

private:
    QMultiHash<size_t, Shape> _shapes;
};

} // namespace

GeoZoneFenceImporter::Fences GeoZoneFenceImporter::_newFences(const QList<const GeoZone*> &zones, double simplifyTolerance) const
{
    ShapeSet<QList<QGeoCoordinate>, _polygonHash> polygonSet;
    QmlObjectListModel *const polygons = _geoFenceController->polygons();
    for (int i = 0; i < polygons->count(); i++) {
        (void) polygonSet.insert(polygons->value<QGCFencePolygon*>(i)->coordinateList());
    }
    ShapeSet<QGeoCircle, _circleHash> circleSet;
    QmlObjectListModel *const circles = _geoFenceController->circles();
    for (int i = 0; i < circles->count(); i++) {
        QGCFenceCircle *const circle = circles->value<QGCFenceCircle*>(i);
        (void) circleSet.insert(QGeoCircle(circle->center(), circle->radius()->rawValue().toDouble()));
    }

    Fences fences;
    for (const GeoZone *zone : zones) {
        const GeoZoneFences::ZoneFences zoneShapes = GeoZoneFences::zoneFences(zone, simplifyTolerance);
        for (const QList<QGeoCoordinate> &vertices : zoneShapes.polygons) {
            if (polygonSet.insert(vertices)) {
                fences.polygons.append(vertices);
                fences.polygonZones.append(zone);
            }
        }
        for (const QGeoCircle &geoCircle : zoneShapes.circles) {
            if (circleSet.insert(geoCircle)) {
                fences.circles.append(geoCircle);
                fences.circleZones.append(zone);
            }
        }
    }
    return fences;
}

GeoZoneFences::ZoneFences GeoZoneFenceImporter::exclusionFences(GeoFenceController *geoFenceController)
{
    GeoZoneFences::ZoneFences fences;
    QmlObjectListModel *const polygons = geoFenceController->polygons();
    for (int i = 0; i < polygons->count(); i++) {
        const QGCFencePolygon *const polygon = polygons->value<QGCFencePolygon*>(i);
        if (!polygon->inclusion() && (polygon->count() >= 3)) {
            fences.polygons.append(polygon->coordinateList());
        }
    }
    QmlObjectListModel *const circles = geoFenceController->circles();
    for (int i = 0; i < circles->count(); i++) {
        QGCFenceCircle *const circle = circles->value<QGCFenceCircle*>(i);
        if (!circle->inclusion()) {
            fences.circles.append(QGeoCircle(circle->center(), circle->radius()->rawValue().toDouble()));
        }
    }
    return fences;
}

int GeoZoneFenceImporter::addZones(const QVariantList &zones, double simplifyTolerance)
{
    return addExclusions(GeoZoneInfo::fromVariantList(zones), simplifyTolerance);
}

int GeoZoneFenceImporter::addExclusions(const QList<const GeoZone*> &zones, double simplifyTolerance)
{
    if (!_geoFenceController) {
        return 0;
    }
    return _addFences(_newFences(zones, simplifyTolerance));
}

int GeoZoneFenceImporter::_addFences(const Fences &fences)
{
    // Appended in one batch each, so the fence list and map update once
    QList<QObject*> newPolygons;
    for (qsizetype i = 0; i < fences.polygons.count(); i++) {
        QGCFencePolygon *const polygon = new QGCFencePolygon(false /* inclusion */, _geoFenceController);
        polygon->appendVertices(fences.polygons[i]);
        GeoZoneFenceTag::of(polygon)->setZone(fences.polygonZones[i]);
        newPolygons.append(polygon);
    }
    QList<QObject*> newCircles;
    for (qsizetype i = 0; i < fences.circles.count(); i++) {
        QGCFenceCircle *const circle = new QGCFenceCircle(fences.circles[i].center(), fences.circles[i].radius(), false /* inclusion */, _geoFenceController);
        GeoZoneFenceTag::of(circle)->setZone(fences.circleZones[i]);
        newCircles.append(circle);
    }
    if (!newPolygons.isEmpty()) {
        _geoFenceController->polygons()->append(newPolygons);
    }
    if (!newCircles.isEmpty()) {
        _geoFenceController->circles()->append(newCircles);
    }

    const int added = fences.polygons.count() + fences.circles.count();
    if (added > 0) {
        _geoFenceController->clearAllInteractive();
    }

    return added;
}

QString GeoZoneFenceImporter::_notFencedConfirmation() const
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    for (const GeoZoneFenceTags::NotFenced &zone : _notFenced) {
        // Each length first, so that no two lists of zones give the same bytes
        for (const QString &text : { zone.name, zone.reason }) {
            const QByteArray bytes = text.toUtf8();
            hash.addData(QByteArray::number(bytes.size()));
            hash.addData(QByteArrayView(":"));
            hash.addData(bytes);
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

void GeoZoneFenceImporter::_requireUploadConfirmation()
{
    emit uploadConfirmationRequired(_notFencedConfirmation(), notFencedText(), _zoneTexts(_notFenced));
}

void GeoZoneFenceImporter::requestUpload()
{
    if (!_geoFenceController || _upload->busy()) {
        return;
    }
    // The zones of a selection still being prepared aren't known: asking about the previous one would upload them
    // unconfirmed
    _finishSelection();
    if (_notFenced.isEmpty()) {
        uploadToVehicle();
    } else {
        _requireUploadConfirmation();
    }
}

void GeoZoneFenceImporter::confirmUpload(const QString &confirmation)
{
    if (!_geoFenceController || _upload->busy()) {
        return;
    }
    _finishSelection();
    if (!_notFenced.isEmpty() && (confirmation != _notFencedConfirmation())) {
        _requireUploadConfirmation();
        return;
    }
    uploadToVehicle();
}

void GeoZoneFenceImporter::uploadToVehicle()
{
    if (!_geoFenceController || _upload->busy()) {
        return;
    }
    // The upload fails without changing the plan when it can't start
    if (_upload->canUpload() && !_geoFenceController->syncInProgress()) {
        (void) applySelection();
    }
    _upload->start();
}
