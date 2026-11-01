/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMissionPath.h"
#include "ComplexMissionItem.h"
#include "FlightPathSegment.h"
#include "GeoFenceController.h"
#include "GeoZoneBox.h"
#include "GeoZoneEnvironment.h"
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

double GeoZoneMissionPath::Segment::terrainAt(double along) const
{
    const qsizetype count = terrainAMSL.count();
    if ((count < 2) || (terrainSpacing <= 0)) {
        return qQNaN();
    }
    const double length = from.distanceTo(to);
    const qsizetype last = count - 1;
    const auto sampleDistance = [this, last, length](qsizetype index) {
        return (index == last) ? length : std::min(index * terrainSpacing, length);
    };
    const qsizetype index = std::clamp<qsizetype>(static_cast<qsizetype>(along / terrainSpacing), 0, last - 1);
    const double start = sampleDistance(index);
    const double end = sampleDistance(index + 1);
    const double fraction = (end > start) ? std::clamp((along - start) / (end - start), 0.0, 1.0) : 0.0;
    return terrainAMSL[index] + ((terrainAMSL[index + 1] - terrainAMSL[index]) * fraction);
}

GeoZone::Position GeoZoneMissionPath::Segment::positionAt(double along) const
{
    const double length = from.distanceTo(to);
    const double fraction = (length > 0) ? std::clamp(along / length, 0.0, 1.0) : 0.0;
    GeoZone::Position position;
    if (fraction <= 0) {
        position.coordinate = from;
    } else if (fraction >= 1) {
        position.coordinate = to;
    } else {
        position.coordinate = from.atDistanceAndAzimuth(along, from.azimuthTo(to));
    }
    position.altitudeAMSL = fromAltitudeAMSL + ((toAltitudeAMSL - fromAltitudeAMSL) * fraction);
    position.terrainAMSL = terrainAt(along);
    return position;
}

GeoZoneMissionPath GeoZoneMissionPath::fromMission(MissionController *missionController, bool withContingencies)
{
    PlanMasterController *const masterController = missionController ? missionController->masterController() : nullptr;
    return fromMission(missionController, returnParameters(masterController ? masterController->managerVehicle() : nullptr), withContingencies);
}

GeoZoneMissionPath GeoZoneMissionPath::fromMission(MissionController *missionController, const GeoZoneReturnModel::Parameters &returnParameters,
                                                   bool withContingencies)
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
    QList<ItemSegments> itemSegments(visualItems->count());
    double distance = 0;
    const auto appendSegment = [&path, &distance](const Segment &segment) {
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
                Segment gap;
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
        Leg leg;
        leg.fromItemIndex = coordinateItems[k - 1];
        leg.toItemIndex = coordinateItems[k];
        leg.toSequenceNumber = toItem->sequenceNumber();
        leg.segment.from = fromItem->exitCoordinate();
        leg.segment.to = toItem->coordinate();
        leg.segment.itemIndex = leg.fromItemIndex;
        // Altitudes and terrain of the leg from its flight path segment
        const FlightPathSegment *const flightPathSegment = fromItem->simpleFlightPathSegment();
        if (flightPathSegment && leg.segment.to.isValid() && (flightPathSegment->coordinate2().distanceTo(leg.segment.to) < 1)) {
            const Segment known = segmentFrom(flightPathSegment, leg.fromItemIndex);
            leg.segment.fromAltitudeAMSL = known.fromAltitudeAMSL;
            leg.segment.toAltitudeAMSL = known.toAltitudeAMSL;
            leg.segment.terrainAMSL = known.terrainAMSL;
            leg.segment.terrainSpacing = known.terrainSpacing;
            leg.segment.distance = known.distance;
        }
        path._legs.append(leg);
    }

    path._setFlightPaths(missionController, coordinateItems, itemSegments);
    const GeoZoneReturnModel::Destinations destinations = _destinations(missionController, coordinateItems);
    path._appendCoverage(missionController, coordinateItems, returnParameters, destinations);
    if (withContingencies) {
        path._appendContingencies(coordinateItems, itemSegments, returnParameters, destinations);
    }
    return path;
}

