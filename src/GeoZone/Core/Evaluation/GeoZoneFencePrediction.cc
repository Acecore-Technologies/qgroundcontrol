/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFencePrediction.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneVector.h"

#include <QtCore/QPointF>

#include <algorithm>
#include <cmath>

namespace GeoZoneFencePrediction {

namespace {

using GeoZoneVector::cross;
using GeoZoneVector::dot;

constexpr double kEpsilon = 1e-9;

/// Adds the times within the horizon at which the point moving from p at the velocity v is at the distance from center
void addCircleHits(const QPointF &p, const QPointF &v, double horizon, const QPointF &center, double distance, QList<double> &times)
{
    const QPointF offset = p - center;
    const double a = dot(v, v);
    if (a < kEpsilon) {
        return;
    }
    const double b = 2 * dot(offset, v);
    const double c = dot(offset, offset) - (distance * distance);
    const double discriminant = (b * b) - (4 * a * c);
    if (discriminant < 0) {
        return;
    }
    const double root = std::sqrt(discriminant);
    for (const double t : { (-b - root) / (2 * a), (-b + root) / (2 * a) }) {
        if ((t >= 0) && (t <= horizon)) {
            times.append(t);
        }
    }
}

/// Adds the times within the horizon at which the point moving from p at the velocity v is at the distance from the
/// segment from a to b, where it crosses the segment for a distance of 0
void addSegmentHits(const QPointF &p, const QPointF &v, double horizon, const QPointF &a, const QPointF &b, double distance, QList<double> &times)
{
    const QPointF edge = b - a;
    const auto add = [&times, horizon](double t) {
        if ((t >= 0) && (t <= horizon)) {
            times.append(t);
        }
    };
    if (distance <= 0) {
        const double denominator = cross(v, edge);
        if (std::abs(denominator) < kEpsilon) {
            return;
        }
        const QPointF toA = a - p;
        const double u = cross(toA, v) / denominator;
        if ((u >= 0) && (u <= 1)) {
            add(cross(toA, edge) / denominator);
        }
        return;
    }
    // The sides of the capsule around the segment, then its round ends
    const double length = GeoZoneVector::length(edge);
    if (length > 0) {
        const QPointF normal(-edge.y() / length, edge.x() / length);
        const double offset = dot(p - a, normal);
        const double speed = dot(v, normal);
        if (std::abs(speed) > kEpsilon) {
            for (const double side : { distance, -distance }) {
                const double t = (side - offset) / speed;
                const double u = dot((p + (v * t)) - a, edge) / (length * length);
                if ((u >= 0) && (u <= 1)) {
                    add(t);
                }
            }
        }
    }
    addCircleHits(p, v, horizon, a, distance, times);
    addCircleHits(p, v, horizon, b, distance, times);
}

/// A polygon or circle of the fence, in meters around the vehicle
struct Area {
    Boundary boundary = Boundary::Polygon;
    int index = -1;
    bool inclusion = false;
    QList<QPointF> ring;    ///< Of a polygon
    QPointF center;         ///< Of a circle
    double radius = 0;
    double margin = 0;      ///< Within which the boundary counts as breached

    bool contains(const QPointF &point) const
    {
        if (boundary == Boundary::Polygon) {
            // Autopilots fill fences with the even-odd rule
            return GeoZoneGeometry::ringContains(ring, point, Qt::OddEvenFill);
        }
        return GeoZoneVector::length(point - center) <= radius;
    }

    double boundaryDistance(const QPointF &point) const
    {
        if (boundary == Boundary::Polygon) {
            return GeoZoneGeometry::distanceToRing(point, ring);
        }
        return std::abs(GeoZoneVector::length(point - center) - radius);
    }

    /// @return true on the allowed side, farther than the margin from the boundary
    bool clear(const QPointF &point) const
    {
        return (contains(point) == inclusion) && (boundaryDistance(point) >= (margin - 1e-6));
    }

