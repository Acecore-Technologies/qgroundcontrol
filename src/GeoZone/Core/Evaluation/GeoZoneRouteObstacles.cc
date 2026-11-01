/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRouteObstacles.h"
#include "GeoZoneBox.h"
#include "GeoZoneRouteVector.h"

#include <QtCore/QtMath>

namespace {

constexpr double kNodeOffset = 0.5;             ///< Meters nodes are placed beyond the grown zones, so that edges along them pass
constexpr double kMaxArcStep = M_PI / 4;        ///< Radians between the nodes around a convex corner
constexpr int kMinCircleNodes = 8;
constexpr int kMaxCircleNodes = 32;
constexpr double kEndpointRelaxation = 0.9;     ///< Of the distance to a zone that segments from an end within the margin keep
constexpr qsizetype kMinSimplifiedVertices = 16;

using GeoZoneVector::cross;
using GeoZoneVector::dot;
using GeoZoneVector::length;

double _pointSegmentDistance(const QPointF &point, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double lengthSquared = dot(ab, ab);
    const double t = (lengthSquared > 0) ? std::clamp(dot(point - a, ab) / lengthSquared, 0.0, 1.0) : 0.0;
    return length(point - (a + (ab * t)));
}

/// @return true if the segments cross at a point inside both
bool _segmentsCross(const QPointF &a, const QPointF &b, const QPointF &c, const QPointF &d)
{
    const double d1 = cross(b - a, c - a);
    const double d2 = cross(b - a, d - a);
    const double d3 = cross(d - c, a - c);
    const double d4 = cross(d - c, b - c);
    return (((d1 > 0) && (d2 < 0)) || ((d1 < 0) && (d2 > 0))) && (((d3 > 0) && (d4 < 0)) || ((d3 < 0) && (d4 > 0)));
}

double _segmentDistance(const QPointF &a, const QPointF &b, const QPointF &c, const QPointF &d)
{
    if (_segmentsCross(a, b, c, d)) {
        return 0;
    }
    return (std::min)({ _pointSegmentDistance(a, c, d), _pointSegmentDistance(b, c, d), _pointSegmentDistance(c, a, b), _pointSegmentDistance(d, a, b) });
}

} // namespace

void GeoZoneRouteGrid::reset(const QRectF &extent)
{
    _extent = extent;
    _cellWidth = (std::max)(extent.width() / kCells, 1.0);
    _cellHeight = (std::max)(extent.height() / kCells, 1.0);
    _cells.assign(static_cast<size_t>(kCells) * kCells, {});
    _stamps.clear();
    _stamp = 0;
}

void GeoZoneRouteGrid::insertRect(int item, const QRectF &bounds)
{
    const int firstColumn = _column(bounds.left());
    const int lastColumn = _column(bounds.right());
    for (int row = _row(bounds.top()); row <= _row(bounds.bottom()); row++) {
        for (int column = firstColumn; column <= lastColumn; column++) {
            _cells[(row * kCells) + column].push_back(item);
        }
    }
    _grow(item);
}

void GeoZoneRouteGrid::insertSegment(int item, const QPointF &a, const QPointF &b, double distance)
{
    (void) _visitCells(a, b, distance, [this, item](int cell) {
        _cells[cell].push_back(item);
        return false;
    });
    _grow(item);
}

int GeoZoneRouteGrid::_column(double x) const
{
    const double column = std::floor((x - _extent.left()) / _cellWidth);
    return static_cast<int>(std::clamp(std::isnan(column) ? 0.0 : column, 0.0, static_cast<double>(kCells - 1)));
}

int GeoZoneRouteGrid::_row(double y) const
{
    const double row = std::floor((y - _extent.top()) / _cellHeight);
    return static_cast<int>(std::clamp(std::isnan(row) ? 0.0 : row, 0.0, static_cast<double>(kCells - 1)));
}

void GeoZoneRouteGrid::_grow(int item)
{
    if (static_cast<size_t>(item) >= _stamps.size()) {
        _stamps.resize(static_cast<size_t>(item) + 1, 0);
    }
}

