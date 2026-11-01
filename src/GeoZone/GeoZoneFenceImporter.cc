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
#include "GeoZoneGeometry.h"
#include "GeoZoneManager.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include <algorithm>

GeoZoneFenceImporter::GeoZoneFenceImporter(QObject *parent)
    : QObject(parent)
{
    (void) connect(GeoZoneManager::instance(), &GeoZoneManager::zonesChanged, this, [this]() { _previewFences.reset(); });
}

void GeoZoneFenceImporter::setGeoFenceController(GeoFenceController *geoFenceController)
{
    if (geoFenceController != _geoFenceController) {
        _geoFenceController = geoFenceController;
        emit geoFenceControllerChanged();
    }
}

QList<const GeoZone*> GeoZoneFenceImporter::_zonesToImport(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours)
{
    QList<GeoZone::Restriction> restrictionList;
    for (const QVariant &restriction : restrictions) {
        restrictionList.append(static_cast<GeoZone::Restriction>(restriction.toInt()));
    }

    QDateTime activeFrom;
    QDateTime activeTo;
    if (activeWithinHours >= 0) {
        activeFrom = QDateTime::currentDateTimeUtc();
        if (activeWithinHours > 0) {
            activeTo = activeFrom.addSecs(activeWithinHours * 3600);
        }
    }

    return GeoZoneManager::instance()->catalog().zonesInArea(area, GeoZonePolicy(restrictionList).activeDuring(activeFrom, activeTo));
}

GeoZoneFenceImporter::Fences GeoZoneFenceImporter::_fencesFor(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours, double simplifyTolerance) const
{
    const PreviewKey key{ restrictions, area, activeWithinHours, simplifyTolerance, _geoFenceController->polygons()->count(), _geoFenceController->circles()->count() };
    if (_previewFences && (_previewFences->first == key) && (_previewAge.elapsed() < _previewReuseMSecs)) {
        return _previewFences->second;
    }

    // Simplifying many zones takes a while, so the fences of a preview are kept for the import
    _previewFences.emplace(key, _newFences(_zonesToImport(restrictions, area, activeWithinHours), simplifyTolerance));
    _previewAge.start();
    return _previewFences->second;
}

int GeoZoneFenceImporter::importZones(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours, double simplifyTolerance)
{
    if (!_geoFenceController) {
        return 0;
    }
    const int added = _addFences(_fencesFor(restrictions, area, activeWithinHours, simplifyTolerance));
    _previewFences.reset();

    qCDebug(GeoZoneManagerLog) << "importZones fences" << added;

    return added;
}

QVariantMap GeoZoneFenceImporter::preview(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours, double simplifyTolerance) const
{
    if (!_geoFenceController) {
        return QVariantMap();
    }
    return _preview(_fencesFor(restrictions, area, activeWithinHours, simplifyTolerance));
}

QVariantMap GeoZoneFenceImporter::exclusionsPreview(const QList<const GeoZone*> &zones, double simplifyTolerance) const
{
    if (!_geoFenceController) {
        return QVariantMap();
    }
    return _preview(_newFences(zones, simplifyTolerance));
}

