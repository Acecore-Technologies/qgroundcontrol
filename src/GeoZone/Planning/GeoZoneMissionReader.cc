/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMissionReader.h"
#include "ComplexMissionItem.h"
#include "FlightPathSegment.h"
#include "GeoFenceController.h"
#include "GeoZoneBox.h"
#include "GeoZoneReturnDestinations.h"
#include "GeoZoneTerrain.h"
#include "LandingComplexItem.h"
#include "MissionController.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "PlanViewSettings.h"
#include "QGCMapPolygon.h"
#include "QmlObjectListModel.h"
#include "RallyPoint.h"
#include "RallyPointController.h"
#include "SettingsManager.h"
#include "SimpleMissionItem.h"
#include "StructureScanComplexItem.h"
#include "TransectStyleComplexItem.h"
#include "Vehicle.h"
#include "VisualMissionItem.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

GeoZoneMissionPath::Segment segmentFrom(const FlightPathSegment *flightPathSegment, int itemIndex)
{
    GeoZoneMissionPath::Segment segment;
    segment.from = flightPathSegment->coordinate1();
    segment.to = flightPathSegment->coordinate2();
    segment.fromAltitudeAMSL = flightPathSegment->coord1AMSLAlt();
    segment.toAltitudeAMSL = flightPathSegment->coord2AMSLAlt();
    for (const QVariant &height : flightPathSegment->amslTerrainHeights()) {
        segment.terrainAMSL.append(height.toDouble());
    }
    segment.terrainSpacing = flightPathSegment->distanceBetween();
    segment.distance = flightPathSegment->totalDistance();
    segment.itemIndex = itemIndex;
    return segment;
}

/// Survey, corridor and structure scan area of the item, with the range of its altitudes
std::optional<GeoZone::Area> areaOf(ComplexMissionItem *complexItem)
{
    QList<QGeoCoordinate> polygon;
    if (TransectStyleComplexItem *const transectItem = qobject_cast<TransectStyleComplexItem*>(complexItem)) {
        polygon = transectItem->surveyAreaPolygon()->coordinateList();
    } else if (StructureScanComplexItem *const structureItem = qobject_cast<StructureScanComplexItem*>(complexItem)) {
        polygon = structureItem->flightPolygon()->coordinateList();
    }
    if (polygon.count() < 3) {
        return std::nullopt;
    }

    // Terrain along the item's flight path bounds the height above ground, it stays unknown without samples
    double minTerrain = qQNaN();
    double maxTerrain = qQNaN();
    const QmlObjectListModel *const segments = complexItem->flightPathSegments();
    for (int j = 0; j < segments->count(); j++) {
        for (const QVariant &height : segments->value<const FlightPathSegment*>(j)->amslTerrainHeights()) {
            const double terrain = height.toDouble();
            minTerrain = qIsNaN(minTerrain) ? terrain : std::min(minTerrain, terrain);
            maxTerrain = qIsNaN(maxTerrain) ? terrain : std::max(maxTerrain, terrain);
        }
    }

    const QGeoCoordinate center = GeoZoneBox::fromPath(polygon).center();
    return GeoZone::Area{ polygon,
                          GeoZone::Position{ center, complexItem->minAMSLAltitude(), maxTerrain },
                          GeoZone::Position{ center, complexItem->maxAMSLAltitude(), minTerrain } };
}

} // namespace

GeoZoneMissionPath GeoZoneMissionReader::read(MissionController *missionController, bool withContingencies)
{
    PlanMasterController *const masterController = missionController ? missionController->masterController() : nullptr;
    return read(missionController, returnParameters(masterController ? masterController->managerVehicle() : nullptr), withContingencies);
}

