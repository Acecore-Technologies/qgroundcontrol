/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRouteSearch.h"
#include "GeoZoneBox.h"
#include "GeoZoneGeometry.h"

#include <QtCore/QHash>
#include <QtCore/QRectF>
#include <QtCore/QSet>
#include <QtCore/QtMath>

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

struct GeoZoneRouteSearch::Cache {
    double tolerance = 0;
    QHash<std::pair<const GeoZone*, int>, GeoZone::Ring> outerRings;    ///< Simplified outward, by zone and volume
};

namespace {

constexpr double kTolerance = 0.01;             ///< Meters segments may come closer to the zones than the margin, for rounding
constexpr double kNodeOffset = 0.5;             ///< Meters nodes are placed beyond the grown zones, so that edges along them pass
constexpr double kMaxArcStep = M_PI / 4;        ///< Radians between the nodes around a convex corner
constexpr int kMinCircleNodes = 8;
constexpr int kMaxCircleNodes = 32;
constexpr int kGridCells = 64;                  ///< Cells of the grids along each side
constexpr int kMaxRounds = 64;                  ///< Searches of the graph of a detour
constexpr int kMaxNodes = 20000;
constexpr int kMaxVerticalRounds = 8;           ///< Climbs and descents raising the targets for the zones they enter
constexpr int kMaxSamples = 2000;
constexpr double kEndpointRelaxation = 0.9;
constexpr double kShortDetourFactor = 1.3;      ///< Detours searched first are at most this many times as long as the leg, or
constexpr double kShortDetourAllowance = 1000;  ///< this many meters longer, whichever is more     ///< Of the distance to a zone that segments from an end within the margin keep
constexpr qsizetype kMinSimplifiedVertices = 16;
constexpr double kUnlimitedCeiling = 99999;     ///< Published as upper limit when a zone has no ceiling, see GeoZone

using Statistics = GeoZoneRouteSearch::Statistics;

double _dot(const QPointF &a, const QPointF &b)
{
    return (a.x() * b.x()) + (a.y() * b.y());
}

double _cross(const QPointF &a, const QPointF &b)
{
    return (a.x() * b.y()) - (a.y() * b.x());
}

double _length(const QPointF &vector)
{
    return std::hypot(vector.x(), vector.y());
}

double _pointSegmentDistance(const QPointF &point, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double lengthSquared = _dot(ab, ab);
    const double t = (lengthSquared > 0) ? std::clamp(_dot(point - a, ab) / lengthSquared, 0.0, 1.0) : 0.0;
    return _length(point - (a + (ab * t)));
}

/// @return true if the segments cross at a point inside both
bool _segmentsCross(const QPointF &a, const QPointF &b, const QPointF &c, const QPointF &d)
{
    const double d1 = _cross(b - a, c - a);
    const double d2 = _cross(b - a, d - a);
    const double d3 = _cross(d - c, a - c);
    const double d4 = _cross(d - c, b - c);
    return (((d1 > 0) && (d2 < 0)) || ((d1 < 0) && (d2 > 0))) && (((d3 > 0) && (d4 < 0)) || ((d3 < 0) && (d4 > 0)));
}

double _segmentDistance(const QPointF &a, const QPointF &b, const QPointF &c, const QPointF &d)
{
    if (_segmentsCross(a, b, c, d)) {
        return 0;
    }
    return (std::min)({ _pointSegmentDistance(a, c, d), _pointSegmentDistance(b, c, d), _pointSegmentDistance(c, a, b), _pointSegmentDistance(d, a, b) });
}

/// Uniform grid of items over an extent, items and queries beyond it fall in the cells along its sides
class Grid
{
public:
    void reset(const QRectF &extent)
    {
        _extent = extent;
        _cellWidth = (std::max)(extent.width() / kGridCells, 1.0);
        _cellHeight = (std::max)(extent.height() / kGridCells, 1.0);
        _cells.assign(static_cast<size_t>(kGridCells) * kGridCells, {});
        _stamps.clear();
        _stamp = 0;
    }

    void insertRect(int item, const QRectF &bounds)
    {
        const int firstColumn = _column(bounds.left());
        const int lastColumn = _column(bounds.right());
        for (int row = _row(bounds.top()); row <= _row(bounds.bottom()); row++) {
            for (int column = firstColumn; column <= lastColumn; column++) {
                _cells[(row * kGridCells) + column].push_back(item);
            }
        }
        _grow(item);
    }

    /// Inserts the item in the cells within the distance of the segment
    void insertSegment(int item, const QPointF &a, const QPointF &b, double distance)
    {
        (void) _visitCells(a, b, distance, [this, item](int cell) {
            _cells[cell].push_back(item);
            return false;
        });
        _grow(item);
    }

    /// Calls the visitor once with each item of the cells along the segment, until it returns true
    /// @return true if the visitor did
    template<typename Visitor>
    bool visitSegment(const QPointF &a, const QPointF &b, Visitor visitor) const
    {
        if (++_stamp == 0) {
            std::fill(_stamps.begin(), _stamps.end(), 0);
            _stamp = 1;
        }
        return _visitCells(a, b, 0, [this, &visitor](int cell) {
            for (const int item : _cells[cell]) {
                if (_stamps[item] != _stamp) {
                    _stamps[item] = _stamp;
                    if (visitor(item)) {
                        return true;
                    }
                }
            }
            return false;
        });
    }

    /// Calls the visitor with each item of the cell of the point, until it returns true
    /// @return true if the visitor did
    template<typename Visitor>
    bool visitPoint(const QPointF &point, Visitor visitor) const
    {
        for (const int item : _cells[(_row(point.y()) * kGridCells) + _column(point.x())]) {
            if (visitor(item)) {
                return true;
            }
        }
        return false;
    }

private:
    int _column(double x) const
    {
        const double column = std::floor((x - _extent.left()) / _cellWidth);
        return static_cast<int>(std::clamp(std::isnan(column) ? 0.0 : column, 0.0, static_cast<double>(kGridCells - 1)));
    }

