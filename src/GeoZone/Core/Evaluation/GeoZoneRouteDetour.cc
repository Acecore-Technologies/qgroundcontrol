/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRouteDetour.h"
#include "GeoZoneRouteGraph.h"
#include "GeoZoneRouteVector.h"

#include <QtCore/QSet>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMaxRounds = 64;  ///< Searches of the graph of a detour

/// @return Bounds of the ellipse with the ends as foci, which contains all paths within the length limit, grown by the
///         margin, null without limit
QRectF _reachOf(const QPointF &start, const QPointF &end, double directLength, double maxLength, double margin)
{
    if (std::isinf(maxLength)) {
        return QRectF();
    }
    const QPointF center = (start + end) / 2;
    const double major = maxLength / 2;
    const double focal = directLength / 2;
    const double minor = std::sqrt((std::max)((major * major) - (focal * focal), 0.0));
    const double angle = std::atan2(end.y() - start.y(), end.x() - start.x());
    const double halfWidth = std::hypot(major * std::cos(angle), minor * std::sin(angle)) + margin;
    const double halfHeight = std::hypot(major * std::sin(angle), minor * std::cos(angle)) + margin;
    return QRectF(center.x() - halfWidth, center.y() - halfHeight, 2 * halfWidth, 2 * halfHeight);
}

} // namespace

GeoZoneRouteDetour::GeoZoneRouteDetour(const GeoZone::Position &from, const GeoZone::Position &to, double margin, double maxLength, qint64 maxWork,
                                       const GeoZoneCancelToken &cancel, GeoZoneRouteSearch::Cache &cache, GeoZoneRouteSearch::Statistics &statistics)
    : _from(from)
    , _to(to)
    , _margin(margin)
    , _maxLength(maxLength)
    , _maxWork(maxWork)
    , _cancel(cancel)
    , _statistics(statistics)
    , _projection(from.coordinate.atDistanceAndAzimuth(from.coordinate.distanceTo(to.coordinate) / 2, from.coordinate.azimuthTo(to.coordinate)))
    , _start(_projection.toLocal(from.coordinate))
    , _end(_projection.toLocal(to.coordinate, _start))
    , _directLength(GeoZoneVector::length(_end - _start))
    , _reach(_reachOf(_start, _end, _directLength, maxLength, margin))
    , _obstacles(from, to, _projection, _start, _end, margin, _reach, cache, statistics)
{
}

GeoZoneBox GeoZoneRouteDetour::reachBox() const
{
    constexpr double maxReach = 5000000;
    if (_reach.isNull() || (_reach.width() > maxReach) || (_reach.height() > maxReach)) {
        return GeoZoneBox();
    }
    const QGeoCoordinate southWest = _projection.toGeo(_reach.topLeft());
    const QGeoCoordinate northEast = _projection.toGeo(_reach.bottomRight());
    return GeoZoneBox(std::clamp(southWest.latitude(), -90.0, 90.0), southWest.longitude(), std::clamp(northEast.latitude(), -90.0, 90.0), northEast.longitude());
}

GeoZoneRouteDetour::Outcome GeoZoneRouteDetour::run(const Verify &verify)
{
    _obstacles.prepare();
    if (!_obstacles.startInside().isEmpty()) {
        _blockingZones = _obstacles.startInside();
        return Outcome::StartInside;
    }
    if (!_obstacles.endInside().isEmpty()) {
        _blockingZones = _obstacles.endInside();
        return Outcome::EndInside;
    }
    GeoZoneRouteGraph graph(_obstacles, _start, _end, _maxLength, _maxWork, _cancel, _statistics);

    // The obstacles the straight line runs into
    graph.include(graph.hits(GeoZoneRouteGraph::kStart, GeoZoneRouteGraph::kEnd));
    for (int round = 0; round < kMaxRounds; round++) {
        if (_cancel.isCancelled()) {
            return Outcome::Cancelled;
        }
        _statistics.rounds++;
        QSet<int> blockers;
        QList<int> path = graph.shortestPath(true, &blockers, GeoZoneRouteGraph::Search::Near);
        if (_cancel.isCancelled()) {
            return Outcome::Cancelled;
        }
        if (path.isEmpty()) {
            if (graph.limitReached()) {
                return Outcome::SearchLimit;
            }
            // The obstacles which left out nodes around those of the graph may leave a way around them
            const QList<int> pending = graph.takeNodeBlockers();
            if (!pending.isEmpty()) {
                graph.include(pending);
                continue;
            }
            if (graph.nearSearchIncomplete()) {
                if (_preliminary) {
                    return Outcome::TooLong;
                }
                blockers.clear();
                path = graph.shortestPath(true, &blockers);
                if (_cancel.isCancelled()) {
                    return Outcome::Cancelled;
                }
            }
        }
        if (path.isEmpty()) {
            if (graph.limitReached()) {
                return Outcome::SearchLimit;
            }
            if (!std::isinf(_maxLength)) {
                // Whether a longer detour goes around the obstacles found so far
                QSet<int> allBlockers;
                QList<int> longer = graph.shortestPath(false, &allBlockers, GeoZoneRouteGraph::Search::Near);
                if (longer.isEmpty() && !graph.limitReached() && graph.nearSearchIncomplete() && !_cancel.isCancelled()) {
                    allBlockers.clear();
                    longer = graph.shortestPath(false, &allBlockers);
                }
                if (_cancel.isCancelled()) {
                    return Outcome::Cancelled;
                }
                if (!longer.isEmpty()) {
                    _pathLength = graph.lengthOf(longer);
                    _blockingZones = _obstacles.zones(blockers);
                    return Outcome::TooLong;
                }
                if (graph.limitReached()) {
                    return Outcome::SearchLimit;
                }
                blockers = allBlockers;
            }
            _blockingZones = _obstacles.zones(blockers);
            return Outcome::Enclosed;
        }

        QList<int> hits;
        for (qsizetype i = 1; i < path.count(); i++) {
            for (const int obstacle : graph.hits(path[i - 1], path[i])) {
                if (!hits.contains(obstacle)) {
                    hits.append(obstacle);
                }
            }
        }
        if (!hits.isEmpty()) {
            graph.include(hits);
            continue;
        }

        const QList<QGeoCoordinate> coordinates = _coordinates(graph, path);
        const QList<const GeoZone*> entered = verify ? verify(coordinates) : QList<const GeoZone*>();
        if (entered.isEmpty()) {
            _waypoints = coordinates.mid(1, coordinates.count() - 2);
            _pathLength = graph.lengthOf(path);
            return Outcome::Found;
        }
        // Zones whose volumes were left out as above or below the leg, or entered despite the margin
        if (graph.includeZones(entered) == 0) {
            _blockingZones = entered;
            return Outcome::EntersZone;
        }
    }
    return Outcome::SearchLimit;
}

QList<QGeoCoordinate> GeoZoneRouteDetour::_coordinates(const GeoZoneRouteGraph &graph, const QList<int> &path) const
{
    QList<QGeoCoordinate> coordinates;
    for (const int node : path) {
        coordinates.append((node == GeoZoneRouteGraph::kStart) ? _from.coordinate
                                                               : ((node == GeoZoneRouteGraph::kEnd) ? _to.coordinate : _projection.toGeo(graph.point(node))));
    }
    return coordinates;
}
