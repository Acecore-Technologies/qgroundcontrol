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
#include <QtPositioning/QGeoCoordinate>

#include <optional>

#include "GeoZone.h"
#include "GeoZoneMissionItem.h"
#include "GeoZoneMissionJumps.h"
#include "GeoZoneMissionPath.h"
#include "GeoZoneReturnModel.h"

/// Builds the GeoZoneMissionPath of a mission from its items with their drawn flight path, which the application reads
/// from the mission the way the mission controller draws it (GeoZoneMissionReader in QGroundControl): the segments of
/// the items in order with their altitudes and terrain, the areas of patterns, the legs between the items with a
/// coordinate and the segment home after a return to launch. The DO_JUMP items are followed (GeoZoneMissionJumps) and
/// the paths the drawn flight path leaves out are added: loiter circles, the distance VTOL transitions overshoot, the
/// loiters of landing patterns and the return after the return to launch item the flight ends with. A pure computation,
/// without the mission controller.
class GeoZoneMissionPathBuilder
{
public:
    /// Transition of a DO_VTOL_TRANSITION item
    enum class VtolTransition {
        None,
        ToMulticopter,  ///< Slowing down to hover after the previous waypoint
        ToFixedWing,    ///< Speeding up towards the next waypoint
    };

    /// Visual item of the mission with its drawn flight path
    struct Item {
        GeoZoneMissionItem item;
        /// Drawn segments of the item itself, like the flight lines of a pattern, or a segment without coordinates for
        /// the distance of a pattern without flight lines yet
        QList<GeoZoneMissionPath::Segment> segments;
        std::optional<GeoZoneMissionPath::Segment> outgoing;    ///< Drawn segment to the next item
        std::optional<GeoZone::Area> area;                      ///< Of survey, corridor and structure scan patterns
        double loiterRadius = 0;    ///< Meters of the loiter circle around loiterCenter at the entry altitude, 0 without
        QGeoCoordinate loiterCenter;
        bool landingLoiter = false; ///< The loiter circle is that of a landing pattern
        VtolTransition vtolTransition = VtolTransition::None;
    };

    struct Parameters {
        QGeoCoordinate home;                            ///< Planned home, which the drawn segment home leads to
        GeoZoneReturnModel::Parameters returnParameters;
        GeoZoneReturnModel::Destinations destinations;  ///< See GeoZoneReturnDestinations::fromMission
        double vtolTransitionDistance = 0;              ///< Meters VTOL transitions take
    };

    /// @return The path of the mission with the items, the planned home first. The contingency paths, which take
    ///         longer to build, are only built withContingencies.
    static GeoZoneMissionPath build(const QList<Item> &items, const Parameters &parameters, bool withContingencies = false);

private:
    /// Paths flown following the DO_JUMP items of the mission
    struct FlightPaths {
        QList<GeoZoneMissionPath::Path> paths;
        QHash<int, double> itemDistances;   ///< Flown when the vehicle first reaches the visual items, by index
        double distance = 0;
        bool repeatsIndefinitely = false;
        int returnItem = -1;                ///< Return to launch item the flight ends with, -1 without
        int returnFromItem = -1;            ///< Item the vehicle returns from, -1 without
    };

    /// @param itemSegments Indexes of the drawn segments of the items, see GeoZoneMissionJumps::walk
    static FlightPaths _flightPaths(const GeoZoneMissionPath &path, const QList<GeoZoneMissionItem> &items,
                                    const QList<GeoZoneMissionJumps::ItemSegments> &itemSegments);
    /// @return The paths the drawn flight path leaves out
    /// @param coordinateItems Indexes of the items flown to
    static QList<GeoZoneMissionPath::Path> _coveragePaths(const GeoZoneMissionPath &path, const QList<Item> &items, const QList<int> &coordinateItems,
                                                          const Parameters &parameters, int returnItem, int returnFromItem);
    /// Adds the returns from along the mission
    /// @param outgoingSequenceNumbers Of the items the drawn segments to the next item lead to, by segment
    static void _appendContingencies(GeoZoneMissionPath &path, const QHash<int, int> &outgoingSequenceNumbers, const Parameters &parameters);
    /// @return Positions along the model's path for zone checks, with terrain
    static QList<GeoZone::Position> _modelPositions(const QList<GeoZone::Position> &positions, double terrainSpacing);
};