GeoZoneMissionPath GeoZoneMissionReader::read(MissionController *missionController, const GeoZoneReturnModel::Parameters &returnParameters, bool withContingencies)
{
    GeoZoneMissionPath path;
    const QmlObjectListModel *const visualItems = missionController ? missionController->visualItems() : nullptr;
    if (!visualItems) {
        return path;
    }

    // The mission controller draws a segment home from the last item with a coordinate before a return to launch
    int returnItem = -1;
    for (int i = 0; (i < visualItems->count()) && (returnItem < 0); i++) {
        const SimpleMissionItem *const simpleItem = visualItems->value<const SimpleMissionItem*>(i);
        if (simpleItem && (simpleItem->command() == MAV_CMD_NAV_RETURN_TO_LAUNCH)) {
            returnItem = i;
        }
    }

    // Same order and distances as TerrainProfile: the segments of a pattern, then the segment to the next item
    QList<int> coordinateItems;
    QList<GeoZoneMissionJumps::ItemSegments> itemSegments(visualItems->count());
    double distance = 0;
    const auto appendSegment = [&path, &distance](const GeoZoneMissionPath::Segment &segment) {
        path._segments.append(segment);
        distance += segment.distance;
        return static_cast<int>(path._segments.count() - 1);
    };
    for (int i = 0; i < visualItems->count(); i++) {
        VisualMissionItem *const visualItem = visualItems->value<VisualMissionItem*>(i);
        if (!visualItem) {
            continue;
        }
        path._itemDistances.insert(i, distance);
        path._sequenceNumbers.insert(i, visualItem->sequenceNumber());
        if (ComplexMissionItem *const complexItem = qobject_cast<ComplexMissionItem*>(visualItem)) {
            const QmlObjectListModel *const complexSegments = complexItem->flightPathSegments();
            if (complexSegments->count() == 0) {
                GeoZoneMissionPath::Segment gap;
                gap.distance = complexItem->complexDistance();
                gap.itemIndex = i;
                itemSegments[i].segments.append(appendSegment(gap));
            }
            for (int j = 0; j < complexSegments->count(); j++) {
                itemSegments[i].segments.append(appendSegment(segmentFrom(complexSegments->value<const FlightPathSegment*>(j), i)));
            }
            if (const std::optional<GeoZone::Area> area = areaOf(complexItem)) {
                path._areas.append(*area);
            }
        }
        if (visualItem->simpleFlightPathSegment()) {
            itemSegments[i].outgoing = appendSegment(segmentFrom(visualItem->simpleFlightPathSegment(), i));
        }
        if (visualItem->specifiesCoordinate() && !visualItem->isStandaloneCoordinate()) {
            coordinateItems.append(i);
        }
    }
    const QGeoCoordinate home = missionController->plannedHomePosition();
    for (qsizetype k = coordinateItems.count() - 1; (k >= 0) && (returnItem >= 0); k--) {
        if (coordinateItems[k] < returnItem) {
            const int returnSegment = itemSegments[coordinateItems[k]].outgoing;
            if ((returnSegment >= 0) && home.isValid() && (path._segments[returnSegment].to.distanceTo(home) < 1)) {
                path._returnSegment = returnSegment;
            }
            break;
        }
    }

    for (qsizetype k = 1; k < coordinateItems.count(); k++) {
        VisualMissionItem *const fromItem = visualItems->value<VisualMissionItem*>(coordinateItems[k - 1]);
        const VisualMissionItem *const toItem = visualItems->value<const VisualMissionItem*>(coordinateItems[k]);
        GeoZoneMissionPath::Leg leg;
        leg.fromItemIndex = coordinateItems[k - 1];
        leg.toItemIndex = coordinateItems[k];
        leg.toSequenceNumber = toItem->sequenceNumber();
        leg.segment.from = fromItem->exitCoordinate();
        leg.segment.to = toItem->coordinate();
        leg.segment.itemIndex = leg.fromItemIndex;
        // Altitudes and terrain of the leg from its flight path segment
        const FlightPathSegment *const flightPathSegment = fromItem->simpleFlightPathSegment();
        if (flightPathSegment && leg.segment.to.isValid() && (flightPathSegment->coordinate2().distanceTo(leg.segment.to) < 1)) {
            const GeoZoneMissionPath::Segment known = segmentFrom(flightPathSegment, leg.fromItemIndex);
            leg.segment.fromAltitudeAMSL = known.fromAltitudeAMSL;
            leg.segment.toAltitudeAMSL = known.toAltitudeAMSL;
            leg.segment.terrainAMSL = known.terrainAMSL;
            leg.segment.terrainSpacing = known.terrainSpacing;
            leg.segment.distance = known.distance;
        }
        path._legs.append(leg);
    }

    const QList<GeoZoneMissionItem> missionItems = items(missionController);
    const auto [flownReturnItem, returnFromItem] = _setFlightPaths(path, missionItems, itemSegments);
    const GeoZoneReturnModel::Destinations returnDestinations = destinations(missionController, missionItems);
    _appendCoverage(path, missionController, coordinateItems, returnParameters, returnDestinations, flownReturnItem, returnFromItem);
    if (withContingencies) {
        // The sequence numbers the segments lead to
        QHash<int, int> outgoingSequenceNumbers;
        for (qsizetype k = 1; k < coordinateItems.count(); k++) {
            const int outgoing = itemSegments[coordinateItems[k - 1]].outgoing;
            if (outgoing >= 0) {
                outgoingSequenceNumbers.insert(outgoing, path._sequenceNumbers.value(coordinateItems[k], -1));
            }
        }
        path._appendContingencies(outgoingSequenceNumbers, returnParameters, returnDestinations);
    }
    return path;
}


