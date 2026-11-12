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
#include "GeoZoneMissionPath.h"
#include "GeoZoneReturnModel.h"

class MissionController;
class Vehicle;

/// Reads the planned mission of a MissionController for the geozone checks: its items, the way the controller draws its
/// flight path, which GeoZoneMissionPathBuilder makes the GeoZoneMissionPath of, and the return parameters of its
/// vehicle. On the GUI thread.
class GeoZoneMissionReader
{
public:
    /// @return The current path of the mission, empty without a controller, see GeoZoneMissionPathBuilder. Returns are
    ///         flown with the parameters of the controller's vehicle, see returnParameters. The contingency paths, which
    ///         take longer to build, are only built withContingencies.
    static GeoZoneMissionPath path(MissionController *missionController, bool withContingencies = false);
    /// @return The current path of the mission with returns flown with the parameters
    static GeoZoneMissionPath path(MissionController *missionController, const GeoZoneReturnModel::Parameters &returnParameters,
                                   bool withContingencies = false);
    /// @return The visual items of the mission, the planned home first
    static QList<GeoZoneMissionItem> items(MissionController *missionController);

    /// @return The parameters the vehicle returns with. Without them, like for the offline vehicle or before they are
    ///         loaded, the defaults of its firmware and vehicle class, see GeoZoneReturnModel::defaultParameters.
    ///         GeoZoneReturnModel::Autopilot::Unknown without vehicle.
    static GeoZoneReturnModel::Parameters returnParameters(Vehicle *vehicle);
};
