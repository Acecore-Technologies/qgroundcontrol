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
#include "GeoZoneBox.h"
#include "GeoZoneTerrain.h"
#include "LandingComplexItem.h"
#include "MissionController.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "PlanViewSettings.h"
#include "QGCMapPolygon.h"
#include "QmlObjectListModel.h"
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

GeoZoneMissionPath GeoZoneMissionPath::fromMission(MissionController *missionController)
{
    GeoZoneMissionPath path;
    const QmlObjectListModel *const visualItems = missionController ? missionController->visualItems() : nullptr;
    if (!visualItems) {
        return path;
    }

    // Same order and distances as TerrainProfile: the segments of a pattern, then the segment to the next item
    QList<int> coordinateItems;
    for (int i = 0; i < visualItems->count(); i++) {
        VisualMissionItem *const visualItem = visualItems->value<VisualMissionItem*>(i);
        if (!visualItem) {
            continue;
        }
        if (ComplexMissionItem *const complexItem = qobject_cast<ComplexMissionItem*>(visualItem)) {
            const QmlObjectListModel *const complexSegments = complexItem->flightPathSegments();
            if (complexSegments->count() == 0) {
                Segment gap;
                gap.distance = complexItem->complexDistance();
                gap.itemIndex = i;
                path._segments.append(gap);
            }
            for (int j = 0; j < complexSegments->count(); j++) {
                path._segments.append(segmentFrom(complexSegments->value<const FlightPathSegment*>(j), i));
            }
            if (const std::optional<GeoZone::Area> area = areaOf(complexItem)) {
                path._areas.append(*area);
            }
        }
        if (visualItem->simpleFlightPathSegment()) {
            path._segments.append(segmentFrom(visualItem->simpleFlightPathSegment(), i));
        }
        if (visualItem->specifiesCoordinate() && !visualItem->isStandaloneCoordinate()) {
            coordinateItems.append(i);
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

    path._paths = pathsFromSegments(path._segments);
    path._appendCoverage(missionController, coordinateItems);
    return path;
}

void GeoZoneMissionPath::_appendCoverage(MissionController *missionController, const QList<int> &coordinateItems)
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
    const auto appendPath = [this, &terrainAtPathPosition](QList<GeoZone::Position> positions, int itemIndex, const QString &label) {
        if (positions.count() < 2) {
            return;
        }
        if (qIsNaN(positions.first().terrainAMSL)) {
            positions.first().terrainAMSL = terrainAtPathPosition(positions.first().coordinate);
        }
        _paths.append(Path{ GeoZoneTerrain::withTerrain(positions), itemIndex, label });
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
                               tr("the loiter at item %1").arg(simpleItem->sequenceNumber()));
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
                               tr("the VTOL transition at item %1").arg(simpleItem->sequenceNumber()));
                }
            } else if (command == MAV_CMD_NAV_RETURN_TO_LAUNCH) {
                returnItem = i;
            }
        } else if (LandingComplexItem *const landingItem = qobject_cast<LandingComplexItem*>(visualItem)) {
            const double radius = landingItem->loiterRadius()->rawValue().toDouble();
            if (landingItem->useLoiterToAlt()->rawValue().toBool() && landingItem->finalApproachCoordinate().isValid() && (radius > 0)) {
                appendPath(circle(landingItem->finalApproachCoordinate(), radius, landingItem->amslEntryAlt(), qQNaN()), i,
                           tr("the landing pattern loiter at item %1").arg(landingItem->sequenceNumber()));
            }
        }
    }

    // Straight home at the return altitude, climbing to it first
    const QList<int> beforeReturn = previousCoordinateItems(returnItem);
    const QGeoCoordinate home = missionController->plannedHomePosition();
    if ((returnItem >= 0) && !beforeReturn.isEmpty() && home.isValid()) {
        const VisualMissionItem *const lastItem = visualItems->value<const VisualMissionItem*>(beforeReturn.last());
        const QGeoCoordinate from = lastItem->exitCoordinate();
        const double fromAltitude = lastItem->amslExitAlt();
        const double homeAltitude = home.altitude();
        PlanMasterController *const masterController = missionController->masterController();
        const double returnHeight = returnAltitude(masterController ? masterController->managerVehicle() : nullptr);
        double cruise = qQNaN();
        if (!qIsNaN(returnHeight) && !qIsNaN(fromAltitude) && !qIsNaN(homeAltitude)) {
            cruise = (returnHeight > 0) ? (std::max)(fromAltitude, homeAltitude + returnHeight) : fromAltitude;
        }
        const QString label = qIsNaN(cruise) ? tr("the return to launch at an unknown altitude") : tr("the return to launch");
        const QGeoCoordinate homePosition(home.latitude(), home.longitude());
        if (from.isValid() && (from.distanceTo(homePosition) > 1)) {
            for (const Path &returnPath : pathsFromSegments({ Segment{ from, from, fromAltitude, cruise }, Segment{ from, homePosition, cruise, cruise },
                                                              Segment{ homePosition, homePosition, cruise, homeAltitude } })) {
                appendPath(returnPath.positions, returnItem, label);
            }
        }
    }
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
    QList<GeoZone::Position> positions;
    positions.reserve(circleVertexCount + 1);
    for (int i = 0; i <= circleVertexCount; i++) {
        positions.append(GeoZone::Position{ center.atDistanceAndAzimuth(radius, (360.0 * (i % circleVertexCount)) / circleVertexCount), altitudeAMSL, terrainAMSL });
    }
    return positions;
}

GeoZoneMissionPath GeoZoneMissionPath::fromSegments(const QList<Segment> &segments, const QList<GeoZone::Area> &areas)
{
    GeoZoneMissionPath path;
    path._segments = segments;
    path._areas = areas;
    path._paths = pathsFromSegments(segments);
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

QList<GeoZoneMissionPath::Path> GeoZoneMissionPath::pathsFromSegments(const QList<Segment> &segments)
{
    QList<Path> paths;
    for (const Segment &segment : segments) {
        if (!segment.isValid()) {
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
        positions.reserve(samples.count());
        for (const auto &[along, fraction, terrain] : std::as_const(samples)) {
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
        if (!paths.isEmpty() && (paths.last().positions.last().coordinate == segment.from)) {
            GeoZone::Position &last = paths.last().positions.last();
            const bool sameAltitude = (last.altitudeAMSL == positions.first().altitudeAMSL) ||
                                      (qIsNaN(last.altitudeAMSL) && qIsNaN(positions.first().altitudeAMSL));
            if (sameAltitude) {
                if (qIsNaN(last.terrainAMSL)) {
                    last.terrainAMSL = positions.first().terrainAMSL;
                }
                positions.removeFirst();
            }
            paths.last().positions.append(positions);
        } else {
            paths.append(Path{ positions, segment.itemIndex });
        }
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
