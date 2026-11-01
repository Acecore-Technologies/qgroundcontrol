/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneReturnModel.h"

#include <QtCore/QHash>
#include <QtCore/QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

using Position = GeoZone::Position;

constexpr double infinity = std::numeric_limits<double>::infinity();

bool sameAltitude(double a, double b)
{
    return (a == b) || (qIsNaN(a) && qIsNaN(b));
}

/// Appends the position unless the path is already there
void append(QList<Position> &positions, const QGeoCoordinate &coordinate, double altitudeAMSL, double terrainAMSL = qQNaN())
{
    const Position position{ QGeoCoordinate(coordinate.latitude(), coordinate.longitude()), altitudeAMSL, terrainAMSL };
    if (!positions.isEmpty() && sameAltitude(positions.last().altitudeAMSL, altitudeAMSL) &&
        (positions.last().coordinate.distanceTo(position.coordinate) < 0.01)) {
        return;
    }
    positions.append(position);
}

void appendStart(QList<Position> &positions, const GeoZoneReturnModel::Start &start)
{
    append(positions, start.position.coordinate, start.position.altitudeAMSL, start.position.terrainAMSL);
}

/// Altitude the vehicle lands at, the ground at the destination if known, else at home
double groundAt(const Position &destination, const Position &home)
{
    return qIsNaN(destination.terrainAMSL) ? home.altitudeAMSL : destination.terrainAMSL;
}

/// @return NaN if any altitude is unknown, else the value
double knownOrNaN(double value, std::initializer_list<double> altitudes)
{
    for (const double altitude : altitudes) {
        if (qIsNaN(altitude)) {
            return qQNaN();
        }
    }
    return value;
}

/// @return Index of the landing whose marker is closest to the coordinate, -1 without landings
int closestLanding(const QList<GeoZoneReturnModel::Landing> &landings, const QGeoCoordinate &from)
{
    int closest = -1;
    double closestDistance = infinity;
    for (qsizetype i = 0; i < landings.count(); i++) {
        const QGeoCoordinate marker = landings[i].marker.isValid() ? landings[i].marker : landings[i].entry.coordinate;
        if (!marker.isValid()) {
            continue;
        }
        const double distance = from.distanceTo(marker);
        if (distance < closestDistance) {
            closestDistance = distance;
            closest = static_cast<int>(i);
        }
    }
    return closest;
}

/// ArduPlane's flight to a destination at the target altitude: climbing as fast as it can when below it, descending
/// gradually along the way when above it. The climb is drawn vertically at the start.
void appendPlaneLeg(QList<Position> &positions, const GeoZoneReturnModel::Start &start, const QGeoCoordinate &to, double targetAMSL)
{
    appendStart(positions, start);
    const double altitude = start.position.altitudeAMSL;
    if (qIsNaN(targetAMSL) || qIsNaN(altitude)) {
        append(positions, start.position.coordinate, qQNaN());
        append(positions, to, qQNaN());
        return;
    }
    if (targetAMSL > altitude) {
        append(positions, start.position.coordinate, targetAMSL);
    }
    append(positions, to, targetAMSL);
}

GeoZoneReturnModel::Path unknownReturn(const GeoZoneReturnModel::Parameters &parameters, const GeoZoneReturnModel::Destinations &destinations,
                                       const GeoZoneReturnModel::Start &start)
{
    GeoZoneReturnModel::Path path;
    const QGeoCoordinate &from = start.position.coordinate;
    Position destination = destinations.home;
    path.destination = GeoZoneReturnModel::Destination::Home;
    double closest = destinations.home.coordinate.isValid() ? from.distanceTo(destinations.home.coordinate) : infinity;
    for (qsizetype i = 0; i < destinations.rallyPoints.count(); i++) {
        const Position &rallyPoint = destinations.rallyPoints[i];
        if (rallyPoint.coordinate.isValid() && (from.distanceTo(rallyPoint.coordinate) < closest)) {
            closest = from.distanceTo(rallyPoint.coordinate);
            destination = rallyPoint;
            path.destination = GeoZoneReturnModel::Destination::RallyPoint;
            path.destinationIndex = static_cast<int>(i);
        }
    }
    if (!destination.coordinate.isValid()) {
        path.destination = GeoZoneReturnModel::Destination::None;
        return path;
    }

    const double altitude = start.position.altitudeAMSL;
    const double homeAltitude = destinations.home.altitudeAMSL;
    double cruise = qQNaN();
    if (parameters.altitudeKnown()) {
        cruise = knownOrNaN((parameters.returnAltitude > 0) ? (std::max)(altitude, homeAltitude + parameters.returnAltitude) : altitude,
                            { altitude, homeAltitude });
    }
    appendStart(path.positions, start);
    append(path.positions, from, cruise);
    append(path.positions, destination.coordinate, cruise);
    append(path.positions, destination.coordinate, destination.altitudeAMSL, destination.terrainAMSL);
    path.altitudeKnown = !qIsNaN(cruise);
    return path;
}