    int _row(double y) const
    {
        const double row = std::floor((y - _extent.top()) / _cellHeight);
        return static_cast<int>(std::clamp(std::isnan(row) ? 0.0 : row, 0.0, static_cast<double>(kGridCells - 1)));
    }

    void _grow(int item)
    {
        if (static_cast<size_t>(item) >= _stamps.size()) {
            _stamps.resize(static_cast<size_t>(item) + 1, 0);
        }
    }

    /// Calls the function with the cells within the distance of the segment, row by row, until it returns true
    template<typename Function>
    bool _visitCells(const QPointF &a, const QPointF &b, double distance, Function function) const
    {
        constexpr double infinity = std::numeric_limits<double>::infinity();
        const int lastRow = _row((std::max)(a.y(), b.y()) + distance);
        for (int row = _row((std::min)(a.y(), b.y()) - distance); row <= lastRow; row++) {
            // The part of the segment in the band of the row, widened by the distance. The outer rows extend to infinity.
            const double bandLow = (row == 0) ? -infinity : (_extent.top() + (row * _cellHeight) - distance);
            const double bandHigh = (row == (kGridCells - 1)) ? infinity : (_extent.top() + ((row + 1) * _cellHeight) + distance);
            double low = (std::min)(a.x(), b.x());
            double high = (std::max)(a.x(), b.x());
            const double dy = b.y() - a.y();
            if (std::abs(dy) > 1e-12) {
                double t0 = (bandLow - a.y()) / dy;
                double t1 = (bandHigh - a.y()) / dy;
                if (t0 > t1) {
                    std::swap(t0, t1);
                }
                t0 = (std::max)(t0, 0.0);
                t1 = (std::min)(t1, 1.0);
                if (t0 > t1) {
                    continue;
                }
                low = a.x() + ((b.x() - a.x()) * t0);
                high = a.x() + ((b.x() - a.x()) * t1);
                if (low > high) {
                    std::swap(low, high);
                }
            }
            const int lastColumn = _column(high + distance);
            for (int column = _column(low - distance); column <= lastColumn; column++) {
                if (function((row * kGridCells) + column)) {
                    return true;
                }
            }
        }
        return false;
    }

    QRectF _extent;
    double _cellWidth = 1;
    double _cellHeight = 1;
    std::vector<std::vector<int>> _cells;
    mutable std::vector<quint32> _stamps;   ///< Of the items, the last query visiting them
    mutable quint32 _stamp = 0;
};

/// Shortest path from a start to an end keeping a margin from obstacles, the volumes of zones. Only the obstacles the
/// paths run into are added to the graph: each round searches the graph, then adds the obstacles the path runs into
/// until it runs into none.
class DetourSearch
{
public:
    enum class Outcome {
        Found,
        StartInside,
        EndInside,
        Enclosed,
        TooLong,
        NotFound,
        EntersZone,
        Cancelled,
    };

    /// @return Zones the path through the coordinates enters
    using Verify = std::function<QList<const GeoZone*>(const QList<QGeoCoordinate> &path)>;

    DetourSearch(const GeoZone::Position &from, const GeoZone::Position &to, double margin, double maxLength, const GeoZoneCancelToken &cancel,
                 GeoZoneRouteSearch::Cache &cache, Statistics &statistics)
        : _from(from)
        , _to(to)
        , _margin(margin)
        , _maxLength(maxLength)
        , _cancel(cancel)
        , _cache(cache)
        , _statistics(statistics)
        , _projection(from.coordinate.atDistanceAndAzimuth(from.coordinate.distanceTo(to.coordinate) / 2, from.coordinate.azimuthTo(to.coordinate)))
    {
        _start = _projection.toLocal(from.coordinate);
        _end = _projection.toLocal(to.coordinate, _start);
        _directLength = _length(_end - _start);
        if (!std::isinf(_maxLength)) {
            // Bounds of the ellipse with the ends as foci, which contains all paths within the length limit
            const QPointF center = (_start + _end) / 2;
            const double major = _maxLength / 2;
            const double focal = _directLength / 2;
            const double minor = std::sqrt((std::max)((major * major) - (focal * focal), 0.0));
            const double angle = std::atan2(_end.y() - _start.y(), _end.x() - _start.x());
            const double halfWidth = std::hypot(major * std::cos(angle), minor * std::sin(angle)) + _margin;
            const double halfHeight = std::hypot(major * std::sin(angle), minor * std::cos(angle)) + _margin;
            _reach = QRectF(center.x() - halfWidth, center.y() - halfHeight, 2 * halfWidth, 2 * halfHeight);
        }
    }

    double directLength() const { return _directLength; }

    /// @return Bounds of the paths within the length limit, invalid without limit or when they span the earth
    GeoZoneBox reachBox() const
    {
        constexpr double maxReach = 5000000;
        if (_reach.isNull() || (_reach.width() > maxReach) || (_reach.height() > maxReach)) {
            return GeoZoneBox();
        }
        const QGeoCoordinate southWest = _projection.toGeo(_reach.topLeft());
        const QGeoCoordinate northEast = _projection.toGeo(_reach.bottomRight());
        return GeoZoneBox(std::clamp(southWest.latitude(), -90.0, 90.0), southWest.longitude(), std::clamp(northEast.latitude(), -90.0, 90.0), northEast.longitude());
    }

    /// Adds a volume of a zone as an obstacle, before run
    void addObstacle(const GeoZone *zone, int volume)
    {
        (void) _createObstacle(zone, volume);
    }

