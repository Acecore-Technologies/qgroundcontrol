/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneGuidedCheck.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionPath.h"
#include "GeoZonePlanning.h"
#include "GeoZoneMonitor.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZoneTerrain.h"
#include "LandingComplexItem.h"
#include "MissionController.h"
#include "QmlObjectListModel.h"
#include "RallyPointManager.h"
#include "SimpleMissionItem.h"
#include "Vehicle.h"
#include "VisualMissionItem.h"

#include <algorithm>
#include <cmath>

namespace {

/// Meters positions along the mission may be before the distance of an item, like the position of the item itself
constexpr double kDistanceTolerance = 0.01;

GeoZone::Position positionOf(const QGeoCoordinate &coordinate, double altitudeAMSL)
{
    return GeoZone::Position{ coordinate.isValid() ? QGeoCoordinate(coordinate.latitude(), coordinate.longitude()) : QGeoCoordinate(), altitudeAMSL };
}

GeoZonePathEvaluator::Vertex vertexOf(const GeoZone::Position &position)
{
    return GeoZonePathEvaluator::Vertex{ position.coordinate, position.altitudeAMSL, position.terrainAMSL, QDateTime() };
}

/// @return Ground elevation below the vehicle at the position, the home altitude where the terrain is unknown
double groundBelow(Vehicle *vehicle, const GeoZone::Position &position)
{
    if (!std::isnan(position.terrainAMSL)) {
        return position.terrainAMSL;
    }
    const QGeoCoordinate home = vehicle->homePosition();
    return home.isValid() ? home.altitude() : qQNaN();
}

} // namespace

GeoZoneGuidedCheck::GeoZoneGuidedCheck(GeoZoneMonitor *monitor)
    : QObject(monitor)
    , _monitor(monitor)
{
}

QString GeoZoneGuidedCheck::pathWarning(const QList<GeoZone::Position> &path) const
{
    return pathWarning(GeoZonePathEvaluator::Path::fromPositions(path));
}

QString GeoZoneGuidedCheck::pathWarning(const GeoZonePathEvaluator::Path &path) const
{
    return pathsWarning({ path });
}

QString GeoZoneGuidedCheck::pathsWarning(const QList<GeoZonePathEvaluator::Path> &paths, const QString &subject) const
{
    if (paths.isEmpty()) {
        return QString();
    }
    GeoZoneManager *const manager = _monitor->manager();
    GeoZonePathEvaluator evaluator(manager->catalog(), GeoZonePolicy::alerts().excludingAuthorised(manager->planning()->authorisations()),
                                   GeoZoneEnvironment::currentDateTimeUtc());
    evaluator.setTerrainLookup(GeoZoneTerrain::elevationAt);
    const auto atVehicle = [](const GeoZonePathEvaluator::Crossing &crossing) {
        return (crossing.entry.path == 0) && (crossing.entry.segment == 0) && (crossing.entry.fraction <= 0);
    };
    QList<const GeoZone*> zones;
    for (const GeoZonePathEvaluator::ZoneResult &result : evaluator.evaluate(paths).zones) {
        // Zones the vehicle is already in were alerted when it entered them, it may enter them again after leaving them
        const bool inside = std::any_of(result.crossings.cbegin(), result.crossings.cend(), atVehicle);
        const bool enters = std::any_of(result.crossings.cbegin(), result.crossings.cend(), [&atVehicle, inside](const GeoZonePathEvaluator::Crossing &crossing) {
            return crossing.active && !atVehicle(crossing) && (!inside || (crossing.entry.path == 0));
        });
        if (enters) {
            zones.append(result.zone);
        }
    }
    if (zones.isEmpty()) {
        return QString();
    }
    GeoZonePathEvaluator::sortBySeverity(zones);
    const QString names = GeoZonePathEvaluator::zoneNames(zones, maxListedZones, true);
    return subject.isEmpty() ? tr("The vehicle would enter geozones: %1").arg(names) : tr("%1 would enter geozones: %2").arg(subject, names);
}

