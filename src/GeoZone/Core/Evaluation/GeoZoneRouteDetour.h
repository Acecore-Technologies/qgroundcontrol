/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtPositioning/QGeoCoordinate>

#include <functional>

#include "GeoZone.h"
#include "GeoZoneBox.h"
#include "GeoZoneCancelToken.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneRouteObstacles.h"
#include "GeoZoneRouteSearch.h"

class GeoZoneRouteGraph;

/// Shortest path from a start to an end keeping a margin from obstacles, the volumes of zones. Only the obstacles the
/// paths run into are added to the graph: each round searches the graph, then adds the obstacles the path runs into
/// until it runs into none.
class GeoZoneRouteDetour
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

    /// @param maxLength Meters the detour may be long at most, infinite without limit
    GeoZoneRouteDetour(const GeoZone::Position &from, const GeoZone::Position &to, double margin, double maxLength, const GeoZoneCancelToken &cancel,
                       GeoZoneRouteSearch::Cache &cache, GeoZoneRouteSearch::Statistics &statistics);

    double directLength() const { return _directLength; }
    /// @return Bounds of the paths within the length limit, invalid without limit or when they span the earth
    GeoZoneBox reachBox() const;

    /// Adds a volume of a zone as an obstacle, before run
    void addObstacle(const GeoZone *zone, int volume) { (void) _obstacles.add(zone, volume); }

    /// Searches the detour, verifying the paths keeping the margin with the function when set
    Outcome run(const Verify &verify);

    const QList<QGeoCoordinate> &waypoints() const { return _waypoints; }
    /// Meters from the start to the end, of the shortest detour found when TooLong
    double length() const { return _pathLength; }
    /// Zones the start or the end is inside, which surround them, or which the path enters
    const QList<const GeoZone*> &blockingZones() const { return _blockingZones; }

private:
    QList<QGeoCoordinate> _coordinates(const GeoZoneRouteGraph &graph, const QList<int> &path) const;

    const GeoZone::Position _from;
    const GeoZone::Position _to;
    const double _margin;
    const double _maxLength;
    const GeoZoneCancelToken _cancel;
    GeoZoneRouteSearch::Statistics &_statistics;
    const GeoZoneGeometry::LocalProjection _projection;
    const QPointF _start;
    const QPointF _end;
    const double _directLength;
    const QRectF _reach;            ///< Of the paths within the length limit, null without limit
    GeoZoneRouteObstacles _obstacles;

    QList<QGeoCoordinate> _waypoints;
    double _pathLength = 0;
    QList<const GeoZone*> _blockingZones;
};
