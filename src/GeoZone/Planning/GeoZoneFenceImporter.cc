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
#include "GeoZone.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneFenceTags.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionPath.h"
#include "GeoZoneMissionWorker.h"
#include "MissionController.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"


#include <algorithm>
#include <cmath>

GeoZoneFenceImporter::GeoZoneFenceImporter(QObject *parent)
    : QObject(parent)
    , _worker(new GeoZoneMissionWorker(0, this))
    , _choices(new GeoZoneFenceChoiceModel(this))
    , _upload(new GeoZoneFenceUpload(this))
{
    (void) connect(GeoZoneManager::instance(), &GeoZoneManager::zonesChanged, this, [this]() {
        // The zones of the last selection may be gone
        _worker->invalidate();
        _setBusy(false);
        _zones.clear();
        _prepared = Prepared();
        _selection = GeoZoneFences::Selection();
        _selectedCount = 0;
        _choices->setRows({});
        emit selectionChanged();
    });
}

void GeoZoneFenceImporter::setGeoFenceController(GeoFenceController *geoFenceController)
{
    if (geoFenceController != _geoFenceController) {
        _geoFenceController = geoFenceController;
        _upload->setGeoFenceController(geoFenceController);
        _connectVehicle();
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
    }
}

GeoZoneFences::Capacity GeoZoneFenceImporter::vehicleCapacity(Vehicle *vehicle)
{
    if (!vehicle || vehicle->isOfflineEditingVehicle()) {
        return GeoZoneFences::Capacity::conservative();
    }
    if (vehicle->capabilitiesKnown() && !(vehicle->capabilityBits() & MAV_PROTOCOL_CAPABILITY_MISSION_FENCE)) {
        return GeoZoneFences::Capacity::unsupported();
    }
    if (vehicle->px4Firmware()) {
        return GeoZoneFences::Capacity::px4();
    }
    if (vehicle->apmFirmware()) {
        int sdCardKB = 0;
        ParameterManager *const parameters = vehicle->parameterManager();
        if (parameters->parametersReady() && parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("BRD_SD_FENCE"))) {
            sdCardKB = parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("BRD_SD_FENCE"))->rawValue().toInt();
        }
        return GeoZoneFences::Capacity::arduPilot(vehicle->multiRotor(), sdCardKB);
    }
    return GeoZoneFences::Capacity::conservative();
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

    QDateTime activeFrom;
    QDateTime activeTo;
    if (activeWithinHours >= 0) {
        activeFrom = GeoZoneEnvironment::currentDateTimeUtc();
        if (activeWithinHours > 0) {
            activeTo = activeFrom.addSecs(activeWithinHours * 3600);
        }
    }

    _query = AreaQuery{ GeoZoneBox(area), GeoZonePolicy(restrictionList).activeDuring(activeFrom, activeTo) };
    _zones.clear();
    _setSimplifyTolerances(maxSimplifyTolerance);
    _prepareSelection(false);
}

void GeoZoneFenceImporter::select(const QList<const GeoZone*> &zones, double maxSimplifyTolerance)
{
    _query.reset();
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
        const GeoZoneMissionPath missionPath = GeoZoneMissionPath::fromMission(missionController);
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

    Prepared prepared{ input.catalog, GeoZoneFences::prepare(GeoZoneFences::candidates(zones, evaluation, input.lines), input.simplifyTolerances, input.circlesAsPolygons) };
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

    QList<GeoZoneFenceChoiceModel::Row> rows;
    _selectedCount = 0;
    int leftOutListed = 0;
    for (const GeoZoneFences::Choice &choice : std::as_const(_selection.choices)) {
        if (choice.status == GeoZoneFences::Status::Selected) {
            _selectedCount++;
        } else if (leftOutListed++ >= maxListedChoices) {
            break;
        }
        rows.append(_row(choice));
    }
    // In priority order rather than selected first, so that the row of a zone changing lists is updated in place
    std::sort(rows.begin(), rows.end(), [this](const GeoZoneFenceChoiceModel::Row &a, const GeoZoneFenceChoiceModel::Row &b) {
        return _prepared.indexes.value(a.key) < _prepared.indexes.value(b.key);
    });
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
        const QString name = choice.candidate.zone->displayName();
        for (const QList<QGeoCoordinate> &polygon : choice.fences.polygons) {
            fences.polygons.append(polygon);
            fences.polygonNames.append(name);
        }
        for (const QGeoCircle &circle : choice.fences.circles) {
            fences.circles.append(circle);
            fences.circleNames.append(name);
        }
    }
    const int added = _addFences(fences);
    qCDebug(GeoZoneManagerLog) << "applySelection fences" << added;
    return added;
}

QString GeoZoneFenceImporter::_capacityNote() const
{
    const Vehicle *const vehicle = _vehicle();
    if (!_capacity.exclusions) {
        return tr("The vehicle doesn't support fences through the mission protocol, like ArduPilot before 4.0.");
    }
    if (!vehicle || vehicle->isOfflineEditingVehicle() || (_geoFenceController->masterController() && _geoFenceController->masterController()->offline())) {
        return tr("No vehicle connected: limits PX4 and ArduPilot copters both meet, with circles as polygons.");
    }
    if (vehicle->px4Firmware()) {
        return tr("PX4 stores %1 fence points.").arg(GeoZoneFences::Capacity::px4MaxItems);
    }
    if (vehicle->apmFirmware()) {
        if (!_capacity.estimated) {
            return tr("ArduPilot fence stored on the SD card (BRD_SD_FENCE).");
        }
        return tr("Estimated for ArduPilot flight controllers with 16 KB storage, others store fewer points. A rejected upload is reported.");
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
        text = tr("Vehicle fence: %1 of %n points", nullptr, _capacity.maxItems).arg(usage.items);
        if (_capacity.storageBytes > 0) {
            text += tr(", %1 of %n bytes", nullptr, _capacity.storageBytes).arg(usage.items > 0 ? usage.bytes + GeoZoneFences::Capacity::arduPilotFixedBytes : 0);
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
    row.restrictionText = zone->restrictionText();
    row.proximityText = proximity;
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
        const QString name = zone->displayName();
        const GeoZoneFences::ZoneFences zoneShapes = GeoZoneFences::zoneFences(zone, simplifyTolerance);
        for (const QList<QGeoCoordinate> &vertices : zoneShapes.polygons) {
            if (polygonSet.insert(vertices)) {
                fences.polygons.append(vertices);
                fences.polygonNames.append(name);
            }
        }
        for (const QGeoCircle &geoCircle : zoneShapes.circles) {
            if (circleSet.insert(geoCircle)) {
                fences.circles.append(geoCircle);
                fences.circleNames.append(name);
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
        GeoZoneFenceTag *const tag = GeoZoneFenceTag::of(polygon);
        tag->setName(fences.polygonNames[i]);
        tag->setGeoZone(true);
        newPolygons.append(polygon);
    }
    QList<QObject*> newCircles;
    for (qsizetype i = 0; i < fences.circles.count(); i++) {
        QGCFenceCircle *const circle = new QGCFenceCircle(fences.circles[i].center(), fences.circles[i].radius(), false /* inclusion */, _geoFenceController);
        GeoZoneFenceTag *const tag = GeoZoneFenceTag::of(circle);
        tag->setName(fences.circleNames[i]);
        tag->setGeoZone(true);
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