GeoZoneReturnModel::Path px4Return(const GeoZoneReturnModel::Parameters &parameters, const GeoZoneReturnModel::Destinations &destinations,
                                   const GeoZoneReturnModel::Start &start)
{
    using Destination = GeoZoneReturnModel::Destination;
    GeoZoneReturnModel::Path path;
    const QGeoCoordinate &from = start.position.coordinate;
    const Position &home = destinations.home;
    const int type = parameters.returnType;
    const bool fixedWing = !parameters.multicopter && !parameters.vtol;

    // Mission path returns: on to the landing of the mission or back along it, home after the first waypoint
    if ((type == 2) || (type == 4)) {
        const bool hasLanding = !destinations.landings.isEmpty();
        const bool forward = hasLanding && ((type == 2) ||
                             ((start.sequenceNumber >= 0) && ((destinations.landings.first().sequenceNumber - start.sequenceNumber) < start.sequenceNumber)));
        if (forward) {
            path.destination = Destination::MissionForward;
            path.destinationIndex = 0;
            path.altitudeKnown = true;
            return path;
        }
        if (destinations.firstWaypoint.coordinate.isValid() && home.coordinate.isValid()) {
            const Position &first = destinations.firstWaypoint;
            const double cruise = GeoZoneReturnModel::px4ReturnAltitude(parameters, first, home);
            append(path.positions, first.coordinate, first.altitudeAMSL, first.terrainAMSL);
            append(path.positions, first.coordinate, cruise);
            append(path.positions, home.coordinate, cruise);
            const double descend = qIsNaN(parameters.descendAltitude) ? home.altitudeAMSL : (std::min)(cruise, home.altitudeAMSL + parameters.descendAltitude);
            append(path.positions, home.coordinate, qIsNaN(cruise) ? qQNaN() : descend);
            if (parameters.landDelay >= 0) {
                append(path.positions, home.coordinate, home.altitudeAMSL, home.terrainAMSL);
            }
            path.destination = Destination::ReverseMission;
            path.altitudeKnown = !qIsNaN(cruise);
            return path;
        }
    }

    // The closest destination the type allows, which must be closer than the previous candidate by the margin. Mission
    // path returns without mission fly home directly.
    Position destination = home;
    path.destination = home.coordinate.isValid() ? Destination::Home : Destination::None;
    double minDistance = infinity;
    const bool missionPathType = (type == 2) || (type == 4);
    if (missionPathType) {
        minDistance = 0;
    } else if (type != 5) {
        if ((type != 1) && home.coordinate.isValid()) {
            minDistance = from.distanceTo(home.coordinate);
        }
        if (((type == 1) || (type == 3) || std::isinf(minDistance)) && !destinations.landings.isEmpty()) {
            const GeoZoneReturnModel::Landing &landing = destinations.landings.first();
            const double distance = from.distanceTo(landing.touchdown.coordinate);
            if ((distance + GeoZoneReturnModel::px4DestinationMargin) < minDistance) {
                minDistance = distance;
                destination = landing.touchdown;
                path.destination = Destination::MissionLanding;
                path.destinationIndex = 0;
            }
        }
    }
    for (qsizetype i = 0; i < destinations.rallyPoints.count(); i++) {
        const Position &rallyPoint = destinations.rallyPoints[i];
        if (!rallyPoint.coordinate.isValid()) {
            continue;
        }
        const double distance = from.distanceTo(rallyPoint.coordinate);
        if ((distance + GeoZoneReturnModel::px4DestinationMargin) < minDistance) {
            minDistance = distance;
            destination = rallyPoint;
            path.destination = Destination::RallyPoint;
            path.destinationIndex = static_cast<int>(i);
        }
    }
    // Without rally points it lands where it is
    if ((type == 5) && (path.destination != Destination::RallyPoint)) {
        path.destination = Destination::None;
        path.altitudeKnown = true;
        return path;
    }
    if (path.destination == Destination::None) {
        return path;
    }

    const double cruise = GeoZoneReturnModel::px4ReturnAltitude(parameters, start.position, destination);
    appendStart(path.positions, start);
    append(path.positions, from, cruise);
    path.altitudeKnown = !qIsNaN(cruise);
    if (path.destination == Destination::MissionLanding) {
        // Then the landing sequence from its first position
        const Position &entry = destinations.landings.first().entry;
        append(path.positions, entry.coordinate, qIsNaN(cruise) ? qQNaN() : entry.altitudeAMSL, entry.terrainAMSL);
        return path;
    }

    // Down to the descend altitude above the destination, then landing or loitering
    append(path.positions, destination.coordinate, cruise);
    const double descend = qIsNaN(parameters.descendAltitude) ? destination.altitudeAMSL :
                                                                (std::min)(cruise, destination.altitudeAMSL + parameters.descendAltitude);
    const double loiterAltitude = qIsNaN(cruise) ? qQNaN() : descend;
    append(path.positions, destination.coordinate, loiterAltitude);
    if (fixedWing && (parameters.loiterRadius > 0)) {
        path.loiter = GeoZoneReturnModel::circle(destination.coordinate, parameters.loiterRadius, loiterAltitude, destination.terrainAMSL);
    }
    if (parameters.landDelay >= 0) {
        append(path.positions, destination.coordinate, destination.altitudeAMSL, destination.terrainAMSL);
    }
    return path;
}