QString GeoZoneGuidedCheck::gotoWarning(QObject *vehicle, const QGeoCoordinate &coordinate, double altitudeAMSL) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid() || !coordinate.isValid()) {
        return QString();
    }
    const GeoZone::Position from = _monitor->vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    const GeoZone::Position to{ coordinate, std::isnan(altitudeAMSL) ? from.altitudeAMSL : altitudeAMSL };
    return pathWarning({ from, to });
}

QString GeoZoneGuidedCheck::orbitWarning(QObject *vehicle, const QGeoCoordinate &center, double radius, double altitudeAMSL) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid() || !center.isValid() || !(radius > 0)) {
        return QString();
    }
    const GeoZone::Position from = _monitor->vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    QList<GeoZone::Position> path = { from };
    constexpr int circlePoints = 36;
    const double startAzimuth = center.azimuthTo(from.coordinate);
    for (int i = 0; i <= circlePoints; i++) {
        path.append(GeoZone::Position{ center.atDistanceAndAzimuth(radius, startAzimuth + (360.0 * i / circlePoints)),
                                       std::isnan(altitudeAMSL) ? from.altitudeAMSL : altitudeAMSL });
    }
    return pathWarning(path);
}

QString GeoZoneGuidedCheck::altitudeWarning(QObject *vehicle, double relativeAltitude) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid() || std::isnan(relativeAltitude)) {
        return QString();
    }
    const GeoZone::Position from = _monitor->vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    const double currentRelative = guidedVehicle->altitudeRelative()->rawValue().toDouble();
    if (std::isnan(from.altitudeAMSL) || std::isnan(currentRelative)) {
        // Unknown altitudes are inside all limits, so only zones applying at any altitude would be named
        return pathWarning({ from, from });
    }
    // Climbing or descending in place relative to the home altitude
    GeoZonePathEvaluator::Path path;
    path.altitudeReference = GeoZonePathEvaluator::AltitudeReference::Relative;
    path.referenceAltitudeAMSL = from.altitudeAMSL - currentRelative;
    path.vertices = { GeoZonePathEvaluator::Vertex{ from.coordinate, currentRelative, from.terrainAMSL, QDateTime() },
                      GeoZonePathEvaluator::Vertex{ from.coordinate, relativeAltitude, from.terrainAMSL, QDateTime() } };
    return pathWarning(path);
}

QString GeoZoneGuidedCheck::returnWarning(QObject *vehicle, QObject *missionController) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid()) {
        return QString();
    }
    MissionController *const mission = qobject_cast<MissionController*>(missionController);
    const GeoZoneReturnModel::Parameters parameters = GeoZoneMissionPath::returnParameters(guidedVehicle);
    const GeoZoneReturnModel::Destinations destinations = returnDestinations(guidedVehicle, mission);
    // PX4 returns along the mission from the item flown to, which also chooses the direction while flying the mission
    const int currentItem = mission ? mission->currentMissionIndex() : -1;
    GeoZoneReturnModel::Start start;
    start.position = _monitor->vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    start.sequenceNumber = (guidedVehicle->flightMode() == guidedVehicle->missionFlightMode()) ? currentItem : -1;
    const GeoZoneReturnModel::Path path = GeoZoneReturnModel::returnPath(parameters, destinations, start);
    const QList<GeoZonePathEvaluator::Path> paths = returnPaths(path, start, destinations, GeoZoneMissionPath::fromMission(mission, parameters), currentItem,
                                                                groundBelow(guidedVehicle, start.position));
    // Like "The return to rally point 2 assuming the PX4 default return altitude of 30 m"
    QString subject = GeoZoneMissionPath::returnDescription(path, parameters, true);
    if (!subject.isEmpty()) {
        subject[0] = subject[0].toUpper();
    }
    return pathsWarning(paths, subject);
}

