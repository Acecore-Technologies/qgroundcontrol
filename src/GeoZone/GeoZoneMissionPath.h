/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"
#include "GeoZonePathEvaluator.h"

class MissionController;
class Vehicle;

/// Flight path of a planned mission as the geozone checks see it: the segments flown with their altitudes and
/// terrain, the areas of patterns, and the legs between mission items. Besides the flight path the mission
/// controller draws, it covers loiter circles, the loiter of landing patterns, the distance VTOL transitions
/// overshoot and the return to launch at the vehicle's return altitude. Built on the GUI thread from the
/// MissionController, then only read, so that zone checks can run on other threads.
class GeoZoneMissionPath
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneMissionPath)

public:
    /// Straight flight path segment, in mission order
    struct Segment {
        QGeoCoordinate from;
        QGeoCoordinate to;
        double fromAltitudeAMSL = qQNaN();
        double toAltitudeAMSL = qQNaN();
        QList<double> terrainAMSL;  ///< Ground elevation every terrainSpacing meters from the start, the last sample at the end
        double terrainSpacing = 0;
        double distance = 0;        ///< Meters along the mission, which can differ slightly from the great circle
        int itemIndex = -1;         ///< Visual item of the mission the segment belongs to or starts from

        /// Segments without coordinates, like patterns without a flight path yet, only advance the distance
        bool isValid() const { return from.isValid() && to.isValid(); }

        /// @return Ground elevation at the distance along the great circle from the start, interpolated between the
        ///         terrain samples, NaN without them
        double terrainAt(double along) const;
        /// @return Position at the distance along the great circle from the start, with the altitude and terrain
        ///         interpolated
        GeoZone::Position positionAt(double along) const;
    };

    /// Connected positions for zone checks, see pathsFromSegments
    struct Path {
        QList<GeoZone::Position> positions;
        int itemIndex = -1;         ///< Item of the first segment
        QString label;              ///< What the vehicle does on paths besides the drawn flight path, like "the loiter at item 3"
        QList<double> distances;    ///< Meters along the mission at each position, like Segment::distance. Paths besides the
                                    ///< drawn flight path are at the distance where the vehicle flies them.
    };

    /// Straight leg between two mission items with a coordinate, which a route can go around
    struct Leg {
        int fromItemIndex = -1;
        int toItemIndex = -1;
        int toSequenceNumber = -1;
        Segment segment;            ///< With altitudes and terrain when the flight path segment is known
    };

    GeoZoneMissionPath() = default;

    /// @return The current path of the mission, empty without a controller
    static GeoZoneMissionPath fromMission(MissionController *missionController);
    /// @return Path made of the segments, for tests and paths which aren't missions
    static GeoZoneMissionPath fromSegments(const QList<Segment> &segments, const QList<GeoZone::Area> &areas = {});

    const QList<Segment> &segments() const { return _segments; }
    /// Survey, corridor and structure scan areas, since camera footprints and turns extend past the flight lines
    const QList<GeoZone::Area> &areas() const { return _areas; }
    const QList<Leg> &legs() const { return _legs; }
    /// The segments as connected paths, see pathsFromSegments
    const QList<Path> &paths() const { return _paths; }
    /// The positions of paths
    QList<QList<GeoZone::Position>> positionLists() const;
    /// @return Meters along the whole mission, the sum of the segment distances
    double totalDistance() const;
    /// @return Meters along the mission where the vehicle reaches the item, NaN for unknown items
    double itemDistance(int itemIndex) const { return _itemDistances.value(itemIndex, qQNaN()); }

    /// @return The paths for GeoZonePathEvaluator, with times estimated from start to end in proportion to the distance
    ///         along the mission, all at start without end and unknown without start
    QList<GeoZonePathEvaluator::Path> evaluatorPaths(const QDateTime &start = QDateTime(), const QDateTime &end = QDateTime()) const;
    /// @return Estimated time at the distance along a mission flown from start to end, see evaluatorPaths
    static QDateTime timeAt(double distance, double totalDistance, const QDateTime &start, const QDateTime &end);

    static constexpr double maxStepDistance = 1000;     ///< Meters between positions along long legs without terrain
    static constexpr int maxStepsPerSegment = 10000;
    static constexpr int circleVertexCount = 36;        ///< Of loiter circles

    /// @return Return altitude above home in meters from the vehicle's parameters, 0 to return at the current
    ///         altitude, NaN when unknown like for the offline vehicle
    static double returnAltitude(Vehicle *vehicle);

    /// @return Closed path around the center at the radius, the altitude and terrain
    static QList<GeoZone::Position> circle(const QGeoCoordinate &center, double radius, double altitudeAMSL, double terrainAMSL);

    /// Splits segments into connected paths with a position at each terrain sample, or along the great circle every
    /// maxStepDistance without terrain samples. Altitudes are interpolated. Segments without coordinates end a path.
    static QList<Path> pathsFromSegments(const QList<Segment> &segments);

private:
    /// Adds the paths the drawn flight path leaves out
    void _appendCoverage(MissionController *missionController, const QList<int> &coordinateItems);

    QList<Segment> _segments;
    QList<GeoZone::Area> _areas;
    QList<Leg> _legs;
    QList<Path> _paths;
    QHash<int, double> _itemDistances;  ///< Of the visual items, by index
};