QList<GeoZoneMissionItem> GeoZoneMissionReader::items(MissionController *missionController)
{
    using Kind = GeoZoneMissionItem::Kind;
    QList<GeoZoneMissionItem> items;
    QmlObjectListModel *const visualItems = missionController ? missionController->visualItems() : nullptr;
    if (!visualItems) {
        return items;
    }
    items.reserve(visualItems->count());
    for (int i = 0; i < visualItems->count(); i++) {
        GeoZoneMissionItem item;
        VisualMissionItem *const visualItem = visualItems->value<VisualMissionItem*>(i);
        if (!visualItem) {
            items.append(item);
            continue;
        }
        item.sequenceNumber = visualItem->sequenceNumber();
        item.lastSequenceNumber = visualItem->lastSequenceNumber();
        item.flies = visualItem->specifiesCoordinate() && !visualItem->isStandaloneCoordinate();
        item.land = visualItem->isLandCommand();
        item.entry = GeoZone::Position{ visualItem->coordinate(), visualItem->amslEntryAlt() };
        item.exit = GeoZone::Position{ visualItem->exitCoordinate(), visualItem->amslExitAlt() };
        if (qobject_cast<const LandingComplexItem*>(visualItem)) {
            item.kind = Kind::LandingPattern;
        } else if (qobject_cast<const ComplexMissionItem*>(visualItem)) {
            item.kind = Kind::Pattern;
        } else if (const SimpleMissionItem *const simpleItem = qobject_cast<const SimpleMissionItem*>(visualItem)) {
            switch (simpleItem->command()) {
            case MAV_CMD_NAV_TAKEOFF:
            case MAV_CMD_NAV_VTOL_TAKEOFF:
                item.kind = Kind::Takeoff;
                break;
            case MAV_CMD_NAV_RETURN_TO_LAUNCH:
                item.kind = Kind::ReturnToLaunch;
                break;
            case MAV_CMD_DO_JUMP:
                item.kind = Kind::Jump;
                item.jumpSequence = qRound(simpleItem->missionItem().param1());
                item.jumpRepeat = qRound(simpleItem->missionItem().param2());
                break;
            case MAV_CMD_DO_LAND_START:
                item.kind = Kind::LandStart;
                if (simpleItem->specifiesCoordinate()) {
                    item.marker = simpleItem->coordinate();
                }
                break;
            case MAV_CMD_DO_RETURN_PATH_START:
                item.kind = Kind::ReturnPathStart;
                break;
            default:
                break;
            }
        }
        items.append(item);
    }
    return items;
}

GeoZoneReturnModel::Destinations GeoZoneMissionReader::destinations(MissionController *missionController, const QList<GeoZoneMissionItem> &items)
{
    const QGeoCoordinate homeCoordinate = missionController->plannedHomePosition();
    const double homeAltitude = homeCoordinate.isValid() ? homeCoordinate.altitude() : qQNaN();
    const GeoZone::Position home{ homeCoordinate, homeAltitude };

    // Rally points and the breach return point are above home
    QList<GeoZone::Position> rallyPoints;
    GeoZone::Position breachReturnPoint;
    if (PlanMasterController *const masterController = missionController->masterController()) {
        const QmlObjectListModel *const points = masterController->rallyPointController()->points();
        for (int i = 0; i < points->count(); i++) {
            if (const RallyPoint *const rallyPoint = points->value<const RallyPoint*>(i)) {
                const QGeoCoordinate coordinate = rallyPoint->coordinate();
                rallyPoints.append(GeoZone::Position{ coordinate, homeAltitude + coordinate.altitude() });
            }
        }
        GeoFenceController *const geoFenceController = masterController->geoFenceController();
        breachReturnPoint = GeoZone::Position{ geoFenceController->breachReturnPoint(),
                                               homeAltitude + geoFenceController->breachReturnAltitude()->rawValue().toDouble() };
    }
    return GeoZoneReturnDestinations::fromMission(items.mid(1), home, rallyPoints, breachReturnPoint);
}