GeoZoneRouteObstacles::GeoZoneRouteObstacles(const GeoZone::Position &from, const GeoZone::Position &to, const GeoZoneGeometry::LocalProjection &projection,
                                             const QPointF &start, const QPointF &end, double margin, const QRectF &reach,
                                             GeoZoneRouteSearch::Cache &cache, GeoZoneRouteSearch::Statistics &statistics)
    : _from(from)
    , _to(to)
    , _projection(projection)
    , _start(start)
    , _end(end)
    , _margin(margin)
    , _reach(reach)
    , _cache(cache)
    , _statistics(statistics)
{
}

int GeoZoneRouteObstacles::add(const GeoZone *zone, int volume)
{
    const GeoZone::Volume &data = zone->data().volumes[volume];
    Obstacle obstacle;
    obstacle.zone = zone;
    obstacle.volume = volume;
    obstacle.startMargin = _margin;
    obstacle.endMargin = _margin;
    if (data.isCircle) {
        if (!data.center.isValid() || (data.radius <= 0)) {
            return -1;
        }
        obstacle.circle = true;
        obstacle.center = _projection.toLocal(data.center);
        obstacle.radius = data.radius;
        const double grown = data.radius + _margin;
        obstacle.bounds = QRectF(obstacle.center - QPointF(grown, grown), obstacle.center + QPointF(grown, grown));
    } else {
        const GeoZoneBox box = data.boundingBox();
        if (!box.isValid() || data.rings.isEmpty()) {
            return -1;
        }
        const QPointF southWest = _projection.lonLatToLocal(QPointF(box.west(), box.south()));
        const QPointF northEast = _projection.lonLatToLocal(QPointF(box.east(), box.north()), southWest);
        // Simplified outlines extend up to half the margin beyond the zone
        const double grown = (_margin * 1.5) + 1;
        obstacle.bounds = QRectF(southWest, northEast).normalized().adjusted(-grown, -grown, grown, grown);
    }
    const int index = static_cast<int>(_obstacles.count());
    _obstacles.append(obstacle);
    _indexes.insert({ zone, volume }, index);
    _statistics.obstacles++;
    if (_prepared) {
        _insert(index);
    }
    return index;
}

void GeoZoneRouteObstacles::prepare()
{
    QRectF extent = QRectF(_start, _end).normalized().adjusted(-_margin, -_margin, _margin, _margin);
    for (const Obstacle &obstacle : std::as_const(_obstacles)) {
        extent = extent.united(obstacle.bounds);
    }
    if (!_reach.isNull()) {
        extent = extent.intersected(_reach.united(QRectF(_start, _end).normalized()));
    }
    _extent = extent;
    _edgeGrid.reset(extent);
    _obstacleGrid.reset(extent);
    _prepared = true;
    for (int index = 0; index < _obstacles.count(); index++) {
        _insert(index);
    }
}

QList<const GeoZone*> GeoZoneRouteObstacles::zones(const QSet<int> &indexes) const
{
    QList<const GeoZone*> zones;
    for (const int index : indexes) {
        if (!zones.contains(_obstacles[index].zone)) {
            zones.append(_obstacles[index].zone);
        }
    }
    return zones;
}

/// Adds the obstacle to the grid of obstacles, and builds it when its bounds contain an end
void GeoZoneRouteObstacles::_insert(int index)
{
    _obstacleGrid.insertRect(index, _obstacles[index].bounds);
    if (_obstacles[index].bounds.contains(_start) || _obstacles[index].bounds.contains(_end)) {
        build(index);
    }
}

