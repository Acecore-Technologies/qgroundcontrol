/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"

/// Item of a planned mission as the geozone checks see it, read from the mission by the application, so that the
/// computations on the items, like the return destinations (GeoZoneReturnDestinations) and the DO_JUMP loops
/// (GeoZoneMissionJumps), don't need the mission controller
struct GeoZoneMissionItem
{
    enum class Kind {
        Other,              ///< Simple items like waypoints, loiters, landings and commands
        Takeoff,            ///< Takeoff or VTOL takeoff
        ReturnToLaunch,
        Jump,               ///< DO_JUMP, see jumpSequence and jumpRepeat
        LandStart,          ///< DO_LAND_START, see marker
        ReturnPathStart,    ///< DO_RETURN_PATH_START
        LandingPattern,     ///< Fixed wing or VTOL landing pattern, from its final approach to its touchdown
        Pattern,            ///< Other complex items, like surveys
    };

    Kind kind = Kind::Other;
    int sequenceNumber = -1;
    int lastSequenceNumber = -1;    ///< Of the last mission item of complex items, the sequence number of simple ones
    bool flies = false;             ///< The vehicle flies to its coordinate, unlike commands and standalone coordinates
    bool land = false;              ///< The vehicle lands at its exit
    GeoZone::Position entry;        ///< Coordinate where the vehicle enters the item, with its altitude AMSL
    GeoZone::Position exit;         ///< Where it leaves the item, like the touchdown of landings
    QGeoCoordinate marker;          ///< Of a DO_LAND_START, invalid without
    int jumpSequence = -1;          ///< Sequence number the DO_JUMP jumps to
    int jumpRepeat = 0;             ///< Times the DO_JUMP jumps, negative without end
};
