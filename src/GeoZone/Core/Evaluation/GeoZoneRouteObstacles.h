/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "GeoZone.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneRouteSearch.h"

/// Zone outlines simplified for the margin, which the searches of a GeoZoneRouteSearch and its copies share
struct GeoZoneRouteSearch::Cache {
    double tolerance = 0;
    QHash<std::pair<const GeoZone*, int>, GeoZone::Ring> outerRings;    ///< Simplified outward, by zone and volume
};

/// Uniform grid of items over an extent, items and queries beyond it fall in the cells along its sides
class GeoZoneRouteGrid
{
public:
    static constexpr int kCells = 64;   ///< Cells along each side

    void reset(const QRectF &extent);

    void insertRect(int item, const QRectF &bounds);
    /// Inserts the item in the cells within the distance of the segment
    void insertSegment(int item, const QPointF &a, const QPointF &b, double distance);

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
        for (const int item : _cells[(_row(point.y()) * kCells) + _column(point.x())]) {
            if (visitor(item)) {
                return true;
            }
        }
        return false;
    }

    /// Calls the visitor with each item of the cells the distance in cells away from the cell of the point, the ring
    /// of cells around it, which is the cell itself at distance 0
    /// @return false if the ring is entirely beyond the grid
    template<typename Visitor>
    bool visitRing(const QPointF &point, int distance, Visitor visitor) const
    {
        const int row = _row(point.y());
        const int column = _column(point.x());
        const int firstRow = row - distance;
        const int lastRow = row + distance;
        const int firstColumn = column - distance;
        const int lastColumn = column + distance;
        if ((firstRow < 0) && (firstColumn < 0) && (lastRow >= kCells) && (lastColumn >= kCells)) {
            return false;
        }
        const auto visitCell = [this, &visitor](int cellRow, int cellColumn) {
            for (const int item : _cells[(cellRow * kCells) + cellColumn]) {
                visitor(item);
            }
        };
        for (int cellRow = (std::max)(firstRow, 0); cellRow <= (std::min)(lastRow, kCells - 1); cellRow++) {
            if ((cellRow == firstRow) || (cellRow == lastRow)) {
                for (int cellColumn = (std::max)(firstColumn, 0); cellColumn <= (std::min)(lastColumn, kCells - 1); cellColumn++) {
                    visitCell(cellRow, cellColumn);
                }
                continue;
            }
            if (firstColumn >= 0) {
                visitCell(cellRow, firstColumn);
            }
            if (lastColumn < kCells) {
                visitCell(cellRow, lastColumn);
            }
        }
        return true;
    }