QString GeoZoneGuidedCheck::landWarning(QObject *vehicle) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid()) {
        return QString();
    }
    const GeoZone::Position from = _monitor->vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    return pathsWarning({ landingPath(from, groundBelow(guidedVehicle, from)) }, tr("The landing"));
}

QString GeoZoneGuidedCheck::missionWarning(QObject *vehicle, QObject *missionController, int sequenceNumber, bool fromGround) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    MissionController *const mission = qobject_cast<MissionController*>(missionController);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid() || !mission) {
        return QString();
    }
    QList<GeoZonePathEvaluator::Path> paths = missionPaths(GeoZoneMissionPath::fromMission(mission, GeoZoneMissionPath::returnParameters(guidedVehicle)),
                                                           sequenceNumber);
    if (paths.isEmpty()) {
        return QString();
    }
    // From the vehicle to the item, climbing to its altitude first when taking off
    const GeoZone::Position from = _monitor->vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    QList<GeoZonePathEvaluator::Vertex> &vertices = paths.first().vertices;
    QList<GeoZonePathEvaluator::Vertex> leg = { vertexOf(from) };
    if (fromGround && !vertices.isEmpty()) {
        leg.append(GeoZonePathEvaluator::Vertex{ from.coordinate, vertices.constFirst().altitude, from.terrainAMSL, QDateTime() });
    }
    vertices = leg + vertices;
    return pathsWarning(paths, tr("The mission from item %1").arg(sequenceNumber));
}

GeoZoneReturnModel::Destinations GeoZoneGuidedCheck::returnDestinations(Vehicle *vehicle, MissionController *missionController)
{
    GeoZoneReturnModel::Destinations destinations;
    QGeoCoordinate home = vehicle ? vehicle->homePosition() : QGeoCoordinate();
    if (!home.isValid() && missionController) {
        home = missionController->plannedHomePosition();
    }
    const double homeAltitude = home.isValid() ? home.altitude() : qQNaN();
    if (home.isValid()) {
        destinations.home = positionOf(home, homeAltitude);
    }
    // Rally points are above home
    if (vehicle && vehicle->rallyPointManager()) {
        for (const QGeoCoordinate &rallyPoint : vehicle->rallyPointManager()->points()) {
            if (rallyPoint.isValid()) {
                destinations.rallyPoints.append(positionOf(rallyPoint, homeAltitude + rallyPoint.altitude()));
            }
        }
    }
    if (!missionController) {
        return destinations;
    }

    // The first waypoint, the landing sequences from their DO_LAND_START and the return path after a
    // DO_RETURN_PATH_START, like GeoZoneMissionPath
    const QmlObjectListModel *const visualItems = missionController->visualItems();
    const auto isCoordinateItem = [](const VisualMissionItem *item) { return item && item->specifiesCoordinate() && !item->isStandaloneCoordinate(); };
    bool onReturnPath = false;
    for (int i = 1; i < visualItems->count(); i++) {
        const VisualMissionItem *const visualItem = visualItems->value<const VisualMissionItem*>(i);
        if (!destinations.firstWaypoint.coordinate.isValid() && isCoordinateItem(visualItem)) {
            destinations.firstWaypoint = positionOf(visualItem->coordinate(), visualItem->amslEntryAlt());
        }
        if (const LandingComplexItem *const landingItem = qobject_cast<const LandingComplexItem*>(visualItem)) {
            GeoZoneReturnModel::Landing landing;
            landing.entry = positionOf(landingItem->finalApproachCoordinate(), landingItem->amslEntryAlt());
            landing.touchdown = positionOf(landingItem->landingCoordinate(), landingItem->amslExitAlt());
            landing.marker = landing.entry.coordinate;
            landing.sequenceNumber = landingItem->sequenceNumber();
            if (landing.entry.coordinate.isValid() && landing.touchdown.coordinate.isValid()) {
                destinations.landings.append(landing);
            }
            onReturnPath = false;
            continue;
        }
        const SimpleMissionItem *const simpleItem = qobject_cast<const SimpleMissionItem*>(visualItem);
        if (!simpleItem) {
            continue;
        }
        if (simpleItem->command() == MAV_CMD_DO_LAND_START) {
            GeoZoneReturnModel::Landing landing;
            landing.sequenceNumber = simpleItem->sequenceNumber();
            const QGeoCoordinate marker = simpleItem->coordinate();
            if (simpleItem->specifiesCoordinate() && marker.isValid() && ((marker.latitude() != 0) || (marker.longitude() != 0))) {
                landing.marker = positionOf(marker, qQNaN()).coordinate;
            }
            for (int j = i + 1; j < visualItems->count(); j++) {
                const VisualMissionItem *const item = visualItems->value<const VisualMissionItem*>(j);
                if (!isCoordinateItem(item)) {
                    continue;
                }
                if (!landing.entry.coordinate.isValid()) {
                    landing.entry = positionOf(item->coordinate(), item->amslEntryAlt());
                }
                if (item->isLandCommand()) {
                    landing.touchdown = positionOf(item->exitCoordinate(), item->amslExitAlt());
                    break;
                }
            }
            if (landing.entry.coordinate.isValid() && landing.touchdown.coordinate.isValid()) {
                if (!landing.marker.isValid()) {
                    landing.marker = landing.entry.coordinate;
                }
                destinations.landings.append(landing);
            }
            onReturnPath = false;
        } else if (simpleItem->command() == MAV_CMD_DO_RETURN_PATH_START) {
            onReturnPath = true;
        } else if (onReturnPath && isCoordinateItem(simpleItem)) {
            if (simpleItem->isLandCommand()) {
                onReturnPath = false;
            } else {
                destinations.returnPath.append(positionOf(simpleItem->coordinate(), simpleItem->amslEntryAlt()));
            }
        }
    }
    return destinations;
}

