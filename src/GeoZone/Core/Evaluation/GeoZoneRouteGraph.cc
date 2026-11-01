/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRouteGraph.h"
#include "GeoZoneRouteObstacles.h"
#include "GeoZoneVector.h"

#include <algorithm>
#include <array>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

namespace {

constexpr int kMaxNodes = 20000;
constexpr int kExhaustiveNodes = 1000;          ///< Graphs of up to this many nodes are searched along all edges
constexpr int kNearNodes = 32;                  ///< The near edges of a node lead to at least this many nodes,
constexpr int kNearObstacles = 8;               ///< around at least this many obstacles, or all those of the graph
constexpr qint64 kVisibilityTestWork = 16;      ///< Units of work of a visibility test, the others are edges and nodes considered

using GeoZoneVector::length;

} // namespace

GeoZoneRouteGraph::GeoZoneRouteGraph(GeoZoneRouteObstacles &obstacles, const QPointF &start, const QPointF &end, double maxLength, qint64 maxWork,
                                     const GeoZoneCancelToken &cancel, GeoZoneRouteSearch::Statistics &statistics)
    : _obstacles(obstacles)
    , _start(start)
    , _end(end)
    , _maxLength(maxLength)
    , _maxWork(maxWork)
    , _cancel(cancel)
    , _statistics(statistics)
{
    _nodes = { Node{ start, length(end - start), true, true, -1 }, Node{ end, 0, true, true, -1 } };
    _statistics.nodes += 2;
    _nodeGrid.reset(obstacles.extent());
}

void GeoZoneRouteGraph::_addWork(qint64 work)
{
    _work += work;
    _statistics.work += work;
}

bool GeoZoneRouteGraph::nearSearchIncomplete() const
{
    return _nodes.count() > kExhaustiveNodes;
}

int GeoZoneRouteGraph::_ends(int from, int to)
{
    int ends = GeoZoneRouteObstacles::NoEnds;
    if ((from == kStart) || (to == kStart)) {
        ends |= GeoZoneRouteObstacles::StartEnd;
    }
    if ((from == kEnd) || (to == kEnd)) {
        ends |= GeoZoneRouteObstacles::EndEnd;
    }
    return ends;
}

QList<int> GeoZoneRouteGraph::hits(int from, int to)
{
    return _obstacles.hits(_nodes[from].point, _nodes[to].point, _ends(from, to));
}

QList<int> GeoZoneRouteGraph::takeNodeBlockers()
{
    QList<int> pending;
    for (const int obstacle : std::as_const(_nodeBlockers)) {
        if (_obstacles.rank(obstacle) < 0) {
            pending.append(obstacle);
        }
    }
    _nodeBlockers.clear();
    return pending;
}

/// @return true if the segment between the nodes keeps the margin from the obstacles of the graph, from the cache
///         when known
bool GeoZoneRouteGraph::_visible(int from, int to, int &blocker)
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
    _addWork(kVisibilityTestWork);
    blocker = _obstacles.firstBlocker(_nodes[from].point, _nodes[to].point, _ends(from, to), checkedRank);
    _visibility.insert(key, Visibility{ rank, blocker });
    return blocker < 0;
}

void GeoZoneRouteGraph::include(const QList<int> &indexes)
{
    for (const int index : indexes) {
        _obstacles.build(index);
        if (_obstacles.rank(index) >= 0) {
            continue;
        }
        _obstacles.setRank(index, static_cast<int>(_included.count()));
        _included.append(index);
        _statistics.includedObstacles++;
        for (const QPointF &point : _obstacles.cornerNodes(index)) {
            if (_nodes.count() >= kMaxNodes) {
                _nodeLimitReached = true;
                return;
            }
            const QList<int> blockers = _obstacles.pointBlockers(point);
            if (blockers.isEmpty()) {
                const double toEnd = length(_end - point);
                _nodeGrid.insertRect(static_cast<int>(_nodes.count()), QRectF(point, point));
                _nodes.append(Node{ point, toEnd, (length(point - _start) + toEnd) <= _maxLength, true, index });
                _statistics.nodes++;
                continue;
            }
            for (const int blocker : blockers) {
                if (_obstacles.rank(blocker) < 0) {
                    _nodeBlockers.insert(blocker);
                }
            }
        }
    }
}

int GeoZoneRouteGraph::includeZones(const QList<const GeoZone*> &zones)
{
    int added = 0;
    for (const GeoZone *zone : zones) {
        for (int volume = 0; volume < zone->data().volumes.count(); volume++) {
            int index = _obstacles.indexOf(zone, volume);
            if (index < 0) {
                index = _obstacles.add(zone, volume);
                if (index < 0) {
                    continue;
                }
                // Nodes are outside the obstacles, also this one
                for (qsizetype node = 2; node < _nodes.count(); node++) {
                    if (_nodes[node].valid && _obstacles.blocks(index, _nodes[node].point)) {
                        _nodes[node].valid = false;
                    }
                }
            }
            if (_obstacles.rank(index) < 0) {
                include({ index });
                added++;
            }
        }
    }
    return added;
}