GeoZoneReturnModel::Path arduCopterReturn(const GeoZoneReturnModel::Parameters &parameters, const GeoZoneReturnModel::Destinations &destinations,
                                          const GeoZoneReturnModel::Start &start)
{
    GeoZoneReturnModel::Path path;
    const QGeoCoordinate &from = start.position.coordinate;
    const Position &home = destinations.home;
    if (!home.coordinate.isValid()) {
        return path;
    }
    const int rallyIndex = GeoZoneReturnModel::arduPilotRallyPoint(parameters, destinations, from);
    const Position &destination = (rallyIndex >= 0) ? destinations.rallyPoints[rallyIndex] : home;
    path.destination = (rallyIndex >= 0) ? GeoZoneReturnModel::Destination::RallyPoint : GeoZoneReturnModel::Destination::Home;
    path.destinationIndex = rallyIndex;

    // Climbs above its position, returns at the altitude above home, then descends to the final altitude above home or lands
    const double homeAltitude = home.altitudeAMSL;
    const double aboveHome = start.position.altitudeAMSL - homeAltitude;
    const double destinationAboveHome = (rallyIndex >= 0) ? (destination.altitudeAMSL - homeAltitude) : 0.0;
    double cruise = qQNaN();
    if (parameters.altitudeKnown()) {
        cruise = knownOrNaN(homeAltitude + GeoZoneReturnModel::arduCopterReturnAltitude(parameters, aboveHome, destinationAboveHome, from.distanceTo(destination.coordinate)),
                            { aboveHome, destinationAboveHome });
    }
    appendStart(path.positions, start);
    append(path.positions, from, cruise);
    append(path.positions, destination.coordinate, cruise);
    if (parameters.finalAltitude > 0) {
        append(path.positions, destination.coordinate, qIsNaN(cruise) ? qQNaN() : (homeAltitude + parameters.finalAltitude));
    } else {
        append(path.positions, destination.coordinate, groundAt(destination, home), destination.terrainAMSL);
    }
    path.altitudeKnown = !qIsNaN(cruise);
    return path;
}

GeoZoneReturnModel::Path arduPlaneReturn(const GeoZoneReturnModel::Parameters &parameters, const GeoZoneReturnModel::Destinations &destinations,
                                         const GeoZoneReturnModel::Start &start)
{
    using Destination = GeoZoneReturnModel::Destination;
    GeoZoneReturnModel::Path path;
    const QGeoCoordinate &from = start.position.coordinate;
    const Position &home = destinations.home;
    const double altitude = start.position.altitudeAMSL;

    // In mission mode straight to the closest landing sequence, or to the closest position of the return path
    if (parameters.autoLand == GeoZoneReturnModel::AutoLandImmediately) {
        const int landing = closestLanding(destinations.landings, from);
        if (landing >= 0) {
            const Position &entry = destinations.landings[landing].entry;
            appendStart(path.positions, start);
            append(path.positions, entry.coordinate, entry.altitudeAMSL, entry.terrainAMSL);
            path.destination = Destination::MissionLanding;
            path.destinationIndex = landing;
            path.altitudeKnown = !qIsNaN(altitude) && !qIsNaN(entry.altitudeAMSL);
            return path;
        }
    } else if ((parameters.autoLand == GeoZoneReturnModel::AutoLandReturnPath) && !destinations.returnPath.isEmpty()) {
        const auto closest = std::min_element(destinations.returnPath.cbegin(), destinations.returnPath.cend(), [&from](const Position &a, const Position &b) {
            return from.distanceTo(a.coordinate) < from.distanceTo(b.coordinate);
        });
        appendStart(path.positions, start);
        append(path.positions, closest->coordinate, closest->altitudeAMSL, closest->terrainAMSL);
        path.destination = Destination::ReturnPath;
        path.destinationIndex = static_cast<int>(closest - destinations.returnPath.cbegin());
        path.altitudeKnown = !qIsNaN(altitude) && !qIsNaN(closest->altitudeAMSL);
        return path;
    }

    if (!home.coordinate.isValid()) {
        return path;
    }
    const int rallyIndex = GeoZoneReturnModel::arduPilotRallyPoint(parameters, destinations, from);
    const Position &destination = (rallyIndex >= 0) ? destinations.rallyPoints[rallyIndex] : home;
    path.destination = (rallyIndex >= 0) ? Destination::RallyPoint : Destination::Home;
    path.destinationIndex = rallyIndex;
    // At the rally point's altitude, else at the return altitude above home
    double target = qQNaN();
    if (rallyIndex >= 0) {
        target = parameters.altitudeKnown() ? destination.altitudeAMSL : qQNaN();
    } else if (parameters.altitudeKnown()) {
        target = (parameters.returnAltitude < 0) ? altitude : (home.altitudeAMSL + parameters.returnAltitude);
    }
    appendPlaneLeg(path.positions, start, destination.coordinate, target);
    path.altitudeKnown = !qIsNaN(target) && !qIsNaN(altitude);

    // Then the landing sequence closest to the destination, else loitering there
    if (parameters.autoLand == GeoZoneReturnModel::AutoLandAfterHome) {
        const int landing = closestLanding(destinations.landings, destination.coordinate);
        if (landing >= 0) {
            const Position &entry = destinations.landings[landing].entry;
            append(path.positions, entry.coordinate, path.altitudeKnown ? entry.altitudeAMSL : qQNaN(), entry.terrainAMSL);
            path.destination = Destination::MissionLanding;
            path.destinationIndex = landing;
            return path;
        }
    }
    if (parameters.loiterRadius > 0) {
        path.loiter = GeoZoneReturnModel::circle(destination.coordinate, parameters.loiterRadius, path.altitudeKnown ? target : qQNaN(), destination.terrainAMSL);
    }
    return path;
}