std::pair<int, int> GeoZoneMissionReader::_setFlightPaths(GeoZoneMissionPath &path, const QList<GeoZoneMissionItem> &items,
                                                          const QList<GeoZoneMissionJumps::ItemSegments> &itemSegments)
{
    if (!GeoZoneMissionJumps::hasJumps(items)) {
        path._paths = GeoZoneMissionPath::pathsFromSegments(path._segments);
        path._itemFlightDistances = path._itemDistances;
        path._flightDistance = path.totalDistance();
        // The mission ends at the first return to launch
        int returnFromItem = -1;
        for (int i = 0; i < items.count(); i++) {
            if (items[i].kind == GeoZoneMissionItem::Kind::ReturnToLaunch) {
                return { i, returnFromItem };
            }
            if (items[i].flies) {
                returnFromItem = i;
            }
        }
        return { -1, -1 };
    }

    QList<double> segmentDistances;
    segmentDistances.reserve(path._segments.count());
    for (const GeoZoneMissionPath::Segment &segment : std::as_const(path._segments)) {
        segmentDistances.append(segment.distance);
    }
    const int maxSteps = static_cast<int>((std::max)(static_cast<qsizetype>(GeoZoneMissionPath::maxFlightSegments), 2 * path._segments.count()));
    const GeoZoneMissionJumps::Walk walk = GeoZoneMissionJumps::walk(items, itemSegments, segmentDistances, path._returnSegment, maxSteps);
    QList<GeoZoneMissionPath::Segment> flown;
    flown.reserve(walk.steps.count());
    for (const GeoZoneMissionJumps::Step &step : walk.steps) {
        if (step.segment >= 0) {
            flown.append(path._segments[step.segment]);
            continue;
        }
        GeoZoneMissionPath::Segment segment;
        segment.from = items[step.fromItem].exit.coordinate;
        segment.to = items[step.toItem].entry.coordinate;
        segment.fromAltitudeAMSL = items[step.fromItem].exit.altitudeAMSL;
        segment.toAltitudeAMSL = items[step.toItem].entry.altitudeAMSL;
        segment.itemIndex = step.fromItem;
        segment.label = step.label;
        segment.distance = step.distance;
        flown.append(segment);
    }
    path._itemFlightDistances = walk.itemDistances;
    // Items the vehicle doesn't reach keep their place on the drawn path
    for (auto it = path._itemDistances.cbegin(); it != path._itemDistances.cend(); ++it) {
        if (!path._itemFlightDistances.contains(it.key())) {
            path._itemFlightDistances.insert(it.key(), it.value());
        }
    }
    path._paths = GeoZoneMissionPath::pathsFromSegments(flown);
    path._flightDistance = walk.distance;
    path._repeatsIndefinitely = walk.repeatsIndefinitely;
    return { walk.returnItem, (walk.returnItem >= 0) ? walk.lastCoordinateItem : -1 };
}