QList<int> GeoZoneRouteGraph::shortestPath(bool limited, QSet<int> *blockers, Search search)
{
    const bool near = (search == Search::Near) && nearSearchIncomplete();
    const QList<int> path = _astar(limited, near, blockers);
    return (near && !path.isEmpty()) ? _straightened(path) : path;
}

/// @param near Only along the near edges, see GeoZoneRouteGraph
QList<int> GeoZoneRouteGraph::_astar(bool limited, bool near, QSet<int> *blockers)
{
    // Edges are tested lazily, like by Lazy Weighted A*: candidate edges are queued by the estimated length of the paths
    // through them, and only tested when they would reach a node first.
    const QList<Node> &nodes = _nodes;
    const qsizetype count = nodes.count();
    const int nearObstacles = (std::min)(kNearObstacles, static_cast<int>(_included.count()));
    std::vector<int> previous(count, -1);
    std::vector<double> distances(count, 0);
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
        // Adds the edge to the node unless it is closed
        // @return false if paths can't go through the node
        const auto consider = [&](int next) {
            _addWork(1);
            const Node &other = nodes[next];
            if (!other.valid || (limited && !other.inReach)) {
                return false;
            }
            if (!closed[next]) {
                const double estimate = distance + length(other.point - point) + other.toEnd;
                if (!limited || (estimate <= (_maxLength + GeoZoneRouteObstacles::kTolerance))) {
                    expansion.edges.emplace_back(estimate, next);
                }
            }
            return true;
        };
        if (!near) {
            for (int next = 1; next < count; next++) {
                (void) consider(next);
            }
        } else {
            // The nodes of the rings of cells around the node, closed or not, until there are enough
            (void) consider(kEnd);
            int nearNodes = 0;
            std::array<int, kNearObstacles> obstacles{};
            int obstacleCount = 0;
            for (int ring = 0; (nearNodes < kNearNodes) || (obstacleCount < nearObstacles); ring++) {
                const bool inGrid = _nodeGrid.visitRing(point, ring, [&](int next) {
                    if ((next == node) || !consider(next)) {
                        return;
                    }
                    nearNodes++;
                    const int obstacle = nodes[next].obstacle;
                    const auto obstaclesEnd = obstacles.cbegin() + obstacleCount;
                    if ((obstacleCount < kNearObstacles) && (std::find(obstacles.cbegin(), obstaclesEnd, obstacle) == obstaclesEnd)) {
                        obstacles[obstacleCount++] = obstacle;
                    }
                });
                if (!inGrid) {
                    break;
                }
            }
        }
        if (expansion.edges.empty()) {
            return;
        }
        std::sort(expansion.edges.begin(), expansion.edges.end());
        open.push({ expansion.edges.front().first, expansions.size() });
        expansions.push_back(std::move(expansion));
    };
    closed[kStart] = 1;
    expand(kStart, 0);
    while (!open.empty() && !closed[kEnd]) {
        if (_work > _maxWork) {
            _workLimitReached = true;
            return {};
        }
        _addWork(1);
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
        distances[to] = fromDistance + length(nodes[to].point - nodes[from].point);
        // Like Theta*, near edges are combined when the node before sees the next, so that the paths and the lengths
        // their limit is checked with aren't those of the zigzags between near nodes
        const int before = previous[from];
        int shortcutBlocker = -1;
        if (near && (before >= 0) && _visible(before, to, shortcutBlocker)) {
            previous[to] = before;
            distances[to] = distances[before] + length(nodes[to].point - nodes[before].point);
        }
        if (to == kEnd) {
            break;
        }
        if (_cancel.isCancelled()) {
            return {};
        }
        expand(to, distances[to]);
    }
    if (!closed[kEnd]) {
        return {};
    }
    QList<int> path;
    for (int node = kEnd; node >= 0; node = previous[node]) {
        path.prepend(node);
    }
    return path;
}

/// @return The path with its nodes skipped where the nodes before and after them see each other
QList<int> GeoZoneRouteGraph::_straightened(const QList<int> &path)
{
    QList<int> straightened = { path.constFirst() };
    qsizetype from = 0;
    while (from < (path.count() - 1)) {
        qsizetype to = from + 1;
        for (qsizetype farther = path.count() - 1; farther > to; farther--) {
            int blocker = -1;
            if (_visible(path[from], path[farther], blocker)) {
                to = farther;
                break;
            }
        }
        straightened.append(path[to]);
        from = to;
    }
    return straightened;
}

double GeoZoneRouteGraph::lengthOf(const QList<int> &path) const
{
    double pathLength = 0;
    for (qsizetype i = 1; i < path.count(); i++) {
        pathLength += length(_nodes[path[i]].point - _nodes[path[i - 1]].point);
    }
    return pathLength;
}