std::optional<double> lookupFirst(const GeoZoneReturnModel::ParameterLookup &lookup, std::initializer_list<std::pair<const char*, double>> names)
{
    for (const auto &[name, scale] : names) {
        if (const std::optional<double> value = lookup(QString::fromLatin1(name))) {
            return *value * scale;
        }
    }
    return std::nullopt;
}

} // namespace

const QStringList &GeoZoneReturnModel::parameterNames()
{
    static const QStringList names = {
        // PX4
        QStringLiteral("RTL_TYPE"), QStringLiteral("RTL_RETURN_ALT"), QStringLiteral("RTL_DESCEND_ALT"), QStringLiteral("RTL_LAND_DELAY"),
        QStringLiteral("RTL_CONE_ANG"), QStringLiteral("RTL_MIN_DIST"), QStringLiteral("NAV_ACC_RAD"), QStringLiteral("NAV_LOITER_RAD"),
        QStringLiteral("GF_ACTION"),
        // ArduCopter, the meter parameters since 4.7
        QStringLiteral("RTL_ALT"), QStringLiteral("RTL_ALT_M"), QStringLiteral("RTL_CLIMB_MIN"), QStringLiteral("RTL_CLIMB_MIN_M"),
        QStringLiteral("RTL_ALT_FINAL"), QStringLiteral("RTL_ALT_FINAL_M"), QStringLiteral("RTL_CONE_SLOPE"),
        // ArduPlane
        QStringLiteral("RTL_ALTITUDE"), QStringLiteral("ALT_HOLD_RTL"), QStringLiteral("RTL_AUTOLAND"), QStringLiteral("RTL_RADIUS"),
        QStringLiteral("WP_LOITER_RAD"),
        // ArduPilot rally points and fence
        QStringLiteral("RALLY_INCL_HOME"), QStringLiteral("RALLY_LIMIT_KM"),
        QStringLiteral("FENCE_ENABLE"), QStringLiteral("FENCE_AUTOENABLE"), QStringLiteral("FENCE_ACTION"), QStringLiteral("FENCE_TYPE"),
        QStringLiteral("FENCE_ALT_MAX"), QStringLiteral("FENCE_ALT_MIN"), QStringLiteral("FENCE_MARGIN"), QStringLiteral("FENCE_RET_RALLY"),
        QStringLiteral("FENCE_RET_ALT"), QStringLiteral("FENCE_RETALT"), QStringLiteral("FENCE_MAXALT"), QStringLiteral("FENCE_MINALT"),
    };
    return names;
}

