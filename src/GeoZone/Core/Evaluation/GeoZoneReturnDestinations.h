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

#include "GeoZoneMissionItem.h"
#include "GeoZoneReturnModel.h"

/// Where the vehicle can return to along a planned mission, see GeoZoneReturnModel::Destinations, from the items of
/// the mission the way the autopilots read them
class GeoZoneReturnDestinations
{
public:
    /// @return Where the vehicle can return to: home, the rally points and the breach return point of the plan as given,
    ///         and those the mission has: the first waypoint after the takeoff, which PX4 returns to along the mission
    ///         in reverse before flying home, the landing sequences from their DO_LAND_START or landing pattern, and the
    ///         positions after an ArduPlane DO_RETURN_PATH_START up to its landing. The coordinates are without altitude,
    ///         the altitudes AMSL those given and those of the items. Rally points without coordinate are left out.
    /// @param items Of the mission after the planned home, in mission order, see GeoZoneMissionReader::items
    /// @param home Planned home with its ground altitude, invalid without
    /// @param rallyPoints With their altitudes AMSL, which are above home in the plan
    /// @param breachReturnPoint Of the fence, with its altitude AMSL, invalid without
    static GeoZoneReturnModel::Destinations fromMission(const QList<GeoZoneMissionItem> &items, const GeoZone::Position &home = GeoZone::Position(),
                                                        const QList<GeoZone::Position> &rallyPoints = {},
                                                        const GeoZone::Position &breachReturnPoint = GeoZone::Position());
};