void GeoZoneRouteObstacles::build(int index)
{
    Obstacle &obstacle = _obstacles[index];
    if (obstacle.built) {
        return;
    }
    obstacle.built = true;
    if (obstacle.circle) {
        _items.append(Item{ index, obstacle.center, obstacle.center });
        _edgeGrid.insertRect(static_cast<int>(_items.count() - 1), obstacle.bounds);
    } else {
        // Exact near the ends, which must be inside or outside like for the zone
        const GeoZone::Volume &data = obstacle.zone->data().volumes[obstacle.volume];
        const bool simplify = !obstacle.bounds.contains(_start) && !obstacle.bounds.contains(_end);
        for (qsizetype ringIndex = 0; ringIndex < data.rings.count(); ringIndex++) {
            QList<QPointF> ring;
            for (const QPointF &point : _projection.lonLatToLocal(((ringIndex == 0) && simplify) ? _outerRing(obstacle.zone, obstacle.volume) : data.rings[ringIndex])) {
                if (ring.isEmpty() || (point != ring.constLast())) {
                    ring.append(point);
                }
            }
            if ((ring.count() > 1) && (ring.constFirst() == ring.constLast())) {
                ring.removeLast();
            }
            if (ring.count() < 3) {
                if (ringIndex == 0) {
                    break;
                }
                continue;
            }
            if ((ringIndex == 0) == (GeoZoneGeometry::signedArea(ring) < 0)) {
                std::reverse(ring.begin(), ring.end());
            }
            obstacle.rings.append(ring);
        }
        for (const QList<QPointF> &ring : std::as_const(obstacle.rings)) {
            for (qsizetype i = 0; i < ring.count(); i++) {
                _items.append(Item{ index, ring[i], ring[(i + 1) % ring.count()] });
                _edgeGrid.insertSegment(static_cast<int>(_items.count() - 1), ring[i], ring[(i + 1) % ring.count()], _margin + kTolerance);
            }
        }
    }
    bool inside = false;
    obstacle.startMargin = _endpointMargin(obstacle, _start, _from, inside);
    if (inside && !_startInside.contains(obstacle.zone)) {
        _startInside.append(obstacle.zone);
    }
    obstacle.endMargin = _endpointMargin(obstacle, _end, _to, inside);
    if (inside && !_endInside.contains(obstacle.zone)) {
        _endInside.append(obstacle.zone);
    }
}

/// Builds the obstacles whose bounds the segment crosses
void GeoZoneRouteObstacles::_buildAlong(const QPointF &a, const QPointF &b)
{
    QList<int> unbuilt;
    (void) _obstacleGrid.visitSegment(a, b, [&](int index) {
        const Obstacle &obstacle = _obstacles[index];
        if (!obstacle.built && (obstacle.bounds.left() <= (std::max)(a.x(), b.x())) && (obstacle.bounds.right() >= (std::min)(a.x(), b.x())) &&
            (obstacle.bounds.top() <= (std::max)(a.y(), b.y())) && (obstacle.bounds.bottom() >= (std::min)(a.y(), b.y()))) {
            unbuilt.append(index);
        }
        return false;
    });
    for (const int index : std::as_const(unbuilt)) {
        build(index);
    }
}

/// Builds the obstacles whose bounds contain the point
void GeoZoneRouteObstacles::_buildAt(const QPointF &point)
{
    QList<int> unbuilt;
    (void) _obstacleGrid.visitPoint(point, [&](int index) {
        if (!_obstacles[index].built && _obstacles[index].bounds.contains(point)) {
            unbuilt.append(index);
        }
        return false;
    });
    for (const int index : std::as_const(unbuilt)) {
        build(index);
    }
}

/// @return Outer ring of the polygon volume in degrees, simplified outward by a quarter of the margin when detailed,
///         so that it extends up to half the margin beyond the zone
GeoZone::Ring GeoZoneRouteObstacles::_outerRing(const GeoZone *zone, int volume)
{
    const GeoZone::Ring &ring = zone->data().volumes[volume].rings.constFirst();
    const double tolerance = _margin / 4;
    if ((ring.count() <= kMinSimplifiedVertices) || (tolerance <= 0)) {
        return ring;
    }
    if (_cache.tolerance != tolerance) {
        _cache.outerRings.clear();
        _cache.tolerance = tolerance;
    }
    const auto it = _cache.outerRings.constFind({ zone, volume });
    if (it != _cache.outerRings.constEnd()) {
        return *it;
    }
    const GeoZone::Ring simplified = GeoZone::toRing(GeoZoneGeometry::simplifyOutward(GeoZone::toCoordinates(ring), tolerance));
    _cache.outerRings.insert({ zone, volume }, simplified);
    return simplified;
}

