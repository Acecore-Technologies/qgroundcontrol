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
#include "MissionController.h"
#include "QGCMapPolygon.h"
#include "QmlObjectListModel.h"
#include "StructureScanComplexItem.h"
#include "TransectStyleComplexItem.h"
#include "VisualMissionItem.h"

#include <QtPositioning/QGeoRectangle>

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

    const QGeoCoordinate center = QGeoRectangle(polygon).center();
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
    return path;
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
