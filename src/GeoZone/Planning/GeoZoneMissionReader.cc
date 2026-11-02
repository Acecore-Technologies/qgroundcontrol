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
#include "GeoZoneMissionPathBuilder.h"
#include "GeoZoneReturnDestinations.h"
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

// Reading the mission of a MissionController the way it draws its flight path
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

/// @return Where the vehicle can return to in the plan: the planned home, the rally points and the breach return
///         point above it, and the destinations of the mission items, see GeoZoneReturnDestinations::fromMission
/// @param items Of the mission, see GeoZoneMissionReader::items
GeoZoneReturnModel::Destinations destinations(MissionController *missionController, const QList<GeoZoneMissionItem> &items)
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

} // namespace

GeoZoneMissionPath GeoZoneMissionReader::path(MissionController *missionController, bool withContingencies)
{
    PlanMasterController *const masterController = missionController ? missionController->masterController() : nullptr;
    return path(missionController, returnParameters(masterController ? masterController->managerVehicle() : nullptr), withContingencies);
}

GeoZoneMissionPath GeoZoneMissionReader::path(MissionController *missionController, const GeoZoneReturnModel::Parameters &returnParameters,
                                              bool withContingencies)
{
    const QmlObjectListModel *const visualItems = missionController ? missionController->visualItems() : nullptr;
    if (!visualItems) {
        return GeoZoneMissionPath();
    }

    const QList<GeoZoneMissionItem> missionItems = items(missionController);
    QList<GeoZoneMissionPathBuilder::Item> builderItems;
    builderItems.reserve(visualItems->count());
    for (int i = 0; i < visualItems->count(); i++) {
        GeoZoneMissionPathBuilder::Item item;
        item.item = missionItems.value(i);
        VisualMissionItem *const visualItem = visualItems->value<VisualMissionItem*>(i);
        if (!visualItem) {
            builderItems.append(item);
            continue;
        }
        if (ComplexMissionItem *const complexItem = qobject_cast<ComplexMissionItem*>(visualItem)) {
            const QmlObjectListModel *const complexSegments = complexItem->flightPathSegments();
            if (complexSegments->count() == 0) {
                GeoZoneMissionPath::Segment gap;
                gap.distance = complexItem->complexDistance();
                item.segments.append(gap);
            }
            for (int j = 0; j < complexSegments->count(); j++) {
                item.segments.append(segmentFrom(complexSegments->value<const FlightPathSegment*>(j), i));
            }
            item.area = areaOf(complexItem);
        }
        if (visualItem->simpleFlightPathSegment()) {
            item.outgoing = segmentFrom(visualItem->simpleFlightPathSegment(), i);
        }
        if (SimpleMissionItem *const simpleItem = qobject_cast<SimpleMissionItem*>(visualItem)) {
            if (simpleItem->isLoiterItem()) {
                // Without a radius the vehicle's default applies, multicopters hover at the waypoint
                item.loiterRadius = std::abs(simpleItem->loiterRadius());
                item.loiterCenter = simpleItem->coordinate();
            } else if (simpleItem->command() == MAV_CMD_DO_VTOL_TRANSITION) {
                switch (qRound(simpleItem->missionItem().param1())) {
                case MAV_VTOL_STATE_MC:
                    item.vtolTransition = GeoZoneMissionPathBuilder::VtolTransition::ToMulticopter;
                    break;
                case MAV_VTOL_STATE_FW:
                    item.vtolTransition = GeoZoneMissionPathBuilder::VtolTransition::ToFixedWing;
                    break;
                default:
                    break;
                }
            }
        } else if (LandingComplexItem *const landingItem = qobject_cast<LandingComplexItem*>(visualItem)) {
            if (landingItem->useLoiterToAlt()->rawValue().toBool()) {
                item.loiterRadius = landingItem->loiterRadius()->rawValue().toDouble();
                item.loiterCenter = landingItem->finalApproachCoordinate();
                item.landingLoiter = true;
            }
        }
        builderItems.append(item);
    }

    GeoZoneMissionPathBuilder::Parameters parameters;
    parameters.home = missionController->plannedHomePosition();
    parameters.returnParameters = returnParameters;
    parameters.destinations = destinations(missionController, missionItems);
    parameters.vtolTransitionDistance = SettingsManager::instance()->planViewSettings()->vtolTransitionDistance()->rawValue().toDouble();
    return GeoZoneMissionPathBuilder::build(builderItems, parameters, withContingencies);
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