QVariantMap GeoZoneFenceImporter::_preview(const Fences &fences) const
{
    int pointCount = fences.circles.count();
    qsizetype maxPolygonVertexCount = 0;
    for (const QList<QGeoCoordinate> &polygon : fences.polygons) {
        pointCount += polygon.count();
        maxPolygonVertexCount = (std::max)(maxPolygonVertexCount, polygon.count());
    }

    QmlObjectListModel *const polygons = _geoFenceController->polygons();
    int totalPointCount = pointCount + _geoFenceController->circles()->count() + (_geoFenceController->breachReturnPoint().isValid() ? 1 : 0);
    for (int i = 0; i < polygons->count(); i++) {
        totalPointCount += polygons->value<QGCFencePolygon*>(i)->count();
    }

    QString limitWarning;
    const Vehicle *const vehicle = _geoFenceController->masterController()->managerVehicle();
    if ((pointCount > 0) && vehicle) {
        if (vehicle->px4Firmware() && (totalPointCount > px4MaxFencePoints)) {
            limitWarning = tr("The fence will have %1 points. PX4 vehicles can store at most %2 fence points, so the upload will likely be rejected. Import fewer zones or increase the simplification.")
                               .arg(totalPointCount).arg(px4MaxFencePoints);
        } else if (vehicle->apmFirmware() && (maxPolygonVertexCount > arduPilotMaxPolygonVertices)) {
            limitWarning = tr("A fence polygon will have %1 vertices. ArduPilot vehicles support at most %2 vertices per polygon and the total fence storage of many boards is much smaller. Increase the simplification.")
                               .arg(maxPolygonVertexCount).arg(arduPilotMaxPolygonVertices);
        }
    }

    return QVariantMap{
        { QStringLiteral("fenceCount"),         fences.polygons.count() + fences.circles.count() },
        { QStringLiteral("pointCount"),         pointCount },
        { QStringLiteral("totalPointCount"),    totalPointCount },
        { QStringLiteral("limitWarning"),       limitWarning },
    };
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
        const ZoneFences zoneShapes = zoneFences(zone, simplifyTolerance);
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

GeoZoneFenceImporter::ZoneFences GeoZoneFenceImporter::zoneFences(const GeoZone *zone, double simplifyTolerance)
{
    return ZoneFences{ zone->exclusionPolygons(simplifyTolerance), zone->exclusionCircles() };
}

GeoZoneFenceImporter::ZoneFences GeoZoneFenceImporter::exclusionFences(GeoFenceController *geoFenceController)
{
    ZoneFences fences;
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

namespace {

/// @return true if the polygon lies within the fence polygon up to the tolerance, edges included
bool _polygonWithin(const QList<QGeoCoordinate> &polygon, const QList<QGeoCoordinate> &fence)
{
    if (polygon == fence) {
        return true;
    }
    const GeoZoneGeometry::LocalProjection projection(fence.first());
    const QList<QList<QPointF>> fenceRings = { projection.toLocal(fence) };
    const QList<QPointF> &ring = fenceRings.first();
    const auto near = [&ring](const QPointF &point) {
        return GeoZoneGeometry::ringContains(ring, point) || (GeoZoneGeometry::distanceToRing(point, ring) <= GeoZoneFenceImporter::fenceBoundaryTolerance);
    };
    for (qsizetype i = 0; i < polygon.count(); i++) {
        // Vertices inside, and edges not leaving the fence between them, like around a notch of the fence
        const QPointF a = projection.toLocal(polygon[i]);
        const QPointF b = projection.toLocal(polygon[(i + 1) % polygon.count()], a);
        if (!near(a)) {
            return false;
        }
        double covered = 0;
        QList<GeoZoneGeometry::Interval> inside = GeoZoneGeometry::segmentPolygonIntervals(a, b, fenceRings);
        inside.append({ 1, 1 });
        for (const GeoZoneGeometry::Interval &interval : std::as_const(inside)) {
            if (interval.first > covered) {
                for (const double fraction : { 0.25, 0.5, 0.75 }) {
                    if (!near(a + ((b - a) * (covered + ((interval.first - covered) * fraction))))) {
                        return false;
                    }
                }
            }
            covered = (std::max)(covered, interval.second);
        }
    }
    return true;
}

bool _polygonWithinCircle(const QList<QGeoCoordinate> &polygon, const QGeoCircle &fence)
{
    return std::all_of(polygon.cbegin(), polygon.cend(), [&fence](const QGeoCoordinate &point) {
        return fence.center().distanceTo(point) <= (fence.radius() + GeoZoneFenceImporter::fenceBoundaryTolerance);
    });
}

bool _circleWithin(const QGeoCircle &circle, const QList<QGeoCoordinate> &fence)
{
    const GeoZoneGeometry::LocalProjection projection(circle.center());
    const QList<QPointF> ring = projection.toLocal(fence);
    return GeoZoneGeometry::ringContains(ring, QPointF(0, 0)) &&
           (GeoZoneGeometry::distanceToRing(QPointF(0, 0), ring) >= (circle.radius() - GeoZoneFenceImporter::fenceBoundaryTolerance));
}

} // namespace

bool GeoZoneFenceImporter::fencesCover(const ZoneFences &fences, const GeoZone *zone)
{
    const ZoneFences zoneShapes = zoneFences(zone);
    if (zoneShapes.isEmpty()) {
        return false;
    }
    for (const QList<QGeoCoordinate> &polygon : zoneShapes.polygons) {
        const bool covered = std::any_of(fences.polygons.cbegin(), fences.polygons.cend(), [&polygon](const QList<QGeoCoordinate> &fence) { return _polygonWithin(polygon, fence); }) ||
                             std::any_of(fences.circles.cbegin(), fences.circles.cend(), [&polygon](const QGeoCircle &fence) { return _polygonWithinCircle(polygon, fence); });
        if (!covered) {
            return false;
        }
    }
    for (const QGeoCircle &circle : zoneShapes.circles) {
        const bool covered =
            std::any_of(fences.circles.cbegin(), fences.circles.cend(), [&circle](const QGeoCircle &fence) {
                return (fence.center().distanceTo(circle.center()) + circle.radius()) <= (fence.radius() + fenceBoundaryTolerance);
            }) ||
            std::any_of(fences.polygons.cbegin(), fences.polygons.cend(), [&circle](const QList<QGeoCoordinate> &fence) { return _circleWithin(circle, fence); });
        if (!covered) {
            return false;
        }
    }
    return true;
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
        polygon->setName(fences.polygonNames[i]);
        polygon->setGeoZone(true);
        newPolygons.append(polygon);
    }
    QList<QObject*> newCircles;
    for (qsizetype i = 0; i < fences.circles.count(); i++) {
        QGCFenceCircle *const circle = new QGCFenceCircle(fences.circles[i].center(), fences.circles[i].radius(), false /* inclusion */, _geoFenceController);
        circle->setName(fences.circleNames[i]);
        circle->setGeoZone(true);
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