    /// Searches the detour, verifying the paths keeping the margin with the function when set
    Outcome run(const Verify &verify)
    {
        _prepare();
        if (!_startInside.isEmpty()) {
            _blockingZones = _startInside;
            return Outcome::StartInside;
        }
        if (!_endInside.isEmpty()) {
            _blockingZones = _endInside;
            return Outcome::EndInside;
        }
        _nodes = { Node{ _start, _directLength, true, true }, Node{ _end, 0, true, true } };
        _statistics.nodes += 2;

        // The obstacles the straight line runs into
        _include(_hits(0, 1));
        for (int round = 0; round < kMaxRounds; round++) {
            if (_cancel.isCancelled()) {
                return Outcome::Cancelled;
            }
            _statistics.rounds++;
            QSet<int> blockers;
            const QList<int> path = _astar(true, &blockers);
            if (_cancel.isCancelled()) {
                return Outcome::Cancelled;
            }
            if (path.isEmpty()) {
                if (_nodeLimitReached) {
                    return Outcome::NotFound;
                }
                // The obstacles which left out nodes around those of the graph may leave a way around them
                QList<int> pending;
                for (const int obstacle : std::as_const(_nodeBlockers)) {
                    if (_obstacles[obstacle].rank < 0) {
                        pending.append(obstacle);
                    }
                }
                _nodeBlockers.clear();
                if (!pending.isEmpty()) {
                    _include(pending);
                    continue;
                }
                if (!std::isinf(_maxLength)) {
                    // Whether a longer detour goes around the obstacles found so far
                    QSet<int> allBlockers;
                    const QList<int> longer = _astar(false, &allBlockers);
                    if (_cancel.isCancelled()) {
                        return Outcome::Cancelled;
                    }
                    if (!longer.isEmpty()) {
                        _pathLength = _lengthOf(longer);
                        _blockingZones = _zones(blockers);
                        return Outcome::TooLong;
                    }
                    blockers = allBlockers;
                }
                _blockingZones = _zones(blockers);
                return Outcome::Enclosed;
            }

            QList<int> hits;
            for (qsizetype i = 1; i < path.count(); i++) {
                for (const int obstacle : _hits(path[i - 1], path[i])) {
                    if (!hits.contains(obstacle)) {
                        hits.append(obstacle);
                    }
                }
            }
            if (!hits.isEmpty()) {
                _include(hits);
                continue;
            }

            const QList<QGeoCoordinate> coordinates = _coordinates(path);
            const QList<const GeoZone*> entered = verify ? verify(coordinates) : QList<const GeoZone*>();
            if (entered.isEmpty()) {
                _waypoints = coordinates.mid(1, coordinates.count() - 2);
                _pathLength = _lengthOf(path);
                return Outcome::Found;
            }
            // Zones whose volumes were left out as above or below the leg, or entered despite the margin
            if (_includeZones(entered) == 0) {
                _blockingZones = entered;
                return Outcome::EntersZone;
            }
        }
        return Outcome::NotFound;
    }

    const QList<QGeoCoordinate> &waypoints() const { return _waypoints; }
    /// Meters from the start to the end, of the shortest detour found when TooLong
    double length() const { return _pathLength; }
    /// Zones the start or the end is inside, which surround them, or which the path enters
    const QList<const GeoZone*> &blockingZones() const { return _blockingZones; }

private:
    struct Obstacle {
        const GeoZone *zone = nullptr;
        int volume = 0;
        bool circle = false;
        QPointF center;                     ///< Circles only
        double radius = 0;                  ///< Circles only
        QList<QList<QPointF>> rings;        ///< Polygons: the outer ring counterclockwise then the holes clockwise, the zone left of the edges
        QRectF bounds;                      ///< Grown by the margin, from the bounding box of the volume
        bool built = false;                 ///< The rings are made and the edges are in the grid, see _build
        int rank = -1;                      ///< Order in which the obstacle was added to the graph, -1 while it isn't
        double startMargin = 0;             ///< Of segments from the start, less than the margin when the start is within it
        double endMargin = 0;
    };

    /// Edge of a polygon, or circle, in the grid
    struct Item {
        int obstacle = -1;
        QPointF a;
        QPointF b;
    };

    struct Node {
        QPointF point;
        double toEnd = 0;       ///< Distance to the end, the heuristic of A*
        bool inReach = true;    ///< Paths through the node can be within the length limit
        bool valid = true;      ///< Keeps the margin from all obstacles
    };

    /// Visibility between two nodes, against the obstacles of the graph
    struct Visibility {
        int checkedRank = 0;    ///< Obstacles of lower rank were checked
        int blocker = -1;       ///< Obstacle blocking the segment, -1 when none of those checked does
    };

    /// Adds an obstacle with its bounds, its rings are made when a query comes near, see _build
    /// @return Index of the obstacle, -1 for an empty volume
    int _createObstacle(const GeoZone *zone, int volume)
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
        _obstacleIndexes.insert({ zone, volume }, index);
        _statistics.obstacles++;
        if (_prepared) {
            _insert(index);
        }
        return index;
    }

