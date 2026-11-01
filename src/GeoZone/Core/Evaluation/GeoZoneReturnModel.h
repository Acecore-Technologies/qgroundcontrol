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
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtPositioning/QGeoCoordinate>

#include <functional>
#include <optional>

#include "GeoZone.h"

/// Paths an autopilot flies when it returns after a return to launch command, a failsafe or a fence breach, computed
/// from its parameters the way the firmware does: the destination among home, the rally points and the landing
/// sequences of the mission, the return altitude with its cone, and what the vehicle does on arrival. A pure
/// computation without the vehicle: the application reads the parameters, see parametersFrom, and the mission. Wind,
/// returns following the terrain, geofence avoidance and the turns of fixed wing vehicles aren't modeled.
///
/// Sources: PX4 src/modules/navigator/rtl.cpp and the Return mode documentation, ArduCopter mode_rtl.cpp, ArduPlane
/// mode_rtl.cpp and fence.cpp, AP_Rally and AP_Mission of ArduPilot.
class GeoZoneReturnModel
{
public:
    enum class Autopilot {
        Unknown,        ///< Returns to the closest of home and the rally points at the generic return altitude, if any
        PX4,
        ArduCopter,
        ArduPlane,
    };

    enum class Firmware {
        Other,
        PX4,
        ArduPilot,
    };

    enum class VehicleClass {
        Other,
        Multirotor,
        FixedWing,
        Vtol,
    };

    /// What the vehicle does after breaching the fence
    enum class BreachAction {
        None,           ///< Reports, holds, brakes, lands or terminates where it is
        Return,         ///< Returns like returnPath
        ReturnPoint,    ///< Flies to the breach return point of the fence and loiters there, see breachPath
    };

    /// Parameters of the vehicle in meters and degrees. Altitudes "above home" are relative to the home altitude.
    struct Parameters {
        Autopilot autopilot = Autopilot::Unknown;
        bool multicopter = false;               ///< PX4 only lowers the return altitude in its cone for multicopters
        bool vtol = false;                      ///< PX4 VTOLs land at home or rally points like multicopters
        /// PX4 RTL_RETURN_ALT above the destination. ArduCopter RTL_ALT above home, 0 to return at the current altitude.
        /// ArduPlane RTL_ALTITUDE above home, negative to return at the current altitude. Otherwise above home, 0 or less
        /// to return at the current altitude. NaN when unknown.
        double returnAltitude = qQNaN();
        double climbMinimum = 0;                ///< ArduPilot RTL_CLIMB_MIN, climbed before turning home

        int returnType = 0;                     ///< PX4 RTL_TYPE
        double descendAltitude = qQNaN();       ///< PX4 RTL_DESCEND_ALT above the destination, NaN to land without loitering above it
        double landDelay = 0;                   ///< PX4 RTL_LAND_DELAY seconds, negative to loiter at the descend altitude
        double coneHalfAngle = 0;               ///< PX4 RTL_CONE_ANG, 0 without a cone
        double coneRadius = 0;                  ///< PX4 RTL_MIN_DIST, from the destination, where the cone lowers the return altitude
        double acceptanceRadius = 0;            ///< PX4 NAV_ACC_RAD

        double coneSlope = 0;                   ///< ArduCopter RTL_CONE_SLOPE, below minConeSlope without a cone
        double finalAltitude = 0;               ///< ArduCopter RTL_ALT_FINAL above home, 0 to land
        double returnAltitudeLimit = qQNaN();   ///< ArduCopter FENCE_ALT_MAX less FENCE_MARGIN above home with the altitude fence

        int autoLand = 0;                       ///< ArduPlane RTL_AUTOLAND, see AutoLand
        double loiterRadius = 0;                ///< ArduPlane RTL_RADIUS or WP_LOITER_RAD, PX4 NAV_LOITER_RAD for fixed wings, 0 without circles

        bool rallyIncludesHome = true;          ///< ArduPilot RALLY_INCL_HOME
        double rallyLimit = 0;                  ///< ArduPilot RALLY_LIMIT_KM in meters, 0 without limit

        BreachAction breachAction = BreachAction::None;
        bool breachReturnToRally = false;       ///< ArduPlane FENCE_RET_RALLY
        double breachReturnAltitude = 0;        ///< ArduPlane FENCE_RET_ALT above home, 0 for the middle of the altitude fence
        double fenceAltitudeMin = qQNaN();      ///< ArduPlane FENCE_ALT_MIN plus FENCE_MARGIN above home
        double fenceAltitudeMax = qQNaN();      ///< ArduPlane FENCE_ALT_MAX less FENCE_MARGIN above home

        /// @return true if the return altitude is known
        bool altitudeKnown() const { return !qIsNaN(returnAltitude); }
    };

    /// ArduPlane RTL_AUTOLAND values
    enum AutoLand {
        AutoLandDisabled = 0,
        AutoLandAfterHome = 1,          ///< Flies home or to a rally point, then the closest landing sequence
        AutoLandImmediately = 2,        ///< Flies the closest landing sequence directly
        AutoLandGoAroundOnly = 3,
        AutoLandReturnPath = 4,         ///< Joins the mission's return path after DO_RETURN_PATH_START
    };