GeoZoneReturnModel::Parameters GeoZoneReturnModel::parametersFrom(Firmware firmware, VehicleClass vehicleClass, const ParameterLookup &lookup)
{
    Parameters parameters;
    const auto value = [&lookup](const char *name, double defaultValue) {
        return lookup(QString::fromLatin1(name)).value_or(defaultValue);
    };

    if (firmware == Firmware::PX4) {
        parameters.autopilot = Autopilot::PX4;
        parameters.multicopter = (vehicleClass == VehicleClass::Multirotor);
        parameters.vtol = (vehicleClass == VehicleClass::Vtol);
        parameters.returnType = qRound(value("RTL_TYPE", 0));
        parameters.returnAltitude = value("RTL_RETURN_ALT", qQNaN());
        parameters.descendAltitude = value("RTL_DESCEND_ALT", qQNaN());
        parameters.landDelay = value("RTL_LAND_DELAY", 0);
        parameters.coneHalfAngle = value("RTL_CONE_ANG", 0);
        parameters.coneRadius = value("RTL_MIN_DIST", 0);
        parameters.acceptanceRadius = value("NAV_ACC_RAD", 0);
        if (vehicleClass == VehicleClass::FixedWing) {
            parameters.loiterRadius = std::abs(value("NAV_LOITER_RAD", 0));
        }
        parameters.breachAction = breachAction(Autopilot::PX4, qRound(value("GF_ACTION", 0)));
        return parameters;
    }

    const bool arduCopter = (firmware == Firmware::ArduPilot) && (vehicleClass == VehicleClass::Multirotor);
    const bool arduPlane = (firmware == Firmware::ArduPilot) && ((vehicleClass == VehicleClass::FixedWing) || (vehicleClass == VehicleClass::Vtol));
    if (!arduCopter && !arduPlane) {
        // Return altitudes above home of other vehicles, 0 or less for the current altitude
        const std::optional<double> altitude = lookupFirst(lookup, { { "RTL_RETURN_ALT", 1.0 }, { "RTL_ALTITUDE", 1.0 }, { "RTL_ALT_M", 1.0 },
                                                                     { "RTL_ALT", 0.01 }, { "ALT_HOLD_RTL", 0.01 } });
        parameters.returnAltitude = altitude ? (std::max)(*altitude, 0.0) : qQNaN();
        return parameters;
    }

    parameters.rallyIncludesHome = value("RALLY_INCL_HOME", arduCopter ? 1 : 0) != 0;
    parameters.rallyLimit = value("RALLY_LIMIT_KM", 0) * 1000;
    // The fence acts when it is enabled, by its parameter or automatically, see FENCE_AUTOENABLE
    const bool fenceEnabled = (value("FENCE_ENABLE", 1) != 0) || (value("FENCE_AUTOENABLE", 0) != 0);

    if (arduCopter) {
        parameters.autopilot = Autopilot::ArduCopter;
        parameters.multicopter = true;
        // Centimeters until 4.6
        parameters.returnAltitude = lookupFirst(lookup, { { "RTL_ALT_M", 1.0 }, { "RTL_ALT", 0.01 } }).value_or(qQNaN());
        parameters.climbMinimum = lookupFirst(lookup, { { "RTL_CLIMB_MIN_M", 1.0 }, { "RTL_CLIMB_MIN", 0.01 } }).value_or(0);
        parameters.finalAltitude = lookupFirst(lookup, { { "RTL_ALT_FINAL_M", 1.0 }, { "RTL_ALT_FINAL", 0.01 } }).value_or(0);
        parameters.coneSlope = value("RTL_CONE_SLOPE", 0);
        // The maximum altitude of the fence caps the return altitude
        const int fenceTypes = qRound(value("FENCE_TYPE", 0));
        if (fenceEnabled && ((fenceTypes & 1) != 0) && lookup(QStringLiteral("FENCE_ALT_MAX"))) {
            parameters.returnAltitudeLimit = value("FENCE_ALT_MAX", 0) - value("FENCE_MARGIN", 0);
        }
        parameters.breachAction = fenceEnabled ? breachAction(Autopilot::ArduCopter, qRound(value("FENCE_ACTION", 0))) : BreachAction::None;
        return parameters;
    }

    parameters.autopilot = Autopilot::ArduPlane;
    parameters.vtol = (vehicleClass == VehicleClass::Vtol);
    // ALT_HOLD_RTL in centimeters until 4.5, negative for the current altitude
    if (const std::optional<double> altitude = lookup(QStringLiteral("RTL_ALTITUDE"))) {
        parameters.returnAltitude = *altitude;
    } else if (const std::optional<double> altitude = lookup(QStringLiteral("ALT_HOLD_RTL"))) {
        parameters.returnAltitude = (*altitude < 0) ? -1.0 : (*altitude / 100);
    }
    parameters.climbMinimum = value("RTL_CLIMB_MIN", 0);
    parameters.autoLand = qRound(value("RTL_AUTOLAND", 0));
    const double rtlRadius = std::abs(value("RTL_RADIUS", 0));
    parameters.loiterRadius = (rtlRadius > 0) ? rtlRadius : std::abs(value("WP_LOITER_RAD", 0));
    parameters.breachReturnToRally = value("FENCE_RET_RALLY", 0) != 0;
    // The fence of Plane until 4.0 has its own parameters
    const bool legacyFence = lookup(QStringLiteral("FENCE_RETALT")).has_value();
    if (legacyFence) {
        parameters.breachReturnAltitude = value("FENCE_RETALT", 0);
        parameters.fenceAltitudeMin = value("FENCE_MINALT", qQNaN());
        parameters.fenceAltitudeMax = value("FENCE_MAXALT", qQNaN());
    } else {
        const double margin = value("FENCE_MARGIN", 0);
        parameters.breachReturnAltitude = value("FENCE_RET_ALT", 0);
        parameters.fenceAltitudeMin = value("FENCE_ALT_MIN", qQNaN()) + margin;
        parameters.fenceAltitudeMax = value("FENCE_ALT_MAX", qQNaN()) - margin;
    }
    parameters.breachAction = fenceEnabled ? breachAction(Autopilot::ArduPlane, qRound(value("FENCE_ACTION", 0)), legacyFence) : BreachAction::None;
    return parameters;
}

