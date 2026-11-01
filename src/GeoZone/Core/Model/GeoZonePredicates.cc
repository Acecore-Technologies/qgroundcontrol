/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePredicates.h"
#include "GeoZoneVector.h"

#include <QtCore/QRectF>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace GeoZoneGeometry {

namespace {

bool _onSegment(const QPointF &a, const QPointF &b, const QPointF &point)
{
    return (point.x() >= std::min(a.x(), b.x())) && (point.x() <= std::max(a.x(), b.x())) &&
           (point.y() >= std::min(a.y(), b.y())) && (point.y() <= std::max(a.y(), b.y()));
}

/// @return Winding number of the unclosed ring around the point, positive counterclockwise, nothing when the point is on
///         the boundary within kBoundaryTolerance
std::optional<int> _winding(const QList<QPointF> &ring, const QPointF &point)
{
    int winding = 0;
    for (qsizetype i = 0, j = ring.count() - 1; i < ring.count(); j = i++) {
        const QPointF &a = ring[j];
        const QPointF &b = ring[i];
        const QPointF edge = b - a;
        const double side = GeoZoneVector::cross(edge, point - a);
        if ((point.x() >= ((std::min)(a.x(), b.x()) - kBoundaryTolerance)) && (point.x() <= ((std::max)(a.x(), b.x()) + kBoundaryTolerance)) &&
            (point.y() >= ((std::min)(a.y(), b.y()) - kBoundaryTolerance)) && (point.y() <= ((std::max)(a.y(), b.y()) + kBoundaryTolerance)) &&
            (std::abs(side) <= (kBoundaryTolerance * GeoZoneVector::length(edge)))) {
            return std::nullopt;
        }
        // Edges crossing the horizontal line through the point upwards with the point on their left, or downwards with
        // the point on their right
        if (a.y() <= point.y()) {
            if ((b.y() > point.y()) && (side > 0)) {
                winding++;
            }
        } else if ((b.y() <= point.y()) && (side < 0)) {
            winding--;
        }
    }
    return winding;
}

bool _filled(int winding, Qt::FillRule fillRule)
{
    return (fillRule == Qt::WindingFill) ? (winding != 0) : ((winding % 2) != 0);
}

} // namespace

bool ringContains(const QList<QPointF> &ring, const QPointF &point, Qt::FillRule fillRule)
{
    const std::optional<int> winding = _winding(ring, point);
    return !winding || _filled(*winding, fillRule);
}

bool polygonContains(const QList<QList<QPointF>> &rings, const QPointF &point, Qt::FillRule fillRule)
{
    if (rings.isEmpty()) {
        return false;
    }
    const std::optional<int> outer = _winding(rings.first(), point);
    if (!outer) {
        return true;
    }
    if (!_filled(*outer, fillRule)) {
        return false;
    }
    for (qsizetype i = 1; i < rings.count(); i++) {
        const std::optional<int> hole = _winding(rings[i], point);
        // The boundary of a hole is that of the polygon
        if (!hole) {
            return true;
        }
        if (_filled(*hole, Qt::OddEvenFill)) {
            return false;
        }
    }
    return true;
}

double signedArea(const QList<QPointF> &ring)
{
    double area = 0;
    for (qsizetype i = 0; i < ring.count(); i++) {
        area += GeoZoneVector::cross(ring[i], ring[(i + 1) % ring.count()]);
    }
    return area / 2;
}

double signedArea(const QList<QGeoCoordinate> &ring)
{
    double area = 0;
    for (qsizetype i = 0; i < ring.count(); i++) {
        const QGeoCoordinate &a = ring[i];
        const QGeoCoordinate &b = ring[(i + 1) % ring.count()];
        area += (a.longitude() * b.latitude()) - (b.longitude() * a.latitude());
    }
    return area / 2;
}

bool segmentsIntersect(const QPointF &a1, const QPointF &a2, const QPointF &b1, const QPointF &b2)
{
    const int o1 = GeoZoneVector::orientation(a1, a2, b1);
    const int o2 = GeoZoneVector::orientation(a1, a2, b2);
    const int o3 = GeoZoneVector::orientation(b1, b2, a1);
    const int o4 = GeoZoneVector::orientation(b1, b2, a2);

    if ((o1 != o2) && (o3 != o4)) {
        return true;
    }
    return ((o1 == 0) && _onSegment(a1, a2, b1)) || ((o2 == 0) && _onSegment(a1, a2, b2)) ||
           ((o3 == 0) && _onSegment(b1, b2, a1)) || ((o4 == 0) && _onSegment(b1, b2, a2));
}

