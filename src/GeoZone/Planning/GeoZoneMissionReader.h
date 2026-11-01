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
#include <QtCore/QList>

#include <utility>

#include "GeoZoneMissionItem.h"
#include "GeoZoneMissionJumps.h"
#include "GeoZoneMissionPath.h"
#include "GeoZoneReturnModel.h"

class MissionController;
class Vehicle;

/// Reads the mission of a MissionController into a GeoZoneMissionPath on the GUI thread, the way the controller draws
/// its flight path: the segments of the items in order with their altitudes and terrain, the areas of patterns, the
/// legs between the items with a coordinate, and the segment home after a return to launch. The pure computations then
/// follow the DO_JUMP items (GeoZoneMissionJumps) and the returns to the destinations of the plan
/// (GeoZoneReturnDestinations, GeoZoneReturnModel), and the reader adds the paths the drawn flight path leaves out.
class GeoZoneMissionReader
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneMissionReader)

public:
    /// @return The path of the controller's mission with the return parameters of its vehicle, see
    ///         GeoZoneMissionPath::fromMission
    static GeoZoneMissionPath read(MissionController *missionController, bool withContingencies);
    /// @return The path of the controller's mission, see GeoZoneMissionPath::fromMission
    static GeoZoneMissionPath read(MissionController *missionController, const GeoZoneReturnModel::Parameters &returnParameters, bool withContingencies);

    /// @return The visual items of the mission, the planned home first
    static QList<GeoZoneMissionItem> items(MissionController *missionController);

    /// @return Where the vehicle can return to in the plan: the planned home, the rally points and the breach return
    ///         point above it, and the destinations of the mission items
    static GeoZoneReturnModel::Destinations destinations(MissionController *missionController, const QList<GeoZoneMissionItem> &items);

    /// see GeoZoneMissionPath::returnParameters
    static GeoZoneReturnModel::Parameters returnParameters(Vehicle *vehicle);
    /// see GeoZoneMissionPath::returnAltitude
    static double returnAltitude(Vehicle *vehicle);

private:
    /// Sets the paths flown, following the DO_JUMP items of the mission
    /// @return The return to launch item the flight ends with and the item the vehicle returns from, -1 without
    static std::pair<int, int> _setFlightPaths(GeoZoneMissionPath &path, const QList<GeoZoneMissionItem> &items,
                                               const QList<GeoZoneMissionJumps::ItemSegments> &itemSegments);
    /// Adds the paths the drawn flight path leaves out: loiter circles, VTOL transitions, the loiters of landing patterns
    /// and the return after the return to launch item the flight ends with
    static void _appendCoverage(GeoZoneMissionPath &path, MissionController *missionController, const QList<int> &coordinateItems,
                                const GeoZoneReturnModel::Parameters &returnParameters, const GeoZoneReturnModel::Destinations &destinations,
                                int returnItem, int returnFromItem);
};
