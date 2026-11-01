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
#include "GeoZoneRouteSearch.h"

class GeoZoneRouteObstacles;

/// Visibility graph of a detour: the start, the end, and nodes around the convex corners of the obstacles added to it,
/// outside the others. Its shortest paths are found by A*, which tests the visibility of an edge against the obstacles
/// of the graph only when it would reach a node first, and caches it: obstacles added later are only tested for the
/// edges tested before.
class GeoZoneRouteGraph
{
public:
    static constexpr int kStart = 0;    ///< Node of the start
    static constexpr int kEnd = 1;      ///< Node of the end

    /// @param maxLength Meters paths may be long at most, infinite without limit
    GeoZoneRouteGraph(GeoZoneRouteObstacles &obstacles, const QPointF &start, const QPointF &end, double maxLength, const GeoZoneCancelToken &cancel,
                      GeoZoneRouteSearch::Statistics &statistics);

    /// Adds the obstacles to the graph, with nodes around their convex corners outside the other obstacles. The
    /// obstacles leaving out nodes are noted, since the graph must go around them when they close the gaps between
    /// those of the graph, see takeNodeBlockers.
    void include(const QList<int> &obstacles);
    /// Adds all volumes of the zones to the graph, also those which weren't obstacles yet
    /// @return Number of obstacles added
    int includeZones(const QList<const GeoZone*> &zones);

    /// @return Nodes of the shortest path from the start to the end, empty without, see GeoZoneRouteGraph
    /// @param limited  Only paths within the length limit
    /// @param blockers Obstacles blocking the edges tested
    QList<int> shortestPath(bool limited, QSet<int> *blockers);

    /// @return Obstacles outside the graph the segment between the nodes comes closer to than the margin
    QList<int> hits(int from, int to);
    /// @return Obstacles outside the graph which left out nodes since the last call
    QList<int> takeNodeBlockers();
    /// @return true if nodes were left out since the graph had too many
    bool nodeLimitReached() const { return _nodeLimitReached; }

    QPointF point(int node) const { return _nodes[node].point; }
    double lengthOf(const QList<int> &path) const;

private:
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

    static int _ends(int from, int to);
    bool _visible(int from, int to, int &blocker);

    GeoZoneRouteObstacles &_obstacles;
    const QPointF _start;
    const QPointF _end;
    const double _maxLength;
    const GeoZoneCancelToken _cancel;
    GeoZoneRouteSearch::Statistics &_statistics;

    QList<int> _included;           ///< Obstacles of the graph, by rank
    QList<Node> _nodes;             ///< The start, the end, then around the obstacles of the graph
    bool _nodeLimitReached = false;
    QSet<int> _nodeBlockers;        ///< Obstacles outside the graph which left out nodes
    QHash<quint64, Visibility> _visibility;
};