double distanceToSegment(const QPointF &point, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double lengthSquared = QPointF::dotProduct(ab, ab);
    const double t = (lengthSquared > 0) ? std::clamp(QPointF::dotProduct(point - a, ab) / lengthSquared, 0.0, 1.0) : 0.0;
    return GeoZoneVector::length(a + (ab * t) - point);
}

double distanceToRing(const QPointF &point, const QList<QPointF> &ring, double xScale)
{
    const auto offset = [&point, xScale](const QPointF &vertex) {
        return QPointF((vertex.x() - point.x()) * xScale, vertex.y() - point.y());
    };
    double minDistance = std::numeric_limits<double>::max();
    for (qsizetype i = 0; i < ring.count(); i++) {
        minDistance = std::min(minDistance, distanceToSegment(QPointF(), offset(ring[i]), offset(ring[(i + 1) % ring.count()])));
    }
    return minDistance;
}

bool segmentIntersectsRing(const QPointF &a, const QPointF &b, const QList<QPointF> &ring)
{
    for (qsizetype i = 0; i < ring.count(); i++) {
        if (segmentsIntersect(a, b, ring[i], ring[(i + 1) % ring.count()])) {
            return true;
        }
    }
    return false;
}

QList<Interval> segmentPolygonIntervals(const QPointF &a, const QPointF &b, const QList<QList<QPointF>> &rings, Qt::FillRule fillRule)
{
    if (rings.isEmpty()) {
        return {};
    }
    const QPointF direction = b - a;
    const double lengthSquared = QPointF::dotProduct(direction, direction);
    if (lengthSquared < 1e-12) {
        return polygonContains(rings, a, fillRule) ? QList<Interval>{ { 0, 1 } } : QList<Interval>{};
    }
    const double length = std::sqrt(lengthSquared);

    // Fractions where the segment meets the boundary, which is inside, and the segment ends
    QList<std::pair<double, bool>> cuts = { { 0, false }, { 1, false } };
    constexpr double tolerance = 1e-9;
    for (const QList<QPointF> &ring : rings) {
        for (qsizetype i = 0; i < ring.count(); i++) {
            const QPointF &edgeStart = ring[i];
            const QPointF edge = ring[(i + 1) % ring.count()] - edgeStart;
            const QPointF offset = edgeStart - a;
            const double denominator = GeoZoneVector::cross(direction, edge);
            if (std::abs(denominator) <= (tolerance * length * GeoZoneVector::length(edge))) {
                // Parallel, a collinear edge overlaps the segment on the boundary
                if (std::abs(GeoZoneVector::cross(offset, direction)) > (1e-6 * length)) {
                    continue;
                }
                const double t1 = QPointF::dotProduct(offset, direction) / lengthSquared;
                const double t2 = QPointF::dotProduct(offset + edge, direction) / lengthSquared;
                const double first = (std::max)((std::min)(t1, t2), 0.0);
                const double last = (std::min)((std::max)(t1, t2), 1.0);
                if (first <= last) {
                    cuts.append({ first, true });
                    cuts.append({ last, true });
                }
                continue;
            }
            const double t = GeoZoneVector::cross(offset, edge) / denominator;
            const double s = GeoZoneVector::cross(offset, direction) / denominator;
            if ((t >= -tolerance) && (t <= (1 + tolerance)) && (s >= -tolerance) && (s <= (1 + tolerance))) {
                cuts.append({ std::clamp(t, 0.0, 1.0), true });
            }
        }
    }
    std::sort(cuts.begin(), cuts.end());

    // Between consecutive cuts the segment is either inside or outside
    QList<Interval> intervals;
    const auto pointAt = [&a, &direction](double t) { return a + (direction * t); };
    for (qsizetype i = 0; i < cuts.count(); i++) {
        const double t = cuts[i].first;
        const bool boundary = cuts[i].second || ((i + 1 < cuts.count()) && (cuts[i + 1].first == t) && cuts[i + 1].second);
        if (boundary || polygonContains(rings, pointAt(t), fillRule)) {
            intervals.append({ t, t });
        }
        if ((i + 1 < cuts.count()) && (cuts[i + 1].first > t) && polygonContains(rings, pointAt((t + cuts[i + 1].first) / 2), fillRule)) {
            intervals.append({ t, cuts[i + 1].first });
        }
    }
    return intervals;
}