GeoZoneReturnModel::Parameters GeoZoneReturnModel::defaultParameters(Firmware firmware, VehicleClass vehicleClass)
{
    QHash<QString, double> defaults;
    if (firmware == Firmware::PX4) {
        // Parameter defaults of PX4 1.14 to 1.16 and main (rtl_params, navigator_params, geofence_params)
        defaults = { { QStringLiteral("RTL_TYPE"), 0 }, { QStringLiteral("RTL_RETURN_ALT"), 60 }, { QStringLiteral("RTL_DESCEND_ALT"), 30 },
                     { QStringLiteral("RTL_LAND_DELAY"), 0 }, { QStringLiteral("RTL_CONE_ANG"), 45 }, { QStringLiteral("RTL_MIN_DIST"), 10 },
                     { QStringLiteral("NAV_ACC_RAD"), 10 }, { QStringLiteral("NAV_LOITER_RAD"), 80 }, { QStringLiteral("GF_ACTION"), 2 } };
        // Changed by the airframe defaults of the vehicle class, rc.mc_defaults, rc.fw_defaults and rc.vtol_defaults
        switch (vehicleClass) {
        case VehicleClass::Multirotor:
            defaults.insert(QStringLiteral("RTL_RETURN_ALT"), 30);
            defaults.insert(QStringLiteral("RTL_DESCEND_ALT"), 10);
            defaults.insert(QStringLiteral("NAV_ACC_RAD"), 2);
            break;
        case VehicleClass::FixedWing:
            defaults.insert(QStringLiteral("RTL_TYPE"), 1);
            defaults.insert(QStringLiteral("RTL_RETURN_ALT"), 100);
            defaults.insert(QStringLiteral("RTL_DESCEND_ALT"), 100);
            defaults.insert(QStringLiteral("RTL_LAND_DELAY"), -1);
            break;
        case VehicleClass::Vtol:
            defaults.insert(QStringLiteral("RTL_TYPE"), 1);
            break;
        case VehicleClass::Other:
            break;
        }
    } else if ((firmware == Firmware::ArduPilot) && (vehicleClass == VehicleClass::Multirotor)) {
        // ArduCopter 4.6, in centimeters, with the fence disabled
        defaults = { { QStringLiteral("RTL_ALT"), 1500 }, { QStringLiteral("RTL_CLIMB_MIN"), 0 }, { QStringLiteral("RTL_ALT_FINAL"), 0 },
                     { QStringLiteral("RTL_CONE_SLOPE"), 3 }, { QStringLiteral("RALLY_INCL_HOME"), 1 }, { QStringLiteral("RALLY_LIMIT_KM"), 0.3 },
                     { QStringLiteral("FENCE_ENABLE"), 0 }, { QStringLiteral("FENCE_AUTOENABLE"), 0 }, { QStringLiteral("FENCE_ACTION"), 1 } };
    } else if ((firmware == Firmware::ArduPilot) && ((vehicleClass == VehicleClass::FixedWing) || (vehicleClass == VehicleClass::Vtol))) {
        // ArduPlane 4.5 and later, with the fence disabled
        defaults = { { QStringLiteral("RTL_ALTITUDE"), 100 }, { QStringLiteral("RTL_CLIMB_MIN"), 0 }, { QStringLiteral("RTL_AUTOLAND"), 0 },
                     { QStringLiteral("RTL_RADIUS"), 0 }, { QStringLiteral("WP_LOITER_RAD"), 60 }, { QStringLiteral("RALLY_INCL_HOME"), 0 },
                     { QStringLiteral("RALLY_LIMIT_KM"), 5 }, { QStringLiteral("FENCE_ENABLE"), 0 }, { QStringLiteral("FENCE_AUTOENABLE"), 0 },
                     { QStringLiteral("FENCE_ACTION"), 1 } };
    } else {
        return Parameters();
    }
    Parameters parameters = parametersFrom(firmware, vehicleClass, [&defaults](const QString &name) -> std::optional<double> {
        const auto it = defaults.constFind(name);
        return (it != defaults.cend()) ? std::optional<double>(*it) : std::nullopt;
    });
    parameters.assumed = true;
    return parameters;
}