    /// Makes the rings of the obstacle and adds its edges to the grid, and sets the margins of segments from the ends
    void _build(int index)
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
    void _buildAlong(const QPointF &a, const QPointF &b)
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
            _build(index);
        }
    }

    /// Builds the obstacles whose bounds contain the point
    void _buildAt(const QPointF &point)
    {
        QList<int> unbuilt;
        (void) _obstacleGrid.visitPoint(point, [&](int index) {
            if (!_obstacles[index].built && _obstacles[index].bounds.contains(point)) {
                unbuilt.append(index);
            }
            return false;
        });
        for (const int index : std::as_const(unbuilt)) {
            _build(index);
        }
    }

    /// @return Outer ring of the polygon volume in degrees, simplified outward by a quarter of the margin when detailed,
    ///         so that it extends up to half the margin beyond the zone
    GeoZone::Ring _outerRing(const GeoZone *zone, int volume)
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

    void _prepare()
    {
        QRectF extent = QRectF(_start, _end).normalized().adjusted(-_margin, -_margin, _margin, _margin);
        for (const Obstacle &obstacle : std::as_const(_obstacles)) {
            extent = extent.united(obstacle.bounds);
        }
        if (!_reach.isNull()) {
            extent = extent.intersected(_reach.united(QRectF(_start, _end).normalized()));
        }
        _edgeGrid.reset(extent);
        _obstacleGrid.reset(extent);
        _prepared = true;
        for (int index = 0; index < _obstacles.count(); index++) {
            _insert(index);
        }
    }

    /// Adds the obstacle to the grid of obstacles, and builds it when its bounds contain an end
    void _insert(int index)
    {
        _obstacleGrid.insertRect(index, _obstacles[index].bounds);
        if (_obstacles[index].bounds.contains(_start) || _obstacles[index].bounds.contains(_end)) {
            _build(index);
        }
    }

    /// @return Margin segments from the end keep from the obstacle: less than the margin when the end is within it,
    ///         0 when the end is inside the volume horizontally but above or below it
    /// @param inside Set when the end is inside the volume
    double _endpointMargin(const Obstacle &obstacle, const QPointF &point, const GeoZone::Position &position, bool &inside) const
    {
        inside = false;
        double distance = 0;
        if (obstacle.circle) {
            distance = _length(point - obstacle.center) - obstacle.radius;
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

    double _segmentMargin(const Obstacle &obstacle, int from, int to) const
    {
        double margin = _margin;
        if ((from == 0) || (to == 0)) {
            margin = (std::min)(margin, obstacle.startMargin);
        }
        if ((from == 1) || (to == 1)) {
            margin = (std::min)(margin, obstacle.endMargin);
        }
        return margin;
    }

    /// @return true if the segment comes closer to the item of the obstacle than the margin
    static bool _blocks(const Obstacle &obstacle, const Item &item, const QPointF &a, const QPointF &b, double margin)
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

    /// @return true if the point is inside the obstacle grown by the margin
    bool _blockedBy(int index, const QPointF &point)
    {
        if (!_obstacles[index].bounds.contains(point)) {
            return false;
        }
        _build(index);
        const Obstacle &obstacle = _obstacles[index];
        if (obstacle.circle) {
            return _length(point - obstacle.center) < (obstacle.radius + _margin - kTolerance);
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

    /// @return Obstacles the point is inside of or within the margin of
    QList<int> _pointBlockers(const QPointF &point)
    {
        _buildAt(point);
        QList<int> blockers;
        (void) _obstacleGrid.visitPoint(point, [&](int index) {
            const Obstacle &obstacle = _obstacles[index];
            if (obstacle.bounds.contains(point) &&
                (obstacle.circle ? (_length(point - obstacle.center) < (obstacle.radius + _margin - kTolerance))
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

    /// @return First obstacle of the graph of the rank or higher blocking the segment between the nodes, -1 without
    int _firstBlocker(int from, int to, int minRank)
    {
        const QPointF a = _nodes[from].point;
        const QPointF b = _nodes[to].point;
        _buildAlong(a, b);
        int blocker = -1;
        (void) _edgeGrid.visitSegment(a, b, [&](int index) {
            const Item &item = _items[index];
            const Obstacle &obstacle = _obstacles[item.obstacle];
            if ((obstacle.rank < 0) || (obstacle.rank < minRank) || !_blocks(obstacle, item, a, b, _segmentMargin(obstacle, from, to))) {
                return false;
            }
            blocker = item.obstacle;
            return true;
        });
        return blocker;
    }

    /// @return Obstacles outside the graph blocking the segment between the nodes
    QList<int> _hits(int from, int to)
    {
        const QPointF a = _nodes[from].point;
        const QPointF b = _nodes[to].point;
        _buildAlong(a, b);
        QList<int> hits;
        (void) _edgeGrid.visitSegment(a, b, [&](int index) {
            const Item &item = _items[index];
            const Obstacle &obstacle = _obstacles[item.obstacle];
            if ((obstacle.rank < 0) && !hits.contains(item.obstacle) && _blocks(obstacle, item, a, b, _segmentMargin(obstacle, from, to))) {
                hits.append(item.obstacle);
            }
            return false;
        });
        return hits;
    }

    /// @return true if the segment between the nodes keeps the margin from the obstacles of the graph, from the cache
    ///         when known
    bool _visible(int from, int to, int &blocker)
    {
        const quint64 key = (static_cast<quint64>((std::min)(from, to)) << 32) | static_cast<quint32>((std::max)(from, to));
        const int rank = static_cast<int>(_included.count());
        int checkedRank = 0;
        const auto it = _visibility.constFind(key);
        if (it != _visibility.constEnd()) {
            _statistics.cachedVisibility++;
            if ((it->blocker >= 0) || (it->checkedRank == rank)) {
                blocker = it->blocker;
                return blocker < 0;
            }
            // Only the obstacles added to the graph since
            checkedRank = it->checkedRank;
        }
        _statistics.visibilityTests++;
        blocker = _firstBlocker(from, to, checkedRank);
        _visibility.insert(key, Visibility{ rank, blocker });
        return blocker < 0;
    }

    /// Adds the obstacles to the graph, with nodes around their convex corners outside the other obstacles. The
    /// obstacles leaving out nodes are noted, since the graph must go around them when they close the gaps between
    /// those of the graph.
    void _include(const QList<int> &indexes)
    {
        for (const int index : indexes) {
            _build(index);
            if (_obstacles[index].rank >= 0) {
                continue;
            }
            _obstacles[index].rank = static_cast<int>(_included.count());
            _included.append(index);
            _statistics.includedObstacles++;
            for (const QPointF &point : _cornerNodes(_obstacles[index])) {
                if (_nodes.count() >= kMaxNodes) {
                    _nodeLimitReached = true;
                    return;
                }
                const QList<int> blockers = _pointBlockers(point);
                if (blockers.isEmpty()) {
                    const double toEnd = _length(_end - point);
                    _nodes.append(Node{ point, toEnd, (_length(point - _start) + toEnd) <= _maxLength, true });
                    _statistics.nodes++;
                    continue;
                }
                for (const int blocker : blockers) {
                    if (_obstacles[blocker].rank < 0) {
                        _nodeBlockers.insert(blocker);
                    }
                }
            }
        }
    }

    /// @return Positions of the nodes around the convex corners of the obstacle, on the arcs of the margin, such that the
    ///         edges between them keep the margin
    QList<QPointF> _cornerNodes(const Obstacle &obstacle) const
    {
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
                const double incomingLength = _length(incoming);
                const double outgoingLength = _length(outgoing);
                // Paths only bend around convex corners, where the zone is on the inside of the turn
                if ((incomingLength <= 0) || (outgoingLength <= 0) || (_cross(incoming, outgoing) <= (1e-9 * incomingLength * outgoingLength))) {
                    continue;
                }
                const QPointF incomingNormal(incoming.y() / incomingLength, -incoming.x() / incomingLength);
                const QPointF outgoingNormal(outgoing.y() / outgoingLength, -outgoing.x() / outgoingLength);
                const double turn = std::atan2(_cross(incomingNormal, outgoingNormal), _dot(incomingNormal, outgoingNormal));
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

    /// Adds all volumes of the zones to the graph
    /// @return Number of obstacles added
    int _includeZones(const QList<const GeoZone*> &zones)
    {
        int added = 0;
        for (const GeoZone *zone : zones) {
            for (int volume = 0; volume < zone->data().volumes.count(); volume++) {
                int index = _obstacleIndexes.value({ zone, volume }, -1);
                if (index < 0) {
                    index = _createObstacle(zone, volume);
                    if (index < 0) {
                        continue;
                    }
                    // Nodes are outside the obstacles, also this one
                    for (qsizetype node = 2; node < _nodes.count(); node++) {
                        if (_nodes[node].valid && _blockedBy(index, _nodes[node].point)) {
                            _nodes[node].valid = false;
                        }
                    }
                }
                if (_obstacles[index].rank < 0) {
                    _include({ index });
                    added++;
                }
            }
        }
        return added;
    }

    /// @return Nodes of the shortest path from the start to the end, empty without. Edges are tested lazily, like by
    ///         Lazy Weighted A*: candidate edges are queued by the estimated length of the paths through them, and only
    ///         tested when they would reach a node first.
    /// @param limited  Only paths within the length limit
    /// @param blockers Obstacles blocking the edges tested
    QList<int> _astar(bool limited, QSet<int> *blockers)
    {
        const QList<Node> &nodes = _nodes;
        const qsizetype count = nodes.count();
        std::vector<int> previous(count, -1);
        std::vector<char> closed(count, 0);
        // Each expanded node with its edges to the other nodes by the estimated length of the paths through them, which
        // the queue takes one at a time, so that it holds an entry per expanded node
        struct Expansion {
            int node = -1;
            double distance = 0;
            std::vector<std::pair<double, int>> edges;
            size_t next = 0;
        };
        std::vector<Expansion> expansions;
        using Entry = std::pair<double, size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
        const auto expand = [&](int node, double distance) {
            Expansion expansion{ node, distance, {}, 0 };
            const QPointF point = nodes[node].point;
            for (int next = 1; next < count; next++) {
                const Node &other = nodes[next];
                if (closed[next] || !other.valid || (limited && !other.inReach)) {
                    continue;
                }
                const double estimate = distance + _length(other.point - point) + other.toEnd;
                if (!limited || (estimate <= (_maxLength + kTolerance))) {
                    expansion.edges.emplace_back(estimate, next);
                }
            }
            if (expansion.edges.empty()) {
                return;
            }
            std::sort(expansion.edges.begin(), expansion.edges.end());
            open.push({ expansion.edges.front().first, expansions.size() });
            expansions.push_back(std::move(expansion));
        };
        closed[0] = 1;
        expand(0, 0);
        while (!open.empty() && !closed[1]) {
            const size_t index = open.top().second;
            open.pop();
            const int from = expansions[index].node;
            const double fromDistance = expansions[index].distance;
            const int to = expansions[index].edges[expansions[index].next].second;
            if (++expansions[index].next < expansions[index].edges.size()) {
                open.push({ expansions[index].edges[expansions[index].next].first, index });
            }
            if (closed[to]) {
                continue;
            }
            int blocker = -1;
            if (!_visible(from, to, blocker)) {
                if (blockers && (blocker >= 0)) {
                    blockers->insert(blocker);
                }
                continue;
            }
            closed[to] = 1;
            previous[to] = from;
            if (to == 1) {
                break;
            }
            if (_cancel.isCancelled()) {
                return {};
            }
            expand(to, fromDistance + _length(nodes[to].point - nodes[from].point));
        }
        if (!closed[1]) {
            return {};
        }
        QList<int> path;
        for (int node = 1; node >= 0; node = previous[node]) {
            path.prepend(node);
        }
        return path;
    }

    double _lengthOf(const QList<int> &path) const
    {
        double length = 0;
        for (qsizetype i = 1; i < path.count(); i++) {
            length += _length(_nodes[path[i]].point - _nodes[path[i - 1]].point);
        }
        return length;
    }

    QList<QGeoCoordinate> _coordinates(const QList<int> &path) const
    {
        QList<QGeoCoordinate> coordinates;
        for (const int node : path) {
            coordinates.append((node == 0) ? _from.coordinate : ((node == 1) ? _to.coordinate : _projection.toGeo(_nodes[node].point)));
        }
        return coordinates;
    }

    QList<const GeoZone*> _zones(const QSet<int> &obstacles) const
    {
        QList<const GeoZone*> zones;
        for (const int index : obstacles) {
            if (!zones.contains(_obstacles[index].zone)) {
                zones.append(_obstacles[index].zone);
            }
        }
        return zones;
    }

    const GeoZone::Position _from;
    const GeoZone::Position _to;
    const double _margin;
    const double _maxLength;
    const GeoZoneCancelToken _cancel;
    GeoZoneRouteSearch::Cache &_cache;
    Statistics &_statistics;
    const GeoZoneGeometry::LocalProjection _projection;
    QPointF _start;
    QPointF _end;
    double _directLength = 0;
    QRectF _reach;                  ///< Of the paths within the length limit, null without limit

    QList<Obstacle> _obstacles;
    QHash<std::pair<const GeoZone*, int>, int> _obstacleIndexes;
    QList<Item> _items;
    Grid _edgeGrid;                 ///< Items within the margin of the cells
    Grid _obstacleGrid;             ///< Obstacles by their grown bounds
    bool _prepared = false;
    QList<const GeoZone*> _startInside;
    QList<const GeoZone*> _endInside;

    QList<int> _included;           ///< Obstacles of the graph, by rank
    QList<Node> _nodes;             ///< The start, the end, then around the obstacles of the graph
    bool _nodeLimitReached = false;
    QSet<int> _nodeBlockers;        ///< Obstacles outside the graph which left out nodes
    QHash<quint64, Visibility> _visibility;

    QList<QGeoCoordinate> _waypoints;
    double _pathLength = 0;
    QList<const GeoZone*> _blockingZones;
};

/// @return Names of the zones, the most restrictive first
QStringList _names(QList<const GeoZone*> zones)
{
    GeoZonePathEvaluator::sortBySeverity(zones);
    QStringList names;
    for (const GeoZone *zone : std::as_const(zones)) {
        if (!names.contains(zone->displayName())) {
            names.append(zone->displayName());
        }
    }
    return names;
}

/// Sets the detour, or why there is none, from the outcome of the search
void _setOutcome(GeoZoneRouteSearch::Route &route, const DetourSearch &search, DetourSearch::Outcome outcome, const QList<const GeoZone*> &avoided,
                 double maxLength)
{
    using Failure = GeoZoneRouteSearch::Failure;
    const QList<const GeoZone*> &blocking = search.blockingZones();
    route.zones = _names(avoided);
    switch (outcome) {
    case DetourSearch::Outcome::Found:
        route.waypoints = search.waypoints();
        route.length = search.length();
        route.cost = (std::max)(route.length - search.directLength(), 0.0);
        return;
    case DetourSearch::Outcome::StartInside:
        route.failure = Failure::StartInside;
        break;
    case DetourSearch::Outcome::EndInside:
        route.failure = Failure::EndInside;
        break;
    case DetourSearch::Outcome::Enclosed:
        route.failure = Failure::Enclosed;
        break;
    case DetourSearch::Outcome::TooLong:
        route.failure = Failure::TooLong;
        route.length = search.length();
        route.limit = maxLength;
        return;
    case DetourSearch::Outcome::NotFound:
        route.failure = Failure::NotFound;
        return;
    case DetourSearch::Outcome::EntersZone:
        route.failure = Failure::EntersZone;
        break;
    case DetourSearch::Outcome::Cancelled:
        route.failure = Failure::Cancelled;
        return;
    }
    if (!blocking.isEmpty()) {
        route.zones = _names(blocking);
    }
}

} // namespace

GeoZoneRouteSearch::GeoZoneRouteSearch(const GeoZoneCatalog &catalog, const GeoZonePolicy &policy, const QDateTime &time)
    : _catalog(catalog)
    , _policy(policy)
    , _time(time)
    , _cache(std::make_shared<Cache>())
{
}

void GeoZoneRouteSearch::setTerrainLookup(const GeoZonePathEvaluator::TerrainLookup &lookup)
{
    _terrainLookup = lookup;
}

double GeoZoneRouteSearch::maxDetourLength(double directLength, const Settings &settings)
{
    return (std::max)(directLength * settings.maxDetourFactor, directLength + settings.minDetourAllowance);
}

GeoZonePathEvaluator GeoZoneRouteSearch::_evaluator() const
{
    GeoZonePathEvaluator evaluator(_catalog, _policy, _time);
    if (_terrainLookup) {
        evaluator.setTerrainLookup(_terrainLookup);
    }
    return evaluator;
}

QList<GeoZone::Position> GeoZoneRouteSearch::_samples(const Leg &leg) const
{
    const GeoZoneGeometry::LocalProjection projection(leg.from.coordinate);
    const QPointF end = projection.toLocal(leg.to.coordinate);
    const int count = std::clamp(static_cast<int>(std::ceil(_length(end) / (std::max)(_settings.sampleSpacing, 1.0))), 1, kMaxSamples);
    QList<GeoZone::Position> samples;
    samples.reserve(count + 1);
    for (int i = 0; i <= count; i++) {
        const double fraction = static_cast<double>(i) / count;
        GeoZone::Position position = (i == 0) ? leg.from : ((i == count) ? leg.to : GeoZone::Position{ projection.toGeo(end * fraction) });
        position.altitudeAMSL = leg.from.altitudeAMSL + ((leg.to.altitudeAMSL - leg.from.altitudeAMSL) * fraction);
        if (qIsNaN(position.terrainAMSL) && _terrainLookup) {
            position.terrainAMSL = _terrainLookup(position.coordinate);
        }
        samples.append(position);
    }
    return samples;
}

QList<const GeoZone*> GeoZoneRouteSearch::_enteredZones(const GeoZonePathEvaluator &evaluator, const QList<GeoZone::Position> &path, const Leg &leg) const
{
    GeoZonePathEvaluator::Path evaluatorPath = GeoZonePathEvaluator::Path::fromPositions(path);
    evaluatorPath.setTimes(leg.start, leg.end);
    QList<const GeoZone*> zones = evaluator.evaluate({ evaluatorPath }).activeEnteredZones();
    GeoZonePathEvaluator::sortBySeverity(zones);
    return zones;
}

GeoZoneRouteSearch::Result GeoZoneRouteSearch::plan(const Leg &leg) const
{
    Result result;
    if (_cancel.isCancelled()) {
        result.cancelled = true;
        return result;
    }
    const QList<GeoZone::Position> samples = _samples(leg);
    const QList<const GeoZone*> crossed = _enteredZones(_evaluator(), { samples.constFirst(), samples.constLast() }, leg);
    if (crossed.isEmpty()) {
        return result;
    }
    result.crossedZones = _names(crossed);
    for (const Kind kind : { Kind::Detour, Kind::Climb, Kind::Descent }) {
        const Route route = (kind == Kind::Detour) ? _detour(leg, samples, crossed) : _vertical(kind, leg, samples, crossed);
        if ((route.failure == Failure::Cancelled) || _cancel.isCancelled()) {
            result.cancelled = true;
            result.routes.clear();
            return result;
        }
        result.routes.append(route);
    }
    std::stable_sort(result.routes.begin(), result.routes.end(), [](const Route &a, const Route &b) {
        if (a.isValid() != b.isValid()) {
            return a.isValid();
        }
        return a.isValid() && (a.cost < b.cost);
    });
    return result;
}

GeoZoneRouteSearch::Route GeoZoneRouteSearch::detour(const Leg &leg) const
{
    const QList<GeoZone::Position> samples = _samples(leg);
    return _detour(leg, samples, _enteredZones(_evaluator(), { samples.constFirst(), samples.constLast() }, leg));
}

GeoZoneRouteSearch::Route GeoZoneRouteSearch::_detour(const Leg &leg, const QList<GeoZone::Position> &samples, const QList<const GeoZone*> &crossed) const
{
    Route route;
    route.kind = Kind::Detour;
    const GeoZone::Position &from = samples.constFirst();
    const GeoZone::Position &to = samples.constLast();

    // The volumes within reach whose limits overlap the altitudes of the leg, over the range of the terrain along it.
    // The evaluation of the paths finds those the terrain elsewhere brings closer.
    double lowTerrain = std::numeric_limits<double>::infinity();
    double highTerrain = -std::numeric_limits<double>::infinity();
    for (const GeoZone::Position &sample : samples) {
        if (!qIsNaN(sample.terrainAMSL)) {
            lowTerrain = (std::min)(lowTerrain, sample.terrainAMSL);
            highTerrain = (std::max)(highTerrain, sample.terrainAMSL);
        }
    }
    if (lowTerrain > highTerrain) {
        lowTerrain = highTerrain = qQNaN();
    }
    const GeoZone::Position low{ from.coordinate, (std::min)(from.altitudeAMSL, to.altitudeAMSL), highTerrain };
    const GeoZone::Position high{ to.coordinate, (std::max)(from.altitudeAMSL, to.altitudeAMSL), lowTerrain };
    GeoZonePolicy policy = _policy;
    if ((policy.schedule() == GeoZonePolicy::Schedule::Planned) && leg.start.isValid()) {
        const int tolerance = GeoZonePathEvaluator::defaultScheduleToleranceSecs;
        policy = policy.activeDuring(leg.start.addSecs(-tolerance), (leg.end.isValid() ? leg.end : leg.start).addSecs(tolerance));
    }

    // Detours change altitude evenly from the start to the end of the leg, like the leg
    const GeoZonePathEvaluator evaluator = _evaluator();
    const DetourSearch::Verify verify = [this, &evaluator, &leg, &from, &to](const QList<QGeoCoordinate> &path) {
        QList<double> distances = { 0 };
        for (qsizetype i = 1; i < path.count(); i++) {
            distances.append(distances.constLast() + path[i - 1].distanceTo(path[i]));
        }
        QList<GeoZone::Position> positions;
        for (qsizetype i = 0; i < path.count(); i++) {
            const double fraction = (distances.constLast() > 0) ? (distances[i] / distances.constLast()) : 0;
            positions.append(GeoZone::Position{ path[i], from.altitudeAMSL + ((to.altitudeAMSL - from.altitudeAMSL) * fraction) });
        }
        positions.first().terrainAMSL = from.terrainAMSL;
        positions.last().terrainAMSL = to.terrainAMSL;
        return _enteredZones(evaluator, positions, leg);
    };

    // Most detours are short, so the zones within the reach of a short one are searched first, which are far fewer
    const double directLength = from.coordinate.distanceTo(to.coordinate);
    const double maxLength = maxDetourLength(directLength, _settings);
    const double shortLength = (std::min)(maxLength, (std::max)(directLength * kShortDetourFactor, directLength + kShortDetourAllowance));
    for (const double length : { shortLength, maxLength }) {
        DetourSearch search(from, to, _settings.margin, length, _cancel, *_cache, route.statistics);
        for (const GeoZone *zone : _catalog.zonesInArea(search.reachBox(), policy)) {
            const QList<GeoZone::Volume> &volumes = zone->data().volumes;
            for (int volume = 0; volume < volumes.count(); volume++) {
                if (GeoZone::verticalOverlap(volumes[volume], low, high)) {
                    search.addObstacle(zone, volume);
                }
            }
        }
        const DetourSearch::Outcome outcome = search.run(verify);
        if ((outcome != DetourSearch::Outcome::TooLong) || (length >= maxLength)) {
            _setOutcome(route, search, outcome, crossed, maxLength);
            break;
        }
    }
    return route;
}

GeoZoneRouteSearch::Route GeoZoneRouteSearch::detourAround(const QGeoCoordinate &from, const QGeoCoordinate &to, const QList<const GeoZone*> &zones,
                                                           const Settings &settings, const GeoZoneCancelToken &cancel)
{
    Route route;
    route.kind = Kind::Detour;
    Cache cache;
    DetourSearch search(GeoZone::Position{ from }, GeoZone::Position{ to }, settings.margin, std::numeric_limits<double>::infinity(), cancel, cache, route.statistics);
    for (const GeoZone *zone : zones) {
        for (int volume = 0; volume < zone->data().volumes.count(); volume++) {
            search.addObstacle(zone, volume);
        }
    }
    _setOutcome(route, search, search.run(DetourSearch::Verify()), zones, std::numeric_limits<double>::infinity());
    return route;
}

GeoZoneRouteSearch::Route GeoZoneRouteSearch::_vertical(Kind kind, const Leg &leg, const QList<GeoZone::Position> &samples, const QList<const GeoZone*> &crossed) const
{
    Route route;
    route.kind = kind;
    route.zones = _names(crossed);
    const bool climb = (kind == Kind::Climb);
    const double fromAltitude = samples.constFirst().altitudeAMSL;
    const double toAltitude = samples.constLast().altitudeAMSL;
    if (qIsNaN(fromAltitude) || qIsNaN(toAltitude)) {
        route.failure = Failure::AltitudeUnknown;
        return route;
    }

    const GeoZonePathEvaluator evaluator = _evaluator();
    const GeoZonePathEvaluator::Path path = GeoZonePathEvaluator::Path::fromPositions({ samples.constFirst(), samples.constLast() });
    const qsizetype last = samples.count() - 1;
    QList<const GeoZone*> avoided;
    QList<const GeoZone*> blocking = crossed;
    double target = climb ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    for (int round = 0; round < kMaxVerticalRounds; round++) {
        if (_cancel.isCancelled()) {
            route.failure = Failure::Cancelled;
            return route;
        }

        // Above the highest ceiling or below the lowest floor where the leg passes the zones horizontally. Limits above
        // ground follow the terrain, which is linear between the samples.
        for (const GeoZone *zone : std::as_const(blocking)) {
            avoided.append(zone);
            for (const GeoZonePathEvaluator::Stretch &stretch : evaluator.evaluateZone(zone, { path }).stretches) {
                const GeoZone::Volume &volume = zone->data().volumes[stretch.volume];
                // The samples between the entry and the exit, along the same line
                QList<GeoZone::Position> positions = { stretch.entry.position, stretch.exit.position };
                const auto firstSample = static_cast<qsizetype>(std::ceil(stretch.entry.fraction * last));
                const auto lastSample = static_cast<qsizetype>(std::floor(stretch.exit.fraction * last));
                for (qsizetype i = (std::max)(firstSample, qsizetype(0)); i <= (std::min)(lastSample, last); i++) {
                    positions.append(samples[i]);
                }
                for (const GeoZone::Position &position : std::as_const(positions)) {
                    const double limit = GeoZone::limitAMSL(volume, climb, position);
                    if (!qIsNaN(limit)) {
                        target = climb ? (std::max)(target, limit + _settings.verticalMargin) : (std::min)(target, limit - _settings.verticalMargin);
                        continue;
                    }
                    route.zones = QStringList{ zone->displayName() };
                    const double published = climb ? volume.upperLimit : volume.lowerLimit;
                    const GeoZone::VerticalReference reference = climb ? volume.upperReference : volume.lowerReference;
                    if (!climb && (qIsNaN(published) || ((reference == GeoZone::VerticalReference::Ground) && (published <= 0)))) {
                        route.failure = Failure::ReachesGround;
                    } else if ((reference == GeoZone::VerticalReference::Ground) && !qIsNaN(published) && (published < kUnlimitedCeiling) &&
                               qIsNaN(position.terrainAMSL)) {
                        route.failure = Failure::TerrainUnknown;
                    } else {
                        route.failure = climb ? Failure::NoCeiling : Failure::NoFloor;
                    }
                    return route;
                }
            }
        }
        route.zones = _names(avoided);
        if (std::isinf(target)) {
            route.failure = Failure::EntersZone;
            return route;
        }

        // The start and the end move to the target when they are below it for climbs, above it for descents
        const double newFrom = climb ? (std::max)(fromAltitude, target) : (std::min)(fromAltitude, target);
        const double newTo = climb ? (std::max)(toAltitude, target) : (std::min)(toAltitude, target);
        QList<GeoZone::Position> moved = samples;
        double extreme = climb ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        for (qsizetype i = 0; i <= last; i++) {
            const double fraction = static_cast<double>(i) / last;
            moved[i].altitudeAMSL = newFrom + ((newTo - newFrom) * fraction);
            if (std::abs(moved[i].altitudeAMSL - samples[i].altitudeAMSL) < kTolerance) {
                continue;
            }
            if (qIsNaN(samples[i].terrainAMSL)) {
                route.failure = Failure::TerrainUnknown;
                return route;
            }
            const double height = moved[i].altitudeAMSL - samples[i].terrainAMSL;
            extreme = climb ? (std::max)(extreme, height) : (std::min)(extreme, height);
        }
        route.altitude = target;
        if (climb && !qIsNaN(_settings.maxAltitudeAGL) && (extreme > (_settings.maxAltitudeAGL + kTolerance))) {
            route.failure = Failure::AboveMaxAltitude;
            route.limit = extreme;
            return route;
        }
        if (!climb && (extreme < (_settings.minGroundClearance - kTolerance))) {
            route.failure = Failure::BelowMinClearance;
            route.limit = extreme;
            return route;
        }

        const QList<const GeoZone*> entered = _enteredZones(evaluator, { moved.constFirst(), moved.constLast() }, leg);
        if (entered.isEmpty()) {
            route.fromAltitude = newFrom;
            route.toAltitude = newTo;
            // The vehicle climbs to the new altitudes and descends back after the leg, or the other way round
            const double change = (std::max)(std::abs(newFrom - fromAltitude), std::abs(newTo - toAltitude));
            route.cost = (_settings.climbWeight + _settings.descentWeight) * change;
            return route;
        }
        blocking.clear();
        for (const GeoZone *zone : entered) {
            if (!avoided.contains(zone)) {
                blocking.append(zone);
            }
        }
        if (blocking.isEmpty()) {
            route.failure = Failure::EntersZone;
            route.zones = _names(entered);
            return route;
        }
    }
    route.failure = Failure::EntersZone;
    route.zones = _names(blocking);
    return route;
}