/// @return Margin segments from the end keep from the obstacle: less than the margin when the end is within it,
///         0 when the end is inside the volume horizontally but above or below it
/// @param inside Set when the end is inside the volume
double GeoZoneRouteObstacles::_endpointMargin(const Obstacle &obstacle, const QPointF &point, const GeoZone::Position &position, bool &inside) const
{
    inside = false;
    double distance = 0;
    if (obstacle.circle) {
        distance = length(point - obstacle.center) - obstacle.radius;
    } else if (obstacle.rings.isEmpty()) {
        return _margin;
    } else if (!GeoZoneGeometry::polygonContains(obstacle.rings, point)) {
        distance = std::numeric_limits<double>::infinity();
        for (const QList<QPointF> &ring : obstacle.rings) {
            for (qsizetype i = 0; i < ring.count(); i++) {
                distance = (std::min)(distance, _pointSegmentDistance(point, ring[i], ring[(i + 1) % ring.count()]));
            }
        }
    }
    if (distance <= 0) {
        inside = GeoZone::verticalPosition(obstacle.zone->data().volumes[obstacle.volume], position) == GeoZone::VerticalPosition::Inside;
        return 0;
    }
    return (distance < _margin) ? (distance * kEndpointRelaxation) : _margin;
}

double GeoZoneRouteObstacles::_segmentMargin(const Obstacle &obstacle, int ends) const
{
    double margin = _margin;
    if (ends & StartEnd) {
        margin = (std::min)(margin, obstacle.startMargin);
    }
    if (ends & EndEnd) {
        margin = (std::min)(margin, obstacle.endMargin);
    }
    return margin;
}

/// @return true if the segment comes closer to the item of the obstacle than the margin
bool GeoZoneRouteObstacles::_blocks(const Obstacle &obstacle, const Item &item, const QPointF &a, const QPointF &b, double margin)
{
    if (margin <= kTolerance) {
        return false;
    }
    const double reach = margin + (obstacle.circle ? obstacle.radius : 0);
    if (((std::max)(a.x(), b.x()) < ((std::min)(item.a.x(), item.b.x()) - reach)) || ((std::min)(a.x(), b.x()) > ((std::max)(item.a.x(), item.b.x()) + reach)) ||
        ((std::max)(a.y(), b.y()) < ((std::min)(item.a.y(), item.b.y()) - reach)) || ((std::min)(a.y(), b.y()) > ((std::max)(item.a.y(), item.b.y()) + reach))) {
        return false;
    }
    const double distance = obstacle.circle ? (_pointSegmentDistance(obstacle.center, a, b) - obstacle.radius) : _segmentDistance(a, b, item.a, item.b);
    return distance < (margin - kTolerance);
}

bool GeoZoneRouteObstacles::blocks(int index, const QPointF &point)
{
    if (!_obstacles[index].bounds.contains(point)) {
        return false;
    }
    build(index);
    const Obstacle &obstacle = _obstacles[index];
    if (obstacle.circle) {
        return length(point - obstacle.center) < (obstacle.radius + _margin - kTolerance);
    }
    if (!obstacle.rings.isEmpty() && GeoZoneGeometry::polygonContains(obstacle.rings, point)) {
        return true;
    }
    for (const QList<QPointF> &ring : obstacle.rings) {
        for (qsizetype i = 0; i < ring.count(); i++) {
            if (_pointSegmentDistance(point, ring[i], ring[(i + 1) % ring.count()]) < (_margin - kTolerance)) {
                return true;
            }
        }
    }
    return false;
}

QList<int> GeoZoneRouteObstacles::pointBlockers(const QPointF &point)
{
    _buildAt(point);
    QList<int> blockers;
    (void) _obstacleGrid.visitPoint(point, [&](int index) {
        const Obstacle &obstacle = _obstacles[index];
        if (obstacle.bounds.contains(point) &&
            (obstacle.circle ? (length(point - obstacle.center) < (obstacle.radius + _margin - kTolerance))
                             : (!obstacle.rings.isEmpty() && GeoZoneGeometry::polygonContains(obstacle.rings, point)))) {
            blockers.append(index);
        }
        return false;
    });
    (void) _edgeGrid.visitPoint(point, [&](int index) {
        const Item &item = _items[index];
        if (!_obstacles[item.obstacle].circle && !blockers.contains(item.obstacle) && (_pointSegmentDistance(point, item.a, item.b) < (_margin - kTolerance))) {
            blockers.append(item.obstacle);
        }
        return false;
    });
    return blockers;
}