QString GeoZoneReturnModel::autopilotName(Autopilot autopilot)
{
    switch (autopilot) {
    case Autopilot::PX4:
        return QStringLiteral("PX4");
    case Autopilot::ArduCopter:
        return QStringLiteral("ArduCopter");
    case Autopilot::ArduPlane:
        return QStringLiteral("ArduPlane");
    case Autopilot::Unknown:
        break;
    }
    return QString();
}

GeoZoneReturnModel::BreachAction GeoZoneReturnModel::breachAction(Autopilot autopilot, int action, bool legacyPlaneFence)
{
    switch (autopilot) {
    case Autopilot::PX4:
        // GF_ACTION: 0 none, 1 warning, 2 hold, 3 return, 4 terminate, 5 land
        return (action == 3) ? BreachAction::Return : BreachAction::None;
    case Autopilot::ArduCopter:
        // FENCE_ACTION: 0 report, 1 RTL or land, 2 land, 3 SmartRTL or RTL or land, 4 brake or land, 5 SmartRTL or land.
        // SmartRTL retraces the path flown.
        return (action == 1) ? BreachAction::Return : BreachAction::None;
    case Autopilot::ArduPlane:
        if (legacyPlaneFence) {
            // FENCE_ACTION: 0 none, 1 guided, 2 report, 3 guided with throttle pass-through, 4 RTL
            if ((action == 1) || (action == 3)) {
                return BreachAction::ReturnPoint;
            }
            return (action == 4) ? BreachAction::Return : BreachAction::None;
        }
        // FENCE_ACTION: 0 report, 1 RTL, 6 guided, 7 guided with throttle pass-through, 8 autoland or RTL
        if ((action == 6) || (action == 7)) {
            return BreachAction::ReturnPoint;
        }
        return ((action == 1) || (action == 8)) ? BreachAction::Return : BreachAction::None;
    case Autopilot::Unknown:
        break;
    }
    return BreachAction::None;
}

int GeoZoneReturnModel::arduPilotRallyPoint(const Parameters &parameters, const Destinations &destinations, const QGeoCoordinate &from)
{
    int nearest = -1;
    double nearestDistance = infinity;
    for (qsizetype i = 0; i < destinations.rallyPoints.count(); i++) {
        const QGeoCoordinate &coordinate = destinations.rallyPoints[i].coordinate;
        if (coordinate.isValid() && (from.distanceTo(coordinate) < nearestDistance)) {
            nearestDistance = from.distanceTo(coordinate);
            nearest = static_cast<int>(i);
        }
    }
    // Rally points beyond the limit are left over from other places
    if ((nearest < 0) || ((parameters.rallyLimit > 0) && (nearestDistance > parameters.rallyLimit))) {
        return -1;
    }
    const QGeoCoordinate &home = destinations.home.coordinate;
    if (!parameters.rallyIncludesHome || !home.isValid() || (nearestDistance < from.distanceTo(home))) {
        return nearest;
    }
    return -1;
}

double GeoZoneReturnModel::px4ReturnAltitude(const Parameters &parameters, const GeoZone::Position &start, const GeoZone::Position &destination)
{
    const double altitude = start.altitudeAMSL;
    const double destinationAltitude = destination.altitudeAMSL;
    if (!parameters.altitudeKnown() || qIsNaN(altitude) || qIsNaN(destinationAltitude)) {
        return qQNaN();
    }
    const double maxReturnAltitude = destinationAltitude + parameters.returnAltitude;
    if ((parameters.coneHalfAngle <= 0) || !parameters.multicopter) {
        return (std::max)(altitude, maxReturnAltitude);
    }

    // Within RTL_MIN_DIST the vehicle only climbs into the cone, at least twice the acceptance radius above the destination
    const double distance = start.coordinate.distanceTo(destination.coordinate);
    double returnAltitude = maxReturnAltitude;
    if (distance <= parameters.acceptanceRadius) {
        returnAltitude = destinationAltitude + (2 * distance);
    } else {
        if (distance <= parameters.coneRadius) {
            const double halfAngle = qDegreesToRadians(std::clamp(parameters.coneHalfAngle, 1.0, 89.0));
            returnAltitude = (std::min)(destinationAltitude + (distance / std::tan(halfAngle)), returnAltitude);
        }
        returnAltitude = (std::max)(returnAltitude, destinationAltitude + (2 * parameters.acceptanceRadius));
    }
    // Never below the current altitude
    if (returnAltitude < altitude) {
        return altitude;
    }
    return (std::min)(returnAltitude, maxReturnAltitude);
}