QList<GeoZonePathEvaluator::Path> GeoZoneGuidedCheck::returnPaths(const GeoZoneReturnModel::Path &path, const GeoZoneReturnModel::Start &start,
                                                                  const GeoZoneReturnModel::Destinations &destinations, const GeoZoneMissionPath &mission,
                                                                  int sequenceNumber, double groundAMSL)
{
    using Destination = GeoZoneReturnModel::Destination;
    QList<GeoZonePathEvaluator::Path> paths;
    switch (path.destination) {
    case Destination::MissionForward:
        // On along the mission to its landing
        paths = missionPaths(mission, sequenceNumber);
        if (!paths.isEmpty()) {
            paths.first().vertices.prepend(vertexOf(start.position));
        }
        return paths;
    case Destination::ReverseMission: {
        // Back along the mission from the item before the one flown to, to the first waypoint, then home
        QList<GeoZone::Position> positions = { start.position };
        const int itemIndex = _itemIndex(mission, sequenceNumber);
        const QList<GeoZoneMissionPath::Leg> &legs = mission.legs();
        const auto leg = std::find_if(legs.cbegin(), legs.cend(), [itemIndex](const GeoZoneMissionPath::Leg &leg) { return leg.toItemIndex == itemIndex; });
        if ((itemIndex >= 0) && (leg != legs.cend())) {
            QList<GeoZone::Position> back = _flightPathPositions(mission, mission.itemFlightDistance(legs.constFirst().toItemIndex),
                                                                 mission.itemFlightDistance(leg->fromItemIndex));
            std::reverse(back.begin(), back.end());
            positions.append(back);
        }
        positions.append(path.positions);
        paths.append(GeoZonePathEvaluator::Path::fromPositions(positions));
        break;
    }
    case Destination::None:
        // Landing where it is
        if (path.altitudeKnown) {
            paths.append(landingPath(start.position, groundAMSL));
        }
        return paths;
    default:
        if (!path.isValid()) {
            return paths;
        }
        paths.append(GeoZonePathEvaluator::Path::fromPositions(path.positions));
        break;
    }
    if (!path.loiter.isEmpty()) {
        paths.append(GeoZonePathEvaluator::Path::fromPositions(path.loiter));
    }
    if ((path.destination == Destination::MissionLanding) && (path.destinationIndex >= 0) && (path.destinationIndex < destinations.landings.count())) {
        // Then the landing sequence of the mission, from the item after its DO_LAND_START
        for (const GeoZonePathEvaluator::Path &landingSequence : missionPaths(mission, destinations.landings[path.destinationIndex].sequenceNumber + 1)) {
            if (!landingSequence.vertices.isEmpty()) {
                paths.append(landingSequence);
            }
        }
    }
    return paths;
}