    /// Landing sequence of the mission, from a DO_LAND_START
    struct Landing {
        QGeoCoordinate marker;          ///< Of the DO_LAND_START, or its first position, ArduPilot flies to the closest
        GeoZone::Position entry;        ///< First position the vehicle flies to in the sequence
        GeoZone::Position touchdown;    ///< Where it lands, PX4 picks the sequence when it is closer than the other destinations
        int sequenceNumber = -1;        ///< Of the DO_LAND_START
    };

    /// Where the vehicle may return to, with altitudes above mean sea level
    struct Destinations {
        GeoZone::Position home;                 ///< The home altitude is the ground there
        QList<GeoZone::Position> rallyPoints;
        QList<Landing> landings;
        GeoZone::Position firstWaypoint;        ///< PX4 returns along the mission in reverse to it, invalid without mission
        QList<GeoZone::Position> returnPath;    ///< Positions after an ArduPlane DO_RETURN_PATH_START, up to the landing
        GeoZone::Position breachReturnPoint;    ///< Of the fence, invalid without
    };

    /// Where the vehicle starts returning
    struct Start {
        GeoZone::Position position;
        int sequenceNumber = -1;        ///< Of the mission item flown to, -1 outside of a mission
    };

    enum class Destination {
        None,                   ///< The vehicle doesn't fly anywhere, like PX4 landing without rally points with RTL_TYPE 5
        Home,
        RallyPoint,
        MissionLanding,         ///< Then the landing sequence of the mission
        ReturnPath,             ///< Then the return path of the mission
        ReverseMission,         ///< Along the mission in reverse, then home
        MissionForward,         ///< Along the rest of the mission to its landing, no other path
        BreachReturnPoint,
    };

    /// Path flown back
    struct Path {
        /// From the start, altitudes NaN when unknown, the terrain only where the destinations have it. Paths along the
        /// mission aren't included, see Destination.
        QList<GeoZone::Position> positions;
        QList<GeoZone::Position> loiter;    ///< Circle flown at the destination, empty without
        Destination destination = Destination::None;
        int destinationIndex = -1;          ///< Of the rally point or landing
        bool altitudeKnown = false;

        bool isValid() const { return positions.count() >= 2; }
    };

    /// @return Value of the parameter, nullopt when the vehicle doesn't have it
    using ParameterLookup = std::function<std::optional<double>(const QString &name)>;

    /// @return The parameters of a vehicle, with their units converted, from those it has. Vehicles of other firmware
    ///         or classes, like rovers, get Autopilot::Unknown with a generic return altitude if they have one.
    static Parameters parametersFrom(Firmware firmware, VehicleClass vehicleClass, const ParameterLookup &lookup);
    /// @return Names of the parameters parametersFrom may read
    static const QStringList &parameterNames();

    /// @return The fence breach action for the autopilot's FENCE_ACTION or GF_ACTION. Plane firmware before 4.1, with
    ///         FENCE_RETALT, numbers its actions differently.
    static BreachAction breachAction(Autopilot autopilot, int action, bool legacyPlaneFence = false);

    /// @return The path the vehicle flies when it returns from the start
    static Path returnPath(const Parameters &parameters, const Destinations &destinations, const Start &start);
    /// @return The path the vehicle flies after breaching the fence at the start with BreachAction::ReturnPoint, invalid
    ///         for the other actions
    static Path breachPath(const Parameters &parameters, const Destinations &destinations, const Start &start);

    /// @return Destination index of the closest rally point or -1 for home, by ArduPilot's AP_Rally rules
    static int arduPilotRallyPoint(const Parameters &parameters, const Destinations &destinations, const QGeoCoordinate &from);

    /// @return Altitude AMSL PX4 returns at from the start to the destination, with its cone for multicopters
    static double px4ReturnAltitude(const Parameters &parameters, const GeoZone::Position &start, const GeoZone::Position &destination);
    /// @return Altitude above home ArduCopter returns at from the start, which is altitudeAboveHome above home, to the
    ///         destination, which is destinationAboveHome above home
    static double arduCopterReturnAltitude(const Parameters &parameters, double altitudeAboveHome, double destinationAboveHome, double distance);

    static constexpr double minConeSlope = 0.5;         ///< ArduCopter RTL_MIN_CONE_SLOPE
    static constexpr double minCopterAltitude = 0.3;    ///< ArduCopter RTL_ALT_MIN_M
    static constexpr double px4DestinationMargin = 2;   ///< PX4 MIN_DIST_THRESHOLD, a destination must be closer by it
    static constexpr int circleVertexCount = 36;

    /// @return Closed circle around the center at the radius and altitude
    static QList<GeoZone::Position> circle(const QGeoCoordinate &center, double radius, double altitudeAMSL, double terrainAMSL = qQNaN());
};