double GeoZoneReturnModel::arduCopterReturnAltitude(const Parameters &parameters, double altitudeAboveHome, double destinationAboveHome, double distance)
{
    const double minimum = (std::max)(minCopterAltitude, altitudeAboveHome + (std::max)(0.0, parameters.climbMinimum));
    double target = (std::max)((std::max)(destinationAboveHome, 0.0), (std::max)(parameters.returnAltitude, minimum));
    // Lower close to the destination
    if (parameters.coneSlope >= minConeSlope) {
        target = (std::min)(target, (std::max)(distance * parameters.coneSlope, minimum));
    }
    if (!qIsNaN(parameters.returnAltitudeLimit) && (target > parameters.returnAltitudeLimit)) {
        target = parameters.returnAltitudeLimit;
    }
    // Never descending
    return (std::max)(target, altitudeAboveHome);
}

GeoZoneReturnModel::Path GeoZoneReturnModel::returnPath(const Parameters &parameters, const Destinations &destinations, const Start &start)
{
    if (!start.position.coordinate.isValid()) {
        return Path();
    }
    switch (parameters.autopilot) {
    case Autopilot::PX4:
        return px4Return(parameters, destinations, start);
    case Autopilot::ArduCopter:
        return arduCopterReturn(parameters, destinations, start);
    case Autopilot::ArduPlane:
        return arduPlaneReturn(parameters, destinations, start);
    case Autopilot::Unknown:
        break;
    }
    return unknownReturn(parameters, destinations, start);
}

GeoZoneReturnModel::Path GeoZoneReturnModel::breachPath(const Parameters &parameters, const Destinations &destinations, const Start &start)
{
    Path path;
    const Position &home = destinations.home;
    if ((parameters.breachAction != BreachAction::ReturnPoint) || !start.position.coordinate.isValid() || !home.coordinate.isValid()) {
        return path;
    }
    const QGeoCoordinate &from = start.position.coordinate;
    const double altitude = start.position.altitudeAMSL;
    // The return altitude above home, negative for the current altitude
    const auto returnAltitude = [&parameters, &home, altitude]() {
        if (!parameters.altitudeKnown()) {
            return qQNaN();
        }
        return (parameters.returnAltitude < 0) ? altitude : (home.altitudeAMSL + parameters.returnAltitude);
    };

    Position destination;
    double target = qQNaN();
    if (parameters.breachReturnToRally) {
        // The closest rally point or home, like a return
        const int rallyIndex = arduPilotRallyPoint(parameters, destinations, from);
        destination = (rallyIndex >= 0) ? destinations.rallyPoints[rallyIndex] : home;
        path.destination = (rallyIndex >= 0) ? Destination::RallyPoint : Destination::Home;
        path.destinationIndex = rallyIndex;
        target = (rallyIndex >= 0) ? destination.altitudeAMSL : returnAltitude();
    } else {
        // The breach return point, home without, at the fence's return altitude, the middle of its altitudes, or the return altitude
        const bool hasReturnPoint = destinations.breachReturnPoint.coordinate.isValid();
        destination = hasReturnPoint ? destinations.breachReturnPoint : home;
        path.destination = hasReturnPoint ? Destination::BreachReturnPoint : Destination::Home;
        if (parameters.breachReturnAltitude > 0) {
            target = home.altitudeAMSL + parameters.breachReturnAltitude;
        } else if (parameters.fenceAltitudeMin < parameters.fenceAltitudeMax) {
            target = home.altitudeAMSL + ((parameters.fenceAltitudeMin + parameters.fenceAltitudeMax) / 2);
        } else {
            target = returnAltitude();
        }
    }
    appendPlaneLeg(path.positions, start, destination.coordinate, target);
    path.altitudeKnown = !qIsNaN(target) && !qIsNaN(altitude);
    if (parameters.loiterRadius > 0) {
        path.loiter = circle(destination.coordinate, parameters.loiterRadius, path.altitudeKnown ? target : qQNaN(), destination.terrainAMSL);
    }
    return path;
}

QList<GeoZone::Position> GeoZoneReturnModel::circle(const QGeoCoordinate &center, double radius, double altitudeAMSL, double terrainAMSL)
{
    QList<GeoZone::Position> positions;
    positions.reserve(circleVertexCount + 1);
    for (int i = 0; i <= circleVertexCount; i++) {
        positions.append(GeoZone::Position{ center.atDistanceAndAzimuth(radius, (360.0 * (i % circleVertexCount)) / circleVertexCount), altitudeAMSL, terrainAMSL });
    }
    return positions;
}
