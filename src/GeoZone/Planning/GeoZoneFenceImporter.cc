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
    , _upload(new GeoZoneFenceUpload(this))
{
    (void) connect(GeoZoneManager::instance(), &GeoZoneManager::zonesChanged, this, [this]() {
        // The zones of the last selection may be gone
        _zones.clear();
        _selection = GeoZoneFences::Selection();
        emit selectionChanged();
    });
}

void GeoZoneFenceImporter::setGeoFenceController(GeoFenceController *geoFenceController)
{
    if (geoFenceController != _geoFenceController) {
        _geoFenceController = geoFenceController;
        _upload->setGeoFenceController(geoFenceController);
        _connectVehicle();
        _select();
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
        _select();
    }, Qt::QueuedConnection);
    if (Vehicle *const vehicle = _vehicle()) {
        // The capacity and warnings depend on the parameters
        (void) connect(vehicle->parameterManager(), &ParameterManager::parametersReadyChanged, _vehicleConnections, [this]() { _select(); });
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

    select(GeoZoneManager::instance()->catalog().zonesInArea(area, GeoZonePolicy(restrictionList).activeDuring(activeFrom, activeTo)), maxSimplifyTolerance);
}

void GeoZoneFenceImporter::select(const QList<const GeoZone*> &zones, double maxSimplifyTolerance)
{
    _zones = zones;
    _simplifyTolerances.clear();
    for (const double tolerance : _simplifySteps) {
        if (tolerance <= maxSimplifyTolerance) {
            _simplifyTolerances.append(tolerance);
        }
    }
    if (_simplifyTolerances.last() < maxSimplifyTolerance) {
        _simplifyTolerances.append(maxSimplifyTolerance);
    }
    _select();
}

void GeoZoneFenceImporter::_select()
{
    if (!_geoFenceController) {
        return;
    }

    // Proximity to the planned flight: its paths and the areas of its patterns, or the planned home without them
    QList<GeoZonePathEvaluator::Path> paths;
    QList<GeoZone::Area> areas;
    QList<QList<QGeoCoordinate>> lines;
    MissionController *const missionController = _geoFenceController->masterController() ? _geoFenceController->masterController()->missionController() : nullptr;
    if (missionController && !_zones.isEmpty()) {
        const GeoZoneMissionPath missionPath = GeoZoneMissionPath::fromMission(missionController);
        paths = missionPath.evaluatorPaths();
        areas = missionPath.areas();
        for (const GeoZoneMissionPath::Segment &segment : missionPath.segments()) {
            if (segment.isValid()) {
                lines.append({ segment.from, segment.to });
            }
        }
        for (const GeoZone::Area &area : std::as_const(areas)) {
            if (!area.polygon.isEmpty()) {
                lines.append(area.polygon);
                lines.last().append(area.polygon.first());
            }
        }
        if (lines.isEmpty() && missionController->plannedHomePosition().isValid()) {
            lines.append({ missionController->plannedHomePosition() });
        }
    }

    QList<GeoZone::Restriction> restrictions;
    for (const GeoZone *zone : std::as_const(_zones)) {
        if (!restrictions.contains(zone->restriction())) {
            restrictions.append(zone->restriction());
        }
    }
    GeoZonePathEvaluator::Result evaluation;
    if (!paths.isEmpty() || !areas.isEmpty()) {
        const GeoZonePathEvaluator evaluator(GeoZoneManager::instance()->catalog(), GeoZonePolicy(restrictions), GeoZoneEnvironment::currentDateTimeUtc());
        evaluation = evaluator.evaluate(paths, areas);
    }

    QList<GeoZoneFences::Candidate> candidates = GeoZoneFences::candidates(_zones, evaluation, lines);
    for (GeoZoneFences::Candidate &candidate : candidates) {
        const auto included = _included.constFind(candidate.zone->key());
        if (included != _included.cend()) {
            candidate.excluded = !included.value();
        }
    }

    _capacity = vehicleCapacity(_vehicle());
    _selection = GeoZoneFences::select(candidates, _capacity, keptUsage(_geoFenceController), _simplifyTolerances);
    emit selectionChanged();
}

int GeoZoneFenceImporter::selectedCount() const
{
    return static_cast<int>(std::count_if(_selection.choices.cbegin(), _selection.choices.cend(), [](const GeoZoneFences::Choice &choice) {
        return choice.status == GeoZoneFences::Status::Selected;
    }));
}

void GeoZoneFenceImporter::setZoneIncluded(const QString &key, bool included)
{
    _included.insert(key, included);
    _select();
}

int GeoZoneFenceImporter::applySelection()
{
    if (!_geoFenceController) {
        return 0;
    }
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
        text = tr("Vehicle fence: %1 of %2 points").arg(usage.items).arg(_capacity.maxItems);
        if (_capacity.storageBytes > 0) {
            text += tr(", %1 of %2 bytes").arg(usage.items > 0 ? usage.bytes + GeoZoneFences::Capacity::arduPilotFixedBytes : 0).arg(_capacity.storageBytes);
        }
    } else {
        text = tr("Vehicle fence: %1 points").arg(usage.items);
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

QVariantList GeoZoneFenceImporter::choicesInfo() const
{
    QVariantList list;
    int leftOut = 0;
    for (const GeoZoneFences::Choice &choice : _selection.choices) {
        const bool selected = choice.status == GeoZoneFences::Status::Selected;
        if (!selected && (++leftOut > maxListedChoices)) {
            break;
        }
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
        list.append(QVariantMap{
            { QStringLiteral("key"),                zone->key() },
            { QStringLiteral("name"),               zone->displayName() },
            { QStringLiteral("restriction"),        static_cast<int>(zone->restriction()) },
            { QStringLiteral("restrictionText"),    zone->restrictionText() },
            { QStringLiteral("proximityText"),      proximity },
            { QStringLiteral("points"),             choice.usage.items },
            { QStringLiteral("simplifyTolerance"),  choice.simplifyTolerance },
            { QStringLiteral("selected"),           selected },
            { QStringLiteral("included"),           !choice.candidate.excluded },
            { QStringLiteral("reason"),             GeoZoneFences::statusText(choice) },
        });
    }
    return list;
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