void GeoZoneMissionPath::_setFlightPaths(MissionController *missionController, const QList<int> &coordinateItems, const QList<ItemSegments> &itemSegments)
{
    const QmlObjectListModel *const visualItems = missionController->visualItems();
    const int count = visualItems->count();

    // DO_JUMP items with the item they jump to and how many times
    struct Jump {
        int target = -1;
        int repeat = 0;     ///< Negative without end
    };
    QHash<int, Jump> jumps;
    for (int i = 1; i < count; i++) {
        const SimpleMissionItem *const simpleItem = visualItems->value<const SimpleMissionItem*>(i);
        if (!simpleItem || (simpleItem->command() != MAV_CMD_DO_JUMP)) {
            continue;
        }
        const int targetSequence = qRound(simpleItem->missionItem().param1());
        for (int j = 1; j < count; j++) {
            const VisualMissionItem *const item = visualItems->value<const VisualMissionItem*>(j);
            if (item && (j != i) && (item->sequenceNumber() <= targetSequence) && (targetSequence <= item->lastSequenceNumber())) {
                jumps.insert(i, Jump{ j, qRound(simpleItem->missionItem().param2()) });
                break;
            }
        }
    }
    if (jumps.isEmpty()) {
        _paths = pathsFromSegments(_segments);
        _itemFlightDistances = _itemDistances;
        _flightDistance = totalDistance();
        return;
    }

    // Walks the mission like the vehicle: on to the next item along the drawn segment, or straight to the item jumped to
    QHash<int, int> nextCoordinateItems;
    for (qsizetype k = 1; k < coordinateItems.count(); k++) {
        nextCoordinateItems.insert(coordinateItems[k - 1], coordinateItems[k]);
    }
    const qsizetype maxSegments = (std::max)(static_cast<qsizetype>(maxFlightSegments), 2 * _segments.count());
    QList<Segment> flown;
    double flownDistance = 0;
    const auto fly = [&flown, &flownDistance](const Segment &segment) {
        flown.append(segment);
        flownDistance += segment.distance;
    };
    QHash<int, int> jumpsLeft;
    qsizetype jumpCount = 0;
    int lastCoordinateItem = -1;
    int jumpItem = -1;  ///< Jumped at, until the next item with a coordinate
    for (int i = 0; i < count; i++) {
        const VisualMissionItem *const item = visualItems->value<const VisualMissionItem*>(i);
        if (!item) {
            continue;
        }
        if (const auto jump = jumps.constFind(i); jump != jumps.cend()) {
            if (!jumpsLeft.contains(i)) {
                // Loops without end are flown twice
                jumpsLeft.insert(i, (jump->repeat < 0) ? 1 : jump->repeat);
                _repeatsIndefinitely |= (jump->repeat < 0);
            }
            // Also limited for loops without segments
            if ((jumpsLeft[i] > 0) && (flown.count() < maxSegments) && (jumpCount < maxSegments)) {
                jumpsLeft[i]--;
                jumpCount++;
                jumpItem = i;
                i = jump->target - 1;
            }
            continue;
        }
        const SimpleMissionItem *const simpleItem = qobject_cast<const SimpleMissionItem*>(item);
        if (simpleItem && (simpleItem->command() == MAV_CMD_NAV_RETURN_TO_LAUNCH)) {
            break;
        }
        const bool coordinateItem = item->specifiesCoordinate() && !item->isStandaloneCoordinate();
        if (coordinateItem && (lastCoordinateItem >= 0)) {
            if ((jumpItem < 0) && (nextCoordinateItems.value(lastCoordinateItem, -1) == i)) {
                if (itemSegments[lastCoordinateItem].outgoing >= 0) {
                    fly(_segments[itemSegments[lastCoordinateItem].outgoing]);
                }
            } else {
                const VisualMissionItem *const fromItem = visualItems->value<const VisualMissionItem*>(lastCoordinateItem);
                Segment segment;
                segment.from = fromItem->exitCoordinate();
                segment.to = item->coordinate();
                segment.fromAltitudeAMSL = fromItem->amslExitAlt();
                segment.toAltitudeAMSL = item->amslEntryAlt();
                segment.itemIndex = lastCoordinateItem;
                if (jumpItem >= 0) {
                    const int jumpSequence = _sequenceNumbers.value(jumpItem);
                    segment.label = (jumps.value(jumpItem).repeat < 0) ? tr("the jump at item %1, repeated without end").arg(jumpSequence) :
                                                                         tr("the jump at item %1").arg(jumpSequence);
                }
                if (segment.isValid() && (segment.from.distanceTo(segment.to) > 0.01)) {
                    segment.distance = segment.from.distanceTo(segment.to);
                    fly(segment);
                }
            }
            jumpItem = -1;
        }
        if (!_itemFlightDistances.contains(i)) {
            _itemFlightDistances.insert(i, flownDistance);
        }
        for (const int segment : itemSegments[i].segments) {
            fly(_segments[segment]);
        }
        if (coordinateItem) {
            lastCoordinateItem = i;
        }
    }
    if ((_returnSegment >= 0) && (lastCoordinateItem >= 0) && (itemSegments[lastCoordinateItem].outgoing == _returnSegment)) {
        fly(_segments[_returnSegment]);
    }
    // Items the vehicle doesn't reach keep their place on the drawn path
    for (auto it = _itemDistances.cbegin(); it != _itemDistances.cend(); ++it) {
        if (!_itemFlightDistances.contains(it.key())) {
            _itemFlightDistances.insert(it.key(), it.value());
        }
    }
    _paths = pathsFromSegments(flown);
    _flightDistance = flownDistance;
}

