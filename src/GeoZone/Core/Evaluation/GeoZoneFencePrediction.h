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
#include <QtCore/QtNumeric>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>

#include <optional>

#include "GeoZoneAlertTracker.h"

/// Where a vehicle flying straight on at its velocity breaches the fence its autopilot enforces, so that the pilot is
/// warned before the autopilot's fence action (EN 4709-003 6.2.2): its inclusion and exclusion polygons and circles, the
/// circle around home and the altitude limits above home. Horizontal geometry is planar around the vehicle.
namespace GeoZoneFencePrediction {

using Velocity = GeoZoneAlertTracker::Velocity;

/// The fence of a vehicle
struct Fence {
    struct Polygon {
        QList<QGeoCoordinate> vertices;
        bool inclusion = false;
    };
    struct Circle {
        QGeoCircle circle;
        bool inclusion = false;
    };

    QList<Polygon> polygons;
    QList<Circle> circles;
    /// Inside any of the inclusion polygons and circles is inside the fence, like PX4 and ArduPilot with FENCE_OPTIONS
    /// bit 1. Otherwise inside all of them, like ArduPilot by default.
    bool inclusionUnion = false;
    QGeoCoordinate home;                ///< Center of maxDistance
    double maxDistance = qQNaN();       ///< Meters horizontally from home, NaN without
    double maxAltitude = qQNaN();       ///< Meters above home, NaN without
    double minAltitude = qQNaN();       ///< Meters above home, NaN without
    double horizontalMargin = 0;        ///< Meters inside the boundaries which count as breaching them, like ArduPilot's FENCE_MARGIN
    double verticalMargin = 0;

    /// @return true without boundaries
    bool isEmpty() const;
};

enum class Boundary {
    Polygon,
    Circle,
    MaxDistance,    ///< The circle around home
    MaxAltitude,
    MinAltitude,
};

/// Where the vehicle breaches the fence
struct Breach {
    Boundary boundary = Boundary::Polygon;
    bool inclusion = false;         ///< Leaving an inclusion polygon or circle, otherwise entering an exclusion one
    int index = -1;                 ///< Of the polygon or circle
    double time = 0;                ///< Seconds from the position
    double distance = 0;            ///< Meters along the velocity, in three dimensions
    QGeoCoordinate coordinate;      ///< Where, horizontally
    double altitude = qQNaN();      ///< Meters above home there, NaN when unknown
};

/// @return The boundary the position is beyond, nothing while it is inside the fence: above or below the altitude
///         limits, beyond the maximum distance from home, inside an exclusion polygon or circle or outside the
///         inclusion ones. Margins don't count.
/// @param altitude Meters above home, NaN when unknown, which is within the altitude limits
std::optional<Boundary> breachedBoundary(const Fence &fence, const QGeoCoordinate &coordinate, double altitude);

/// @return true if the position is outside the fence, see breachedBoundary
bool breached(const Fence &fence, const QGeoCoordinate &coordinate, double altitude);

/// @return The first breach of the fence, margins included, along the straight path at the velocity within the horizon
///         in seconds. Nothing when the position breaches the fence already, without a velocity or when the path stays
///         inside. Boundaries the position is within the margin of are breached at the boundary itself.
/// @param altitude Meters above home, NaN when unknown: the altitude limits are then not predicted
std::optional<Breach> predict(const Fence &fence, const QGeoCoordinate &coordinate, double altitude, const Velocity &velocity, double horizon);

} // namespace GeoZoneFencePrediction