    /// @return Ascending times within the horizon at which the moving point is at the margin from the boundary
    QList<double> hits(const QPointF &p, const QPointF &v, double horizon) const
    {
        QList<double> times;
        if (boundary == Boundary::Polygon) {
            for (qsizetype i = 0; i < ring.count(); i++) {
                addSegmentHits(p, v, horizon, ring[i], ring[(i + 1) % ring.count()], margin, times);
            }
        } else {
            addCircleHits(p, v, horizon, center, inclusion ? (radius - margin) : (radius + margin), times);
        }
        std::sort(times.begin(), times.end());
        return times;
    }
};

bool _isInclusion(const Area &area)
{
    return area.inclusion;
}

} // namespace

bool Fence::isEmpty() const
{
    return polygons.isEmpty() && circles.isEmpty() && std::isnan(maxDistance) && std::isnan(maxAltitude) && std::isnan(minAltitude);
}

bool breached(const Fence &fence, const QGeoCoordinate &coordinate, double altitude)
{
    if (!coordinate.isValid()) {
        return false;
    }
    if (!std::isnan(altitude) && ((altitude > fence.maxAltitude) || (altitude < fence.minAltitude))) {
        return true;
    }
    if (!std::isnan(fence.maxDistance) && fence.home.isValid() && (fence.home.distanceTo(coordinate) > fence.maxDistance)) {
        return true;
    }
    const GeoZoneGeometry::LocalProjection projection(coordinate);
    bool inclusions = false;
    bool insideAny = false;
    bool outsideAny = false;
    const auto check = [&](bool inclusion, bool inside) {
        if (inclusion) {
            inclusions = true;
            insideAny = insideAny || inside;
            outsideAny = outsideAny || !inside;
        }
        return !inclusion && inside;
    };
    for (const Fence::Polygon &polygon : fence.polygons) {
        if ((polygon.vertices.count() >= 3) && check(polygon.inclusion, GeoZoneGeometry::ringContains(projection.toLocal(polygon.vertices), QPointF(), Qt::OddEvenFill))) {
            return true;
        }
    }
    for (const Fence::Circle &circle : fence.circles) {
        if (check(circle.inclusion, circle.circle.center().distanceTo(coordinate) <= circle.circle.radius())) {
            return true;
        }
    }
    return inclusions && (fence.inclusionUnion ? !insideAny : outsideAny);
}

std::optional<Breach> predict(const Fence &fence, const QGeoCoordinate &coordinate, double altitude, const Velocity &velocity, double horizon)
{
    if (!velocity.valid || !coordinate.isValid() || !(horizon > 0) || breached(fence, coordinate, altitude)) {
        return std::nullopt;
    }

    // The vehicle at the origin moving east (x) and north (y)
    const GeoZoneGeometry::LocalProjection projection(coordinate);
    const QPointF start;
    const QPointF v(velocity.east, velocity.north);

    std::optional<Breach> first;
    const auto consider = [&](double time, Boundary boundary, bool inclusion, int index) {
        if (!first || (time < first->time)) {
            first = Breach{ boundary, inclusion, index, time };
        }
    };

    // Within the margin of a boundary already, the boundary itself is predicted
    QList<Area> areas;
    const auto addArea = [&](Area area) {
        area.margin = (area.boundaryDistance(start) > fence.horizontalMargin) ? fence.horizontalMargin : 0;
        areas.append(area);
    };
    for (int i = 0; i < fence.polygons.count(); i++) {
        const Fence::Polygon &polygon = fence.polygons[i];
        if (polygon.vertices.count() >= 3) {
            addArea(Area{ Boundary::Polygon, i, polygon.inclusion, projection.toLocal(polygon.vertices) });
        }
    }
    for (int i = 0; i < fence.circles.count(); i++) {
        const Fence::Circle &circle = fence.circles[i];
        addArea(Area{ Boundary::Circle, i, circle.inclusion, {}, projection.toLocal(circle.circle.center()), circle.circle.radius() });
    }

    if ((v.x() != 0) || (v.y() != 0)) {
        // Entering any exclusion area, or leaving the inclusion areas: one of them, or all of them for their union
        QList<std::pair<double, const Area*>> inclusionHits;
        for (const Area &area : std::as_const(areas)) {
            const QList<double> times = area.hits(start, v, horizon);
            if (times.isEmpty()) {
                continue;
            }
            if (!area.inclusion || !fence.inclusionUnion) {
                consider(times.first(), area.boundary, area.inclusion, area.index);
            } else {
                for (const double time : times) {
                    inclusionHits.append({ time, &area });
                }
            }
        }
        std::sort(inclusionHits.begin(), inclusionHits.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        const double step = 0.01 / GeoZoneVector::length(v);
        for (const auto &[time, area] : std::as_const(inclusionHits)) {
            const QPointF after = start + (v * (time + step));
            if (std::none_of(areas.cbegin(), areas.cend(), [&after](const Area &other) { return _isInclusion(other) && other.clear(after); })) {
                consider(time, area->boundary, true, area->index);
                break;
            }
        }

        if (!std::isnan(fence.maxDistance) && fence.home.isValid()) {
            Area home{ Boundary::MaxDistance, -1, true, {}, projection.toLocal(fence.home), fence.maxDistance };
            home.margin = (home.boundaryDistance(start) > fence.horizontalMargin) ? fence.horizontalMargin : 0;
            const QList<double> times = home.hits(start, v, horizon);
            if (!times.isEmpty()) {
                consider(times.first(), Boundary::MaxDistance, true, -1);
            }
        }
    }

    // Climbing to the ceiling or descending to the floor, at the margin unless within it already
    if (!std::isnan(altitude) && (velocity.up != 0)) {
        const double ceiling = (velocity.up > 0) ? fence.maxAltitude : fence.minAltitude;
        if (!std::isnan(ceiling)) {
            const double margin = (std::abs(ceiling - altitude) > fence.verticalMargin) ? fence.verticalMargin : 0;
            const double limit = (velocity.up > 0) ? (ceiling - margin) : (ceiling + margin);
            const double time = (limit - altitude) / velocity.up;
            if ((time >= 0) && (time <= horizon)) {
                consider(time, (velocity.up > 0) ? Boundary::MaxAltitude : Boundary::MinAltitude, false, -1);
            }
        }
    }

    if (first) {
        const double speed = std::sqrt((velocity.north * velocity.north) + (velocity.east * velocity.east) + (velocity.up * velocity.up));
        first->distance = speed * first->time;
        first->coordinate = projection.toGeo(start + (v * first->time));
        first->altitude = std::isnan(altitude) ? qQNaN() : (altitude + (velocity.up * first->time));
    }
    return first;
}

} // namespace GeoZoneFencePrediction