QList<GeoZonePathEvaluator::Path> GeoZoneGuidedCheck::missionPaths(const GeoZoneMissionPath &mission, int sequenceNumber)
{
    const int itemIndex = _itemIndex(mission, sequenceNumber);
    const double from = (itemIndex >= 0) ? (mission.itemFlightDistance(itemIndex) - kDistanceTolerance) : qQNaN();
    if (std::isnan(from)) {
        return {};
    }
    // The flight path first, then the paths besides it like loiters and the return after the mission
    QList<GeoZonePathEvaluator::Path> flightPaths;
    QList<GeoZonePathEvaluator::Path> otherPaths;
    for (const GeoZoneMissionPath::Path &path : mission.paths()) {
        QList<GeoZone::Position> positions;
        for (qsizetype i = 0; i < path.positions.count(); i++) {
            if (path.distances.value(i, qQNaN()) >= from) {
                positions.append(path.positions[i]);
            }
        }
        if (!positions.isEmpty()) {
            (path.label.isEmpty() ? flightPaths : otherPaths).append(GeoZonePathEvaluator::Path::fromPositions(positions, path.label));
        }
    }
    if (flightPaths.isEmpty()) {
        flightPaths.append(GeoZonePathEvaluator::Path());
    }
    return flightPaths + otherPaths;
}

GeoZonePathEvaluator::Path GeoZoneGuidedCheck::landingPath(const GeoZone::Position &from, double groundAMSL)
{
    // Unknown altitudes are inside all limits, so without them only zones applying at any altitude would be named
    GeoZone::Position to = from;
    if (!std::isnan(from.altitudeAMSL) && !std::isnan(groundAMSL)) {
        to.altitudeAMSL = (std::min)(from.altitudeAMSL, groundAMSL);
    }
    return GeoZonePathEvaluator::Path::fromPositions({ from, to });
}

int GeoZoneGuidedCheck::_itemIndex(const GeoZoneMissionPath &mission, int sequenceNumber)
{
    // The legs join the items with a coordinate in mission order
    int itemIndex = -1;
    for (const GeoZoneMissionPath::Leg &leg : mission.legs()) {
        if (leg.toSequenceNumber > sequenceNumber) {
            break;
        }
        itemIndex = leg.toItemIndex;
    }
    return itemIndex;
}

QList<GeoZone::Position> GeoZoneGuidedCheck::_flightPathPositions(const GeoZoneMissionPath &mission, double from, double to)
{
    QList<GeoZone::Position> positions;
    if (std::isnan(from) || std::isnan(to)) {
        return positions;
    }
    for (const GeoZoneMissionPath::Path &path : mission.paths()) {
        if (!path.label.isEmpty()) {
            continue;
        }
        for (qsizetype i = 0; i < path.positions.count(); i++) {
            const double distance = path.distances.value(i, qQNaN());
            if ((distance >= (from - kDistanceTolerance)) && (distance <= (to + kDistanceTolerance))) {
                positions.append(path.positions[i]);
            }
        }
    }
    return positions;
}
