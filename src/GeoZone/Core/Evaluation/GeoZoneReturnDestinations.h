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
    /// @return The destinations the mission has: the first waypoint PX4 returns to along the mission in reverse, the
    ///         landing sequences from their DO_LAND_START or landing pattern, and the positions after an ArduPlane
    ///         DO_RETURN_PATH_START up to its landing. Home, the rally points and the breach return point are the plan's,
    ///         left invalid and empty. The coordinates are without altitude, the altitudes those of the items.
    /// @param items Of the mission after the planned home, in mission order
    static GeoZoneReturnModel::Destinations fromMission(const QList<GeoZoneMissionItem> &items);
};