void GeoZoneMissionReader::_appendCoverage(GeoZoneMissionPath &path, MissionController *missionController, const QList<int> &coordinateItems,
                                           const GeoZoneReturnModel::Parameters &returnParameters, const GeoZoneReturnModel::Destinations &destinations,
                                           int returnItem, int returnFromItem)
{
    const QmlObjectListModel *const visualItems = missionController->visualItems();

    // Terrain at a position of the flight path, from the segment ending or starting there
    const auto terrainAtPathPosition = [&path](const QGeoCoordinate &coordinate) {
        for (const GeoZoneMissionPath::Segment &segment : std::as_const(path._segments)) {
            if (segment.terrainAMSL.isEmpty()) {
                continue;
            }
            if (segment.to.isValid() && (segment.to.distanceTo(coordinate) < 1)) {
                return segment.terrainAMSL.last();
            }
            if (segment.from.isValid() && (segment.from.distanceTo(coordinate) < 1)) {
                return segment.terrainAMSL.first();
            }
        }
        return qQNaN();
    };
    // The terrain tiles give the terrain along the path where they are loaded, the nearest flight path position's elsewhere
    const auto appendPath = [&path, &terrainAtPathPosition](QList<GeoZone::Position> positions, int itemIndex, const QString &label, double distance) {
        if (positions.count() < 2) {
            return;
        }
        if (qIsNaN(positions.first().terrainAMSL)) {
            positions.first().terrainAMSL = terrainAtPathPosition(positions.first().coordinate);
        }
        const QList<GeoZone::Position> withTerrain = GeoZoneTerrain::withTerrain(positions);
        path._paths.append(GeoZoneMissionPath::Path{ withTerrain, itemIndex, label, QList<double>(withTerrain.count(), distance) });
    };
    const auto previousCoordinateItems = [&coordinateItems](int index) {
        QList<int> previous;
        for (const int item : coordinateItems) {
            if (item < index) {
                previous.append(item);
            }
        }
        return previous;
    };

    const double transitionDistance = SettingsManager::instance()->planViewSettings()->vtolTransitionDistance()->rawValue().toDouble();
    for (int i = 0; i < visualItems->count(); i++) {
        VisualMissionItem *const visualItem = visualItems->value<VisualMissionItem*>(i);
        if (SimpleMissionItem *const simpleItem = qobject_cast<SimpleMissionItem*>(visualItem)) {
            const int command = simpleItem->command();
            if (simpleItem->isLoiterItem() && simpleItem->coordinate().isValid()) {
                // Without a radius the vehicle's default applies, multicopters hover at the waypoint
                const double radius = std::abs(simpleItem->loiterRadius());
                if (radius > 0) {
                    appendPath(GeoZoneMissionPath::circle(simpleItem->coordinate(), radius, simpleItem->amslEntryAlt(), qQNaN()), i,
                               tr("the loiter at item %1").arg(simpleItem->sequenceNumber()), path.itemFlightDistance(i));
                }
            } else if (command == MAV_CMD_DO_VTOL_TRANSITION) {
                // Transitions take about the transition distance, flying on past the waypoints they follow or lead to
                const QList<int> previous = previousCoordinateItems(i);
                const auto next = std::find_if(coordinateItems.cbegin(), coordinateItems.cend(), [i](int item) { return item > i; });
                if (previous.isEmpty() || (transitionDistance <= 0)) {
                    continue;
                }
                const VisualMissionItem *const startItem = visualItems->value<const VisualMissionItem*>(previous.last());
                const QGeoCoordinate start = startItem->exitCoordinate();
                QGeoCoordinate overshootFrom;
                double azimuth = qQNaN();
                double overshoot = 0;
                const int state = qRound(simpleItem->missionItem().param1());
                if ((state == MAV_VTOL_STATE_MC) && (previous.count() >= 2)) {
                    // Slowing down to hover after the waypoint, along the leg to it
                    const QGeoCoordinate before = visualItems->value<const VisualMissionItem*>(previous[previous.count() - 2])->exitCoordinate();
                    if (before.isValid() && (before.distanceTo(start) > 1)) {
                        overshootFrom = start;
                        azimuth = before.azimuthTo(start);
                        overshoot = transitionDistance;
                    }
                } else if ((state == MAV_VTOL_STATE_FW) && (next != coordinateItems.cend())) {
                    // Speeding up towards the next waypoint, past it when it is closer than the transition distance
                    const QGeoCoordinate end = visualItems->value<const VisualMissionItem*>(*next)->coordinate();
                    if (end.isValid() && (start.distanceTo(end) > 1)) {
                        overshootFrom = end;
                        azimuth = start.azimuthTo(end);
                        overshoot = transitionDistance - start.distanceTo(end);
                    }
                }
                if (overshootFrom.isValid() && (overshoot > 0)) {
                    const double altitude = startItem->amslExitAlt();
                    appendPath({ GeoZone::Position{ overshootFrom, altitude }, GeoZone::Position{ overshootFrom.atDistanceAndAzimuth(overshoot, azimuth), altitude } }, i,
                               tr("the VTOL transition at item %1").arg(simpleItem->sequenceNumber()), path.itemFlightDistance(i));
                }
            }
        } else if (LandingComplexItem *const landingItem = qobject_cast<LandingComplexItem*>(visualItem)) {
            const double radius = landingItem->loiterRadius()->rawValue().toDouble();
            if (landingItem->useLoiterToAlt()->rawValue().toBool() && landingItem->finalApproachCoordinate().isValid() && (radius > 0)) {
                appendPath(GeoZoneMissionPath::circle(landingItem->finalApproachCoordinate(), radius, landingItem->amslEntryAlt(), qQNaN()), i,
                           tr("the landing pattern loiter at item %1").arg(landingItem->sequenceNumber()), path.itemFlightDistance(i));
            }
        }
    }

    // The return the vehicle flies after the return to launch item, from the last item before it
    if ((returnItem < 0) || (returnFromItem < 0)) {
        return;
    }
    const VisualMissionItem *const lastItem = visualItems->value<const VisualMissionItem*>(returnFromItem);
    GeoZoneReturnModel::Start start;
    start.position = GeoZone::Position{ lastItem->exitCoordinate(), lastItem->amslExitAlt() };
    start.sequenceNumber = path._sequenceNumbers.value(returnItem, -1);
    const GeoZoneReturnModel::Path returnPath = GeoZoneReturnModel::returnPath(returnParameters, destinations, start);
    const bool flies = std::any_of(returnPath.positions.cbegin(), returnPath.positions.cend(), [&start](const GeoZone::Position &position) {
        return position.coordinate.distanceTo(start.position.coordinate) > 1;
    });
    if (!returnPath.isValid() || !flies) {
        return;
    }
    const QString label = GeoZoneMissionPath::returnDescription(returnPath, returnParameters, true);
    appendPath(GeoZoneMissionPath::_modelPositions(returnPath.positions, GeoZoneTerrain::defaultSpacing), returnItem, label, path.flightDistance());
    appendPath(returnPath.loiter, returnItem, label, path.flightDistance());
}