int GeoZoneRouteObstacles::firstBlocker(const QPointF &a, const QPointF &b, int ends, int minRank)
{
    _buildAlong(a, b);
    int blocker = -1;
    (void) _edgeGrid.visitSegment(a, b, [&](int index) {
        const Item &item = _items[index];
        const Obstacle &obstacle = _obstacles[item.obstacle];
        if ((obstacle.rank < 0) || (obstacle.rank < minRank) || !_blocks(obstacle, item, a, b, _segmentMargin(obstacle, ends))) {
            return false;
        }
        blocker = item.obstacle;
        return true;
    });
    return blocker;
}

QList<int> GeoZoneRouteObstacles::hits(const QPointF &a, const QPointF &b, int ends)
{
    _buildAlong(a, b);
    QList<int> hits;
    (void) _edgeGrid.visitSegment(a, b, [&](int index) {
        const Item &item = _items[index];
        const Obstacle &obstacle = _obstacles[item.obstacle];
        if ((obstacle.rank < 0) && !hits.contains(item.obstacle) && _blocks(obstacle, item, a, b, _segmentMargin(obstacle, ends))) {
            hits.append(item.obstacle);
        }
        return false;
    });
    return hits;
}

QList<QPointF> GeoZoneRouteObstacles::cornerNodes(int index)
{
    build(index);
    const Obstacle &obstacle = _obstacles[index];
    QList<QPointF> nodes;
    if (obstacle.circle) {
        const double grown = obstacle.radius + _margin;
        const int count = std::clamp(static_cast<int>(std::ceil(M_PI / std::acos(grown / (grown + _margin)))), kMinCircleNodes, kMaxCircleNodes);
        const double step = (2 * M_PI) / count;
        const double radius = (grown / std::cos(step / 2)) + kNodeOffset;
        for (int i = 0; i < count; i++) {
            const double angle = (i + 0.5) * step;
            nodes.append(obstacle.center + QPointF(radius * std::cos(angle), radius * std::sin(angle)));
        }
        return nodes;
    }
    for (const QList<QPointF> &ring : obstacle.rings) {
        const qsizetype count = ring.count();
        for (qsizetype i = 0; i < count; i++) {
            const QPointF &vertex = ring[i];
            const QPointF incoming = vertex - ring[(i + count - 1) % count];
            const QPointF outgoing = ring[(i + 1) % count] - vertex;
            const double incomingLength = length(incoming);
            const double outgoingLength = length(outgoing);
            // Paths only bend around convex corners, where the zone is on the inside of the turn
            if ((incomingLength <= 0) || (outgoingLength <= 0) || (cross(incoming, outgoing) <= (1e-9 * incomingLength * outgoingLength))) {
                continue;
            }
            const QPointF incomingNormal(incoming.y() / incomingLength, -incoming.x() / incomingLength);
            const QPointF outgoingNormal(outgoing.y() / outgoingLength, -outgoing.x() / outgoingLength);
            const double turn = std::atan2(cross(incomingNormal, outgoingNormal), dot(incomingNormal, outgoingNormal));
            const int steps = (std::max)(1, static_cast<int>(std::ceil(turn / kMaxArcStep)));
            const double step = turn / steps;
            const double radius = (_margin / std::cos(step / 2)) + kNodeOffset;
            const double base = std::atan2(incomingNormal.y(), incomingNormal.x());
            for (int j = 0; j < steps; j++) {
                const double angle = base + ((j + 0.5) * step);
                nodes.append(vertex + QPointF(radius * std::cos(angle), radius * std::sin(angle)));
            }
        }
    }
    return nodes;
}