private:
    int _column(double x) const;
    int _row(double y) const;
    void _grow(int item);

    /// Calls the function with the cells within the distance of the segment, row by row, until it returns true
    template<typename Function>
    bool _visitCells(const QPointF &a, const QPointF &b, double distance, Function function) const
    {
        constexpr double infinity = std::numeric_limits<double>::infinity();
        const int lastRow = _row((std::max)(a.y(), b.y()) + distance);
        for (int row = _row((std::min)(a.y(), b.y()) - distance); row <= lastRow; row++) {
            // The part of the segment in the band of the row, widened by the distance. The outer rows extend to infinity.
            const double bandLow = (row == 0) ? -infinity : (_extent.top() + (row * _cellHeight) - distance);
            const double bandHigh = (row == (kCells - 1)) ? infinity : (_extent.top() + ((row + 1) * _cellHeight) + distance);
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
                if (function((row * kCells) + column)) {
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

/// Volumes of zones a detour keeps a margin from, its obstacles, in the local projection of the detour. Obstacles are
/// added with their bounds; their outlines are only projected, and their edges added to a grid, when a query comes
/// near them. Detailed outlines are simplified outward, except near the ends of the detour, which must be inside or
/// outside like for the zone. Segments from an end within the margin of an obstacle keep a smaller margin from it.
class GeoZoneRouteObstacles
{
public:
    static constexpr double kTolerance = 0.01;  ///< Meters segments may come closer to the zones than the margin, for rounding

    /// Ends of the detour a segment starts or ends at, see GeoZoneRouteObstacles
    enum Ends {
        NoEnds = 0x0,
        StartEnd = 0x1,
        EndEnd = 0x2,
    };

    /// @param start Local point of from
    /// @param end   Local point of to
    /// @param reach Bounds of the paths within the length limit, null without limit
    GeoZoneRouteObstacles(const GeoZone::Position &from, const GeoZone::Position &to, const GeoZoneGeometry::LocalProjection &projection,
                          const QPointF &start, const QPointF &end, double margin, const QRectF &reach, GeoZoneRouteSearch::Cache &cache,
                          GeoZoneRouteSearch::Statistics &statistics);

    /// Adds a volume of a zone
    /// @return Index of the obstacle, -1 for an empty volume
    int add(const GeoZone *zone, int volume);
    /// @return Index of the volume of the zone, -1 when it wasn't added
    int indexOf(const GeoZone *zone, int volume) const { return _indexes.value({ zone, volume }, -1); }
    /// Sets up the grids for the obstacles added so far, before the queries. Obstacles added later extend beyond them.
    void prepare();

    /// @return Extent of the grids, see prepare
    QRectF extent() const { return _extent; }
    int count() const { return static_cast<int>(_obstacles.count()); }
    const GeoZone *zone(int index) const { return _obstacles[index].zone; }
    /// @return Order in which the obstacle was added to the graph, -1 while it isn't
    int rank(int index) const { return _obstacles[index].rank; }
    void setRank(int index, int rank) { _obstacles[index].rank = rank; }
    /// @return Distinct zones of the obstacles
    QList<const GeoZone*> zones(const QSet<int> &indexes) const;

    /// Zones the start or the end is inside of, among the obstacles built
    const QList<const GeoZone*> &startInside() const { return _startInside; }
    const QList<const GeoZone*> &endInside() const { return _endInside; }

    /// Makes the rings of the obstacle and adds its edges to the grid, and sets the margins of segments from the ends
    void build(int index);
    /// @return Positions around the convex corners of the obstacle, on the arcs of the margin, such that the segments
    ///         between them keep the margin
    QList<QPointF> cornerNodes(int index);
    /// @return true if the point is inside the obstacle grown by the margin
    bool blocks(int index, const QPointF &point);
    /// @return Obstacles the point is inside of or within the margin of
    QList<int> pointBlockers(const QPointF &point);
    /// @return First obstacle of the graph, of the rank or higher, the segment comes closer to than the margin, -1 without
    int firstBlocker(const QPointF &a, const QPointF &b, int ends, int minRank);
    /// @return Obstacles outside the graph the segment comes closer to than the margin
    QList<int> hits(const QPointF &a, const QPointF &b, int ends);

private:
    struct Obstacle {
        const GeoZone *zone = nullptr;
        int volume = 0;
        bool circle = false;
        QPointF center;                     ///< Circles only
        double radius = 0;                  ///< Circles only
        QList<QList<QPointF>> rings;        ///< Polygons: the outer ring counterclockwise then the holes clockwise, the zone left of the edges
        QRectF bounds;                      ///< Grown by the margin, from the bounding box of the volume
        bool built = false;                 ///< The rings are made and the edges are in the grid, see build
        int rank = -1;
        double startMargin = 0;             ///< Of segments from the start, less than the margin when the start is within it
        double endMargin = 0;
    };

    /// Edge of a polygon, or circle, in the grid
    struct Item {
        int obstacle = -1;
        QPointF a;
        QPointF b;
    };

    void _insert(int index);
    void _buildAlong(const QPointF &a, const QPointF &b);
    void _buildAt(const QPointF &point);
    GeoZone::Ring _outerRing(const GeoZone *zone, int volume);
    double _endpointMargin(const Obstacle &obstacle, const QPointF &point, const GeoZone::Position &position, bool &inside) const;
    double _segmentMargin(const Obstacle &obstacle, int ends) const;
    static bool _blocks(const Obstacle &obstacle, const Item &item, const QPointF &a, const QPointF &b, double margin);

    const GeoZone::Position _from;
    const GeoZone::Position _to;
    const GeoZoneGeometry::LocalProjection _projection;
    const QPointF _start;
    const QPointF _end;
    const double _margin;
    const QRectF _reach;
    GeoZoneRouteSearch::Cache &_cache;
    GeoZoneRouteSearch::Statistics &_statistics;

    QList<Obstacle> _obstacles;
    QHash<std::pair<const GeoZone*, int>, int> _indexes;
    QList<Item> _items;
    GeoZoneRouteGrid _edgeGrid;     ///< Items within the margin of the cells
    GeoZoneRouteGrid _obstacleGrid; ///< Obstacles by their grown bounds
    QRectF _extent;
    bool _prepared = false;
    QList<const GeoZone*> _startInside;
    QList<const GeoZone*> _endInside;
};