GeoZoneReturnModel::Parameters GeoZoneMissionReader::returnParameters(Vehicle *vehicle)
{
    if (!vehicle) {
        return GeoZoneReturnModel::Parameters();
    }
    using Firmware = GeoZoneReturnModel::Firmware;
    using VehicleClass = GeoZoneReturnModel::VehicleClass;
    const Firmware firmware = vehicle->px4Firmware() ? Firmware::PX4 : (vehicle->apmFirmware() ? Firmware::ArduPilot : Firmware::Other);
    VehicleClass vehicleClass = VehicleClass::Other;
    if (vehicle->vtol()) {
        vehicleClass = VehicleClass::Vtol;
    } else if (vehicle->fixedWing()) {
        vehicleClass = VehicleClass::FixedWing;
    } else if (vehicle->multiRotor()) {
        vehicleClass = VehicleClass::Multirotor;
    }
    // The firmware's defaults for the plan's firmware and vehicle class until the vehicle's parameters are known
    ParameterManager *const parameterManager = vehicle->parameterManager();
    if (vehicle->isOfflineEditingVehicle() || !parameterManager || !parameterManager->parametersReady()) {
        return GeoZoneReturnModel::defaultParameters(firmware, vehicleClass);
    }
    return GeoZoneReturnModel::parametersFrom(firmware, vehicleClass, [parameterManager](const QString &name) -> std::optional<double> {
        if (!parameterManager->parameterExists(ParameterManager::defaultComponentId, name)) {
            return std::nullopt;
        }
        return parameterManager->getParameter(ParameterManager::defaultComponentId, name)->rawValue().toDouble();
    });
}

double GeoZoneMissionReader::returnAltitude(Vehicle *vehicle)
{
    if (!vehicle || vehicle->isOfflineEditingVehicle() || !vehicle->parameterManager() || !vehicle->parameterManager()->parametersReady()) {
        return qQNaN();
    }
    // PX4 and ArduPlane in meters, ArduCopter and older ArduPlane in centimeters. Zero or less returns at the current altitude.
    static const QList<std::pair<QString, double>> parameters = {
        { QStringLiteral("RTL_RETURN_ALT"), 1.0 },
        { QStringLiteral("RTL_ALTITUDE"), 1.0 },
        { QStringLiteral("RTL_ALT"), 0.01 },
        { QStringLiteral("ALT_HOLD_RTL"), 0.01 },
    };
    ParameterManager *const parameterManager = vehicle->parameterManager();
    for (const auto &[name, scale] : parameters) {
        if (parameterManager->parameterExists(ParameterManager::defaultComponentId, name)) {
            const double value = parameterManager->getParameter(ParameterManager::defaultComponentId, name)->rawValue().toDouble() * scale;
            return (value > 0) ? value : 0;
        }
    }
    return qQNaN();
}

