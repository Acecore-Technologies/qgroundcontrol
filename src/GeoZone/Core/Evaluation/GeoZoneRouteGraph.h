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
#include <QtCore/QSet>

#include "GeoZone.h"
#include "GeoZoneCancelToken.h"
#include "GeoZoneRouteObstacles.h"
#include "GeoZoneRouteSearch.h"

/// Visibility graph of a detour: the start, the end, and nodes around the convex corners of the obstacles added to it,
/// outside the others. Its shortest paths are found by A*, which tests the visibility of an edge against the obstacles
/// of the graph only when it would reach a node first, and caches it: obstacles added later are only tested for the
/// edges tested before.
///
/// In large graphs, edges far from a node are mostly blocked by those nearer, so that testing them all takes time
/// growing with the square of the nodes. Searches of the near edges only follow the edges from each node to the nodes
/// of the cells around it, at least a number of nodes of several obstacles, and to the end; their paths are then
/// straightened. Complete searches follow all edges. Both stop when the work of the graph reaches a limit.
class GeoZoneRouteGraph
{
public:
    static constexpr int kStart = 0;    ///< Node of the start
    static constexpr int kEnd = 1;      ///< Node of the end

    /// Edges a search follows, see GeoZoneRouteGraph
    enum class Search {
        Near,       ///< The near edges of large graphs, all edges of small ones
        Complete,   ///< All edges
    };

    /// @param maxLength Meters paths may be long at most, infinite without limit
    /// @param maxWork   Units of work after which searches stop, see work
    GeoZoneRouteGraph(GeoZoneRouteObstacles &obstacles, const QPointF &start, const QPointF &end, double maxLength, qint64 maxWork,
                      const GeoZoneCancelToken &cancel, GeoZoneRouteSearch::Statistics &statistics);

    /// Adds the obstacles to the graph, with nodes around their convex corners outside the other obstacles. The
    /// obstacles leaving out nodes are noted, since the graph must go around them when they close the gaps between
    /// those of the graph, see takeNodeBlockers.
    void include(const QList<int> &obstacles);
    /// Adds all volumes of the zones to the graph, also those which weren't obstacles yet
    /// @return Number of obstacles added
    int includeZones(const QList<const GeoZone*> &zones);

    /// @return Nodes of the shortest path from the start to the end, of those the search follows, empty without or when
    ///         the work reaches the limit, see GeoZoneRouteGraph
    /// @param limited  Only paths within the length limit
    /// @param blockers Obstacles blocking the edges tested
    QList<int> shortestPath(bool limited, QSet<int> *blockers, Search search = Search::Complete);
    /// @return true if Search::Near leaves out edges, so that it may miss paths a complete search finds
    bool nearSearchIncomplete() const;

    /// @return Obstacles outside the graph the segment between the nodes comes closer to than the margin
    QList<int> hits(int from, int to);
    /// @return Obstacles outside the graph which left out nodes since the last call
    QList<int> takeNodeBlockers();
    /// @return true if nodes were left out since the graph had too many, or a search stopped when the work of the
    ///         graph reached its limit
    bool limitReached() const { return _nodeLimitReached || _workLimitReached; }
    /// @return Units of work of the searches so far: nodes and edges considered, and visibility tests weighted by their cost
    qint64 work() const { return _work; }

    QPointF point(int node) const { return _nodes[node].point; }
    double lengthOf(const QList<int> &path) const;

private:
    struct Node {
        QPointF point;
        double toEnd = 0;       ///< Distance to the end, the heuristic of A*
        bool inReach = true;    ///< Paths through the node can be within the length limit
        bool valid = true;      ///< Keeps the margin from all obstacles
        int obstacle = -1;      ///< Around which the node is, -1 for the start and the end
    };

    /// Visibility between two nodes, against the obstacles of the graph
    struct Visibility {
        int checkedRank = 0;    ///< Obstacles of lower rank were checked
        int blocker = -1;       ///< Obstacle blocking the segment, -1 when none of those checked does
    };

    static int _ends(int from, int to);
    void _addWork(qint64 work);
    bool _visible(int from, int to, int &blocker);
    QList<int> _astar(bool limited, bool near, QSet<int> *blockers);
    QList<int> _straightened(const QList<int> &path);

    GeoZoneRouteObstacles &_obstacles;
    const QPointF _start;
    const QPointF _end;
    const double _maxLength;
    const qint64 _maxWork;
    const GeoZoneCancelToken _cancel;
    GeoZoneRouteSearch::Statistics &_statistics;

    QList<int> _included;           ///< Obstacles of the graph, by rank
    QList<Node> _nodes;             ///< The start, the end, then around the obstacles of the graph
    GeoZoneRouteGrid _nodeGrid;
    bool _nodeLimitReached = false;
    qint64 _work = 0;
    bool _workLimitReached = false;
    QSet<int> _nodeBlockers;        ///< Obstacles outside the graph which left out nodes
    QHash<quint64, Visibility> _visibility;
};
