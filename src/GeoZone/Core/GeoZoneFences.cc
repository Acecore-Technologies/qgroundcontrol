/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFences.h"
#include "GeoZone.h"
#include "GeoZoneGeometry.h"

#include <algorithm>

namespace GeoZoneFences {

ZoneFences zoneFences(const GeoZone *zone, double simplifyTolerance)
{
    return ZoneFences{ zone->exclusionPolygons(simplifyTolerance), zone->exclusionCircles() };
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
        return GeoZoneGeometry::ringContains(ring, point) || (GeoZoneGeometry::distanceToRing(point, ring) <= boundaryTolerance);
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
        return fence.center().distanceTo(point) <= (fence.radius() + boundaryTolerance);
    });
}

bool _circleWithin(const QGeoCircle &circle, const QList<QGeoCoordinate> &fence)
{
    const GeoZoneGeometry::LocalProjection projection(circle.center());
    const QList<QPointF> ring = projection.toLocal(fence);
    return GeoZoneGeometry::ringContains(ring, QPointF(0, 0)) &&
           (GeoZoneGeometry::distanceToRing(QPointF(0, 0), ring) >= (circle.radius() - boundaryTolerance));
}

} // namespace

bool fencesCover(const ZoneFences &fences, const GeoZone *zone)
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
                return (fence.center().distanceTo(circle.center()) + circle.radius()) <= (fence.radius() + boundaryTolerance);
            }) ||
            std::any_of(fences.polygons.cbegin(), fences.polygons.cend(), [&circle](const QList<QGeoCoordinate> &fence) { return _circleWithin(circle, fence); });
        if (!covered) {
            return false;
        }
    }
    return true;
}

} // namespace GeoZoneFences