GeoZoneReturnModel::Destinations GeoZoneMissionPath::_destinations(MissionController *missionController, const QList<int> &coordinateItems)
{
    GeoZoneReturnModel::Destinations destinations;
    const QmlObjectListModel *const visualItems = missionController->visualItems();
    const QGeoCoordinate home = missionController->plannedHomePosition();
    const double homeAltitude = home.isValid() ? home.altitude() : qQNaN();
    if (home.isValid()) {
        destinations.home = GeoZone::Position{ QGeoCoordinate(home.latitude(), home.longitude()), homeAltitude };
    }
    const auto positionOf = [](const QGeoCoordinate &coordinate, double altitudeAMSL) {
        return GeoZone::Position{ coordinate.isValid() ? QGeoCoordinate(coordinate.latitude(), coordinate.longitude()) : QGeoCoordinate(), altitudeAMSL };
    };

    // Rally points and the breach return point are above home
    if (PlanMasterController *const masterController = missionController->masterController()) {
        const QmlObjectListModel *const rallyPoints = masterController->rallyPointController()->points();
        for (int i = 0; i < rallyPoints->count(); i++) {
            if (const RallyPoint *const rallyPoint = rallyPoints->value<const RallyPoint*>(i)) {
                const QGeoCoordinate coordinate = rallyPoint->coordinate();
                if (coordinate.isValid()) {
                    destinations.rallyPoints.append(positionOf(coordinate, homeAltitude + coordinate.altitude()));
                }
            }
        }
        GeoFenceController *const geoFenceController = masterController->geoFenceController();
        if (geoFenceController->breachReturnPoint().isValid()) {
            destinations.breachReturnPoint = positionOf(geoFenceController->breachReturnPoint(),
                                                        homeAltitude + geoFenceController->breachReturnAltitude()->rawValue().toDouble());
        }
    }

    for (const int index : coordinateItems) {
        if (index > 0) {
            const VisualMissionItem *const item = visualItems->value<const VisualMissionItem*>(index);
            destinations.firstWaypoint = positionOf(item->coordinate(), item->amslEntryAlt());
            break;
        }
    }

    // Landing sequences from their DO_LAND_START, and the return path after a DO_RETURN_PATH_START
    const auto isCoordinateItem = [](const VisualMissionItem *item) { return item && item->specifiesCoordinate() && !item->isStandaloneCoordinate(); };
    bool onReturnPath = false;
    for (int i = 1; i < visualItems->count(); i++) {
        VisualMissionItem *const visualItem = visualItems->value<VisualMissionItem*>(i);
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
        const int command = simpleItem->command();
        if (command == MAV_CMD_DO_LAND_START) {
            GeoZoneReturnModel::Landing landing;
            landing.sequenceNumber = simpleItem->sequenceNumber();
            const QGeoCoordinate marker = simpleItem->coordinate();
            if (simpleItem->specifiesCoordinate() && marker.isValid() && ((marker.latitude() != 0) || (marker.longitude() != 0))) {
                landing.marker = QGeoCoordinate(marker.latitude(), marker.longitude());
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
        } else if (command == MAV_CMD_DO_RETURN_PATH_START) {
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

QList<GeoZone::Position> GeoZoneMissionPath::_modelPositions(const QList<GeoZone::Position> &positions, double terrainSpacing)
{
    // Along the great circle on long legs
    QList<Segment> segments;
    for (qsizetype i = 1; i < positions.count(); i++) {
        Segment segment;
        segment.from = positions[i - 1].coordinate;
        segment.to = positions[i].coordinate;
        segment.fromAltitudeAMSL = positions[i - 1].altitudeAMSL;
        segment.toAltitudeAMSL = positions[i].altitudeAMSL;
        segment.distance = segment.from.distanceTo(segment.to);
        segments.append(segment);
    }
    QList<GeoZone::Position> path;
    for (const Path &segmentsPath : pathsFromSegments(segments)) {
        path.append(segmentsPath.positions);
    }
    // The terrain known at the positions, like at the destinations, is kept
    for (GeoZone::Position &position : path) {
        for (const GeoZone::Position &known : positions) {
            if (!qIsNaN(known.terrainAMSL) && (known.coordinate.distanceTo(position.coordinate) < 0.01)) {
                position.terrainAMSL = known.terrainAMSL;
                break;
            }
        }
    }
    return GeoZoneTerrain::withTerrain(path, terrainSpacing);
}

void GeoZoneMissionPath::_appendCoverage(MissionController *missionController, const QList<int> &coordinateItems, const GeoZoneReturnModel::Parameters &returnParameters,
                                         const GeoZoneReturnModel::Destinations &destinations)
{
    const QmlObjectListModel *const visualItems = missionController->visualItems();

    // Terrain at a position of the flight path, from the segment ending or starting there
    const auto terrainAtPathPosition = [this](const QGeoCoordinate &coordinate) {
        for (const Segment &segment : std::as_const(_segments)) {
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
    const auto appendPath = [this, &terrainAtPathPosition](QList<GeoZone::Position> positions, int itemIndex, const QString &label, double distance) {
        if (positions.count() < 2) {
            return;
        }
        if (qIsNaN(positions.first().terrainAMSL)) {
            positions.first().terrainAMSL = terrainAtPathPosition(positions.first().coordinate);
        }
        const QList<GeoZone::Position> withTerrain = GeoZoneTerrain::withTerrain(positions);
        _paths.append(Path{ withTerrain, itemIndex, label, QList<double>(withTerrain.count(), distance) });
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
    int returnItem = -1;
    for (int i = 0; i < visualItems->count(); i++) {
        VisualMissionItem *const visualItem = visualItems->value<VisualMissionItem*>(i);
        if (SimpleMissionItem *const simpleItem = qobject_cast<SimpleMissionItem*>(visualItem)) {
            const int command = simpleItem->command();
            if (simpleItem->isLoiterItem() && simpleItem->coordinate().isValid()) {
                // Without a radius the vehicle's default applies, multicopters hover at the waypoint
                const double radius = std::abs(simpleItem->loiterRadius());
                if (radius > 0) {
                    appendPath(circle(simpleItem->coordinate(), radius, simpleItem->amslEntryAlt(), qQNaN()), i,
                               tr("the loiter at item %1").arg(simpleItem->sequenceNumber()), itemFlightDistance(i));
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
                               tr("the VTOL transition at item %1").arg(simpleItem->sequenceNumber()), itemFlightDistance(i));
                }
            } else if ((command == MAV_CMD_NAV_RETURN_TO_LAUNCH) && (returnItem < 0)) {
                returnItem = i;
            }
        } else if (LandingComplexItem *const landingItem = qobject_cast<LandingComplexItem*>(visualItem)) {
            const double radius = landingItem->loiterRadius()->rawValue().toDouble();
            if (landingItem->useLoiterToAlt()->rawValue().toBool() && landingItem->finalApproachCoordinate().isValid() && (radius > 0)) {
                appendPath(circle(landingItem->finalApproachCoordinate(), radius, landingItem->amslEntryAlt(), qQNaN()), i,
                           tr("the landing pattern loiter at item %1").arg(landingItem->sequenceNumber()), itemFlightDistance(i));
            }
        }
    }

    // The return the vehicle flies after the return to launch item, from the last item before it
    const QList<int> beforeReturn = previousCoordinateItems(returnItem);
    if ((returnItem < 0) || beforeReturn.isEmpty()) {
        return;
    }
    const VisualMissionItem *const lastItem = visualItems->value<const VisualMissionItem*>(beforeReturn.last());
    GeoZoneReturnModel::Start start;
    start.position = GeoZone::Position{ lastItem->exitCoordinate(), lastItem->amslExitAlt() };
    start.sequenceNumber = _sequenceNumbers.value(returnItem, -1);
    const GeoZoneReturnModel::Path returnPath = GeoZoneReturnModel::returnPath(returnParameters, destinations, start);
    const bool flies = std::any_of(returnPath.positions.cbegin(), returnPath.positions.cend(), [&start](const GeoZone::Position &position) {
        return position.coordinate.distanceTo(start.position.coordinate) > 1;
    });
    if (!returnPath.isValid() || !flies) {
        return;
    }
    const QString label = returnDescription(returnPath, returnParameters, true);
    appendPath(_modelPositions(returnPath.positions, GeoZoneTerrain::defaultSpacing), returnItem, label, flightDistance());
    appendPath(returnPath.loiter, returnItem, label, flightDistance());
}

void GeoZoneMissionPath::_appendContingencies(const QList<int> &coordinateItems, const QList<ItemSegments> &itemSegments,
                                              const GeoZoneReturnModel::Parameters &returnParameters, const GeoZoneReturnModel::Destinations &destinations)
{
    // The sequence numbers the segments lead to
    QHash<int, int> outgoingSequenceNumbers;
    for (qsizetype k = 1; k < coordinateItems.count(); k++) {
        const int outgoing = itemSegments[coordinateItems[k - 1]].outgoing;
        if (outgoing >= 0) {
            outgoingSequenceNumbers.insert(outgoing, _sequenceNumbers.value(coordinateItems[k], -1));
        }
    }
    // The flight path after the take-off from home, before the return home
    const auto flown = [this](qsizetype index) {
        const Segment &segment = _segments[index];
        return segment.isValid() && (segment.itemIndex > 0) && (index != _returnSegment);
    };
    double length = 0;
    for (qsizetype k = 0; k < _segments.count(); k++) {
        if (flown(k)) {
            length += _segments[k].from.distanceTo(_segments[k].to);
        }
    }
    const double spacing = (std::max)(minContingencySpacing, length / maxContingencyStarts);

    // At the items, and between them along long segments
    QList<ContingencyStart> starts;
    const auto appendStart = [&starts](const ContingencyStart &start) {
        if (!starts.isEmpty()) {
            const GeoZone::Position &last = starts.last().position;
            const double altitudeChange = std::abs(last.altitudeAMSL - start.position.altitudeAMSL);
            const bool sameAltitude = (altitudeChange < 1) || (qIsNaN(last.altitudeAMSL) && qIsNaN(start.position.altitudeAMSL));
            if (sameAltitude && (last.coordinate.distanceTo(start.position.coordinate) < 1)) {
                return;
            }
        }
        starts.append(start);
    };
    double offset = 0;
    for (qsizetype k = 0; k < _segments.count(); k++) {
        const Segment &segment = _segments[k];
        const double segmentOffset = offset;
        offset += segment.distance;
        if (!flown(k)) {
            continue;
        }
        const int fromSequence = _sequenceNumbers.value(segment.itemIndex, -1);
        const int toSequence = outgoingSequenceNumbers.value(static_cast<int>(k), fromSequence);
        // Where the vehicle first flies the segment
        double flightOffset = itemFlightDistance(segment.itemIndex) + (segmentOffset - itemDistance(segment.itemIndex));
        if (qIsNaN(flightOffset)) {
            flightOffset = segmentOffset;
        }
        const double segmentLength = segment.from.distanceTo(segment.to);
        const int steps = static_cast<int>(std::floor(segmentLength / spacing));
        appendStart(ContingencyStart{ segment.positionAt(0), fromSequence, fromSequence, flightOffset });
        for (int step = 1; step <= steps; step++) {
            const double along = (segmentLength * step) / (steps + 1);
            appendStart(ContingencyStart{ segment.positionAt(along), fromSequence, toSequence, flightOffset + (segment.distance * along / segmentLength) });
        }
        appendStart(ContingencyStart{ segment.positionAt(segmentLength), toSequence, toSequence, flightOffset + segment.distance });
    }
    // Evenly spread when there are too many
    if (starts.count() > maxContingencyStarts) {
        QList<ContingencyStart> spread;
        for (int i = 0; i < maxContingencyStarts; i++) {
            spread.append(starts[std::llround(static_cast<double>(i) * (starts.count() - 1) / (maxContingencyStarts - 1))]);
        }
        starts = spread;
    }
    _contingencyStarts = starts;

    const auto appendContingency = [this](const GeoZoneReturnModel::Path &path, int start, const QString &label) {
        const bool flies = std::any_of(path.positions.cbegin(), path.positions.cend(), [this, start](const GeoZone::Position &position) {
            return position.coordinate.distanceTo(_contingencyStarts[start].position.coordinate) > 1;
        });
        if (!path.isValid() || (!flies && path.loiter.isEmpty())) {
            return;
        }
        const double distance = _contingencyStarts[start].distance;
        for (const QList<GeoZone::Position> &positions : { _modelPositions(path.positions, contingencyTerrainSpacing),
                                                           GeoZoneTerrain::withTerrain(path.loiter, contingencyTerrainSpacing) }) {
            if (positions.count() >= 2) {
                _contingencyPaths.append(ContingencyPath{ Path{ positions, -1, label, QList<double>(positions.count(), distance) }, start });
            }
        }
    };
    for (qsizetype i = 0; i < _contingencyStarts.count(); i++) {
        const GeoZoneReturnModel::Start start{ _contingencyStarts[i].position, _contingencyStarts[i].toSequenceNumber };
        const GeoZoneReturnModel::Path returnPath = GeoZoneReturnModel::returnPath(returnParameters, destinations, start);
        appendContingency(returnPath, static_cast<int>(i), returnDescription(returnPath, returnParameters, false));
        if (returnParameters.breachAction == GeoZoneReturnModel::BreachAction::ReturnPoint) {
            const GeoZoneReturnModel::Path breachPath = GeoZoneReturnModel::breachPath(returnParameters, destinations, start);
            appendContingency(breachPath, static_cast<int>(i), returnDescription(breachPath, returnParameters, false, true));
        }
    }
}

QString GeoZoneMissionPath::returnDescription(const GeoZoneReturnModel::Path &path, const GeoZoneReturnModel::Parameters &parameters, bool definite, bool breach)
{
    using Destination = GeoZoneReturnModel::Destination;
    const int rallyPoint = path.destinationIndex + 1;
    QString description;
    if (breach) {
        switch (path.destination) {
        case Destination::RallyPoint:
            description = definite ? tr("the flight to rally point %1 after a fence breach").arg(rallyPoint) : tr("a flight to rally point %1 after a fence breach").arg(rallyPoint);
            break;
        case Destination::BreachReturnPoint:
            description = definite ? tr("the flight to the breach return point after a fence breach") : tr("a flight to the breach return point after a fence breach");
            break;
        default:
            description = definite ? tr("the flight home after a fence breach") : tr("a flight home after a fence breach");
            break;
        }
    } else {
        switch (path.destination) {
        case Destination::RallyPoint:
            description = definite ? tr("the return to rally point %1").arg(rallyPoint) : tr("a return to rally point %1").arg(rallyPoint);
            break;
        case Destination::MissionLanding:
            description = definite ? tr("the return to the mission landing") : tr("a return to the mission landing");
            break;
        case Destination::ReturnPath:
            description = definite ? tr("the return to the mission return path") : tr("a return to the mission return path");
            break;
        case Destination::ReverseMission:
            description = definite ? tr("the return home after the mission in reverse") : tr("a return home after the mission in reverse");
            break;
        default:
            description = definite ? tr("the return to launch") : tr("a return to launch");
            break;
        }
    }
    if (!path.altitudeKnown) {
        return tr("%1 at an unknown altitude").arg(description);
    }
    if (parameters.assumed) {
        // Like "assuming the PX4 default return altitude of 30 m"
        const QString altitude = QStringLiteral("%1 %2").arg(qRound(GeoZoneEnvironment::metersToVerticalDistanceUnits(parameters.returnAltitude)))
                                                         .arg(GeoZoneEnvironment::verticalDistanceUnits());
        return tr("%1 assuming the %2 default return altitude of %3").arg(description, GeoZoneReturnModel::autopilotName(parameters.autopilot), altitude);
    }
    return description;
}

QString GeoZoneMissionPath::startsText(QList<int> starts) const
{
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    (void) starts.removeIf([this](int start) { return (start < 0) || (start >= _contingencyStarts.count()); });
    if (starts.isEmpty()) {
        return QString();
    }

    // Runs of consecutive starts as ranges of items
    constexpr int maxRanges = 5;
    QStringList ranges;
    int runs = 0;
    for (qsizetype i = 0; i < starts.count(); i++) {
        const qsizetype first = i;
        while (((i + 1) < starts.count()) && (starts[i + 1] == (starts[i] + 1))) {
            i++;
        }
        if (++runs > maxRanges) {
            continue;
        }
        const int from = _contingencyStarts[starts[first]].fromSequenceNumber;
        const int to = _contingencyStarts[starts[i]].toSequenceNumber;
        ranges.append((from == to) ? QString::number(from) : QStringLiteral(u"%1\u2013%2").arg(from).arg(to));
    }
    if (runs > maxRanges) {
        ranges.append(QStringLiteral(u"\u2026"));
    }
    if ((ranges.count() == 1) && !ranges.constFirst().contains(QChar(u'\u2013'))) {
        return tr("item %1").arg(ranges.constFirst());
    }
    return tr("items %1").arg(ranges.join(QStringLiteral(", ")));
}

GeoZoneReturnModel::Parameters GeoZoneMissionPath::returnParameters(Vehicle *vehicle)
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

double GeoZoneMissionPath::returnAltitude(Vehicle *vehicle)
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

QList<GeoZone::Position> GeoZoneMissionPath::circle(const QGeoCoordinate &center, double radius, double altitudeAMSL, double terrainAMSL)
{
    return GeoZoneReturnModel::circle(center, radius, altitudeAMSL, terrainAMSL);
}

GeoZoneMissionPath GeoZoneMissionPath::fromSegments(const QList<Segment> &segments, const QList<GeoZone::Area> &areas)
{
    GeoZoneMissionPath path;
    path._segments = segments;
    path._areas = areas;
    path._paths = pathsFromSegments(segments);
    path._flightDistance = path.totalDistance();
    return path;
}

QList<QList<GeoZone::Position>> GeoZoneMissionPath::positionLists() const
{
    QList<QList<GeoZone::Position>> lists;
    lists.reserve(_paths.count());
    for (const Path &path : _paths) {
        lists.append(path.positions);
    }
    return lists;
}

double GeoZoneMissionPath::totalDistance() const
{
    double distance = 0;
    for (const Segment &segment : _segments) {
        distance += segment.distance;
    }
    return distance;
}

GeoZoneBox GeoZoneMissionPath::box() const
{
    GeoZoneBox box;
    const auto unite = [&box](const QList<GeoZone::Position> &positions) {
        QList<QGeoCoordinate> coordinates;
        coordinates.reserve(positions.count());
        for (const GeoZone::Position &position : positions) {
            coordinates.append(position.coordinate);
        }
        box = box.united(GeoZoneBox::fromPath(coordinates));
    };
    for (const Path &path : _paths) {
        unite(path.positions);
    }
    for (const ContingencyPath &contingency : _contingencyPaths) {
        unite(contingency.path.positions);
    }
    for (const GeoZone::Area &area : _areas) {
        box = box.united(GeoZoneBox::fromPath(area.polygon));
    }
    return box;
}

QDateTime GeoZoneMissionPath::timeAt(double distance, double totalDistance, const QDateTime &start, const QDateTime &end)
{
    if (!start.isValid() || !end.isValid() || !(totalDistance > 0) || qIsNaN(distance)) {
        return start;
    }
    const double fraction = std::clamp(distance / totalDistance, 0.0, 1.0);
    return start.addMSecs(std::llround(static_cast<double>((std::max)(start.msecsTo(end), qint64{ 0 })) * fraction));
}

QDateTime GeoZoneMissionPath::flightEnd(const QDateTime &start, const QDateTime &end) const
{
    const double total = totalDistance();
    if (!start.isValid() || !end.isValid() || !(total > 0) || !(_flightDistance > total)) {
        return end;
    }
    return start.addMSecs(std::llround(static_cast<double>((std::max)(start.msecsTo(end), qint64{ 0 })) * (_flightDistance / total)));
}

QList<GeoZonePathEvaluator::Path> GeoZoneMissionPath::evaluatorPaths(const QDateTime &start, const QDateTime &end) const
{
    // At the pace of the drawn flight path through the repeats of DO_JUMP items
    const QDateTime flownEnd = flightEnd(start, end);
    QList<GeoZonePathEvaluator::Path> paths;
    paths.reserve(_paths.count());
    for (const Path &path : _paths) {
        GeoZonePathEvaluator::Path evaluatorPath = GeoZonePathEvaluator::Path::fromPositions(path.positions, path.label);
        for (qsizetype i = 0; i < evaluatorPath.vertices.count(); i++) {
            evaluatorPath.vertices[i].time = timeAt(path.distances.value(i, qQNaN()), _flightDistance, start, flownEnd);
        }
        paths.append(evaluatorPath);
    }
    return paths;
}

QList<GeoZonePathEvaluator::Path> GeoZoneMissionPath::contingencyEvaluatorPaths(const QDateTime &start, const QDateTime &end) const
{
    const QDateTime flownEnd = flightEnd(start, end);
    QList<GeoZonePathEvaluator::Path> paths;
    paths.reserve(_contingencyPaths.count());
    for (const ContingencyPath &contingency : _contingencyPaths) {
        GeoZonePathEvaluator::Path evaluatorPath = GeoZonePathEvaluator::Path::fromPositions(contingency.path.positions, contingency.path.label);
        const QDateTime time = timeAt(_contingencyStarts.value(contingency.start).distance, _flightDistance, start, flownEnd);
        for (GeoZonePathEvaluator::Vertex &vertex : evaluatorPath.vertices) {
            vertex.time = time;
        }
        paths.append(evaluatorPath);
    }
    return paths;
}

QList<GeoZoneMissionPath::Path> GeoZoneMissionPath::pathsFromSegments(const QList<Segment> &segments)
{
    QList<Path> paths;
    double offset = 0;
    bool connected = false;
    for (const Segment &segment : segments) {
        const double segmentOffset = offset;
        offset += segment.distance;
        // The vehicle flies an unknown path where the segment has no coordinates
        if (!segment.isValid()) {
            connected = false;
            continue;
        }

        const double distance = segment.from.distanceTo(segment.to);
        const double azimuth = segment.from.azimuthTo(segment.to);
        // A vertical segment, such as a takeoff climb at the launch position, has its terrain samples at the same place
        const bool terrainKnown = (segment.terrainAMSL.count() >= 2) && ((segment.terrainSpacing > 0) || (distance <= 0));

        // Distance along the segment, fraction of the segment and the terrain elevation there
        struct Sample {
            double along;
            double fraction;
            double terrain;
        };
        QList<Sample> samples;
        if (terrainKnown) {
            const qsizetype lastIndex = segment.terrainAMSL.count() - 1;
            for (qsizetype i = 0; i <= lastIndex; i++) {
                const double along = (i == lastIndex) ? distance : std::min(i * segment.terrainSpacing, distance);
                samples.append({ along, (distance > 0) ? (along / distance) : (static_cast<double>(i) / lastIndex), segment.terrainAMSL[i] });
            }
        } else {
            // Vehicles fly long legs along the great circle, which zone checks only follow with intermediate points
            const int steps = std::max(1, std::min(static_cast<int>(std::ceil(distance / maxStepDistance)), maxStepsPerSegment));
            for (int step = 0; step <= steps; step++) {
                samples.append({ distance * step / steps, static_cast<double>(step) / steps, qQNaN() });
            }
        }

        QList<GeoZone::Position> positions;
        QList<double> distances;
        positions.reserve(samples.count());
        distances.reserve(samples.count());
        for (const auto &[along, fraction, terrain] : std::as_const(samples)) {
            distances.append(segmentOffset + (segment.distance * fraction));
            GeoZone::Position position;
            if (fraction <= 0) {
                position.coordinate = segment.from;
            } else if (fraction >= 1) {
                position.coordinate = segment.to;
            } else {
                position.coordinate = segment.from.atDistanceAndAzimuth(along, azimuth);
            }
            position.altitudeAMSL = segment.fromAltitudeAMSL + ((segment.toAltitudeAMSL - segment.fromAltitudeAMSL) * fraction);
            position.terrainAMSL = terrain;
            positions.append(position);
        }

        // Continue the previous path, keeping both ends when the altitude changes at the shared waypoint
        if (connected && (paths.last().label == segment.label) && (paths.last().positions.last().coordinate == segment.from)) {
            GeoZone::Position &last = paths.last().positions.last();
            const bool sameAltitude = (last.altitudeAMSL == positions.first().altitudeAMSL) ||
                                      (qIsNaN(last.altitudeAMSL) && qIsNaN(positions.first().altitudeAMSL));
            if (sameAltitude) {
                if (qIsNaN(last.terrainAMSL)) {
                    last.terrainAMSL = positions.first().terrainAMSL;
                }
                positions.removeFirst();
                distances.removeFirst();
            }
            paths.last().positions.append(positions);
            paths.last().distances.append(distances);
        } else {
            paths.append(Path{ positions, segment.itemIndex, segment.label, distances });
        }
        connected = true;
    }

    // Positions without terrain samples, such as a takeoff climb before its terrain is received, take the terrain known at the same place
    for (Path &path : paths) {
        QList<GeoZone::Position> &positions = path.positions;
        for (qsizetype i = positions.count() - 2; i >= 0; i--) {
            if (qIsNaN(positions[i].terrainAMSL) && (positions[i].coordinate.distanceTo(positions[i + 1].coordinate) < 0.01)) {
                positions[i].terrainAMSL = positions[i + 1].terrainAMSL;
            }
        }
        for (qsizetype i = 1; i < positions.count(); i++) {
            if (qIsNaN(positions[i].terrainAMSL) && (positions[i].coordinate.distanceTo(positions[i - 1].coordinate) < 0.01)) {
                positions[i].terrainAMSL = positions[i - 1].terrainAMSL;
            }
        }
    }
    return paths;
}