std::optional<Interval> segmentCircleInterval(const QPointF &a, const QPointF &b, double radius)
{
    const QPointF direction = b - a;
    const double quadratic = QPointF::dotProduct(direction, direction);
    const double linear = 2 * QPointF::dotProduct(a, direction);
    const double constant = QPointF::dotProduct(a, a) - (radius * radius);
    if (quadratic < 1e-12) {
        return (constant <= 0) ? std::make_optional(Interval{ 0, 1 }) : std::nullopt;
    }
    const double discriminant = (linear * linear) - (4 * quadratic * constant);
    if (discriminant < 0) {
        return std::nullopt;
    }
    const double root = std::sqrt(discriminant);
    const double first = (-linear - root) / (2 * quadratic);
    const double last = (-linear + root) / (2 * quadratic);
    if ((last < 0) || (first > 1)) {
        return std::nullopt;
    }
    return Interval{ (std::max)(first, 0.0), (std::min)(last, 1.0) };
}

bool isSimple(const QList<QPointF> &ring)
{
    const qsizetype count = ring.count();
    if (count < 3) {
        return false;
    }

    for (qsizetype i = 0; i < count; i++) {
        const QPointF &a1 = ring[i];
        const QPointF &a2 = ring[(i + 1) % count];
        const QPointF &a3 = ring[(i + 2) % count];
        // Adjacent edges may only share their vertex, not fold back over each other
        if ((GeoZoneVector::orientation(a1, a2, a3) == 0) && (QPointF::dotProduct(a2 - a1, a3 - a2) < 0)) {
            return false;
        }
    }

    const auto adjacent = [count](qsizetype i, qsizetype j) { return (j == ((i + 1) % count)) || (i == ((j + 1) % count)); };
    constexpr qsizetype smallRing = 32;
    if (count <= smallRing) {
        for (qsizetype i = 0; i < count; i++) {
            const QPointF &a1 = ring[i];
            const QPointF &a2 = ring[(i + 1) % count];
            for (qsizetype j = i + 2; j < count; j++) {
                const QPointF &b1 = ring[j];
                const QPointF &b2 = ring[(j + 1) % count];
                const bool apart = ((std::max)(a1.x(), a2.x()) < (std::min)(b1.x(), b2.x())) || ((std::max)(b1.x(), b2.x()) < (std::min)(a1.x(), a2.x())) ||
                                   ((std::max)(a1.y(), a2.y()) < (std::min)(b1.y(), b2.y())) || ((std::max)(b1.y(), b2.y()) < (std::min)(a1.y(), a2.y()));
                if (!apart && !adjacent(i, j) && segmentsIntersect(a1, a2, b1, b2)) {
                    return false;
                }
            }
        }
        return true;
    }

    // Sweep over the edges in the order of their west end, testing each against the following edges which overlap it
    // in x, so that large rings don't take a test of all pairs
    QList<QRectF> bounds;
    bounds.reserve(count);
    for (qsizetype i = 0; i < count; i++) {
        bounds.append(QRectF(ring[i], ring[(i + 1) % count]).normalized());
    }
    QList<qsizetype> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&bounds](qsizetype a, qsizetype b) { return bounds[a].left() < bounds[b].left(); });
    for (qsizetype k = 0; k < count; k++) {
        const qsizetype i = order[k];
        const QRectF &box = bounds[i];
        for (qsizetype l = k + 1; (l < count) && (bounds[order[l]].left() <= box.right()); l++) {
            const qsizetype j = order[l];
            if (adjacent(i, j) || (bounds[j].top() > box.bottom()) || (bounds[j].bottom() < box.top())) {
                continue;
            }
            if (segmentsIntersect(ring[i], ring[(i + 1) % count], ring[j], ring[(j + 1) % count])) {
                return false;
            }
        }
    }
    return true;
}

} // namespace GeoZoneGeometry
