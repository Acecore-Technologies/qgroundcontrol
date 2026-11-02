/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMissionPathBuilder.h"
#include "GeoZoneTerrain.h"

#include <algorithm>
#include <cmath>

GeoZoneMissionPath GeoZoneMissionPathBuilder::build(const QList<Item> &items, const Parameters &parameters, bool withContingencies)
{
    GeoZoneMissionPath path;

    // The mission controller draws a segment home from the last item with a coordinate before a return to launch
    const auto returnToLaunch = std::find_if(items.cbegin(), items.cend(), [](const Item &item) {
        return item.item.kind == GeoZoneMissionItem::Kind::ReturnToLaunch;
    });
    const int returnItem = (returnToLaunch != items.cend()) ? static_cast<int>(std::distance(items.cbegin(), returnToLaunch)) : -1;

    // Same order and distances as TerrainProfile: the segments of a pattern, then the segment to the next item
    QList<int> coordinateItems;
    QList<GeoZoneMissionJumps::ItemSegments> itemSegments(items.count());
    QList<GeoZoneMissionItem> missionItems;
    missionItems.reserve(items.count());
    double distance = 0;
    const auto appendSegment = [&path, &distance](GeoZoneMissionPath::Segment segment, int itemIndex) {
        segment.itemIndex = itemIndex;
        path._segments.append(segment);
        distance += segment.distance;
        return static_cast<int>(path._segments.count() - 1);
    };
    for (int i = 0; i < items.count(); i++) {
        const Item &item = items[i];
        missionItems.append(item.item);
        path._itemDistances.insert(i, distance);
        path._sequenceNumbers.insert(i, item.item.sequenceNumber);
        for (const GeoZoneMissionPath::Segment &segment : item.segments) {
            itemSegments[i].segments.append(appendSegment(segment, i));
        }
        if (item.area) {
            path._areas.append(*item.area);
        }
        if (item.outgoing) {
            itemSegments[i].outgoing = appendSegment(*item.outgoing, i);
        }
        if (item.item.flies) {
            coordinateItems.append(i);
        }
    }
    for (qsizetype k = coordinateItems.count() - 1; (k >= 0) && (returnItem >= 0); k--) {
        if (coordinateItems[k] < returnItem) {
            const int returnSegment = itemSegments[coordinateItems[k]].outgoing;
            if ((returnSegment >= 0) && parameters.home.isValid() && (path._segments[returnSegment].to.distanceTo(parameters.home) < 1)) {
                path._returnSegment = returnSegment;
            }
            break;
        }
    }

    for (qsizetype k = 1; k < coordinateItems.count(); k++) {
        const Item &fromItem = items[coordinateItems[k - 1]];
        const Item &toItem = items[coordinateItems[k]];
        GeoZoneMissionPath::Leg leg;
        leg.fromItemIndex = coordinateItems[k - 1];
        leg.toItemIndex = coordinateItems[k];
        leg.toSequenceNumber = toItem.item.sequenceNumber;
        leg.segment.from = fromItem.item.exit.coordinate;
        leg.segment.to = toItem.item.entry.coordinate;
        leg.segment.itemIndex = leg.fromItemIndex;
        // Altitudes and terrain of the leg from its flight path segment
        if (fromItem.outgoing && leg.segment.to.isValid() && (fromItem.outgoing->to.distanceTo(leg.segment.to) < 1)) {
            const GeoZoneMissionPath::Segment &known = *fromItem.outgoing;
            leg.segment.fromAltitudeAMSL = known.fromAltitudeAMSL;
            leg.segment.toAltitudeAMSL = known.toAltitudeAMSL;
            leg.segment.terrainAMSL = known.terrainAMSL;
            leg.segment.terrainSpacing = known.terrainSpacing;
            leg.segment.distance = known.distance;
        }
        path._legs.append(leg);
    }

    const FlightPaths flight = _flightPaths(path, missionItems, itemSegments);
    path._paths = flight.paths;
    path._itemFlightDistances = flight.itemDistances;
    path._flightDistance = flight.distance;
    path._repeatsIndefinitely = flight.repeatsIndefinitely;
    path._paths.append(_coveragePaths(path, items, coordinateItems, parameters, flight.returnItem, flight.returnFromItem));
    if (withContingencies) {
        // The sequence numbers the segments lead to
        QHash<int, int> outgoingSequenceNumbers;
        for (qsizetype k = 1; k < coordinateItems.count(); k++) {
            const int outgoing = itemSegments[coordinateItems[k - 1]].outgoing;
            if (outgoing >= 0) {
                outgoingSequenceNumbers.insert(outgoing, path._sequenceNumbers.value(coordinateItems[k], -1));
            }
        }
        _appendContingencies(path, outgoingSequenceNumbers, parameters);
    }
    return path;
}

GeoZoneMissionPathBuilder::FlightPaths GeoZoneMissionPathBuilder::_flightPaths(const GeoZoneMissionPath &path, const QList<GeoZoneMissionItem> &items,
                                                                               const QList<GeoZoneMissionJumps::ItemSegments> &itemSegments)
{
    const QList<GeoZoneMissionPath::Segment> &segments = path._segments;
    FlightPaths flight;
    if (!GeoZoneMissionJumps::hasJumps(items)) {
        flight.paths = GeoZoneMissionPath::pathsFromSegments(segments);
        flight.itemDistances = path._itemDistances;
        for (const GeoZoneMissionPath::Segment &segment : segments) {
            flight.distance += segment.distance;
        }
        // The mission ends at the first return to launch
        int returnFromItem = -1;
        for (int i = 0; i < items.count(); i++) {
            if (items[i].kind == GeoZoneMissionItem::Kind::ReturnToLaunch) {
                flight.returnItem = i;
                flight.returnFromItem = returnFromItem;
                break;
            }
            if (items[i].flies) {
                returnFromItem = i;
            }
        }
        return flight;
    }

    QList<double> segmentDistances;
    segmentDistances.reserve(segments.count());
    for (const GeoZoneMissionPath::Segment &segment : segments) {
        segmentDistances.append(segment.distance);
    }
    const int maxSteps = static_cast<int>((std::max)(static_cast<qsizetype>(GeoZoneMissionPath::maxFlightSegments), 2 * segments.count()));
    const GeoZoneMissionJumps::Walk walk = GeoZoneMissionJumps::walk(items, itemSegments, segmentDistances, path._returnSegment, maxSteps);
    QList<GeoZoneMissionPath::Segment> flown;
    flown.reserve(walk.steps.count());
    for (const GeoZoneMissionJumps::Step &step : walk.steps) {
        if (step.segment >= 0) {
            flown.append(segments[step.segment]);
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
    flight.itemDistances = walk.itemDistances;
    // Items the vehicle doesn't reach keep their place on the drawn path
    for (auto it = path._itemDistances.cbegin(); it != path._itemDistances.cend(); ++it) {
        if (!flight.itemDistances.contains(it.key())) {
            flight.itemDistances.insert(it.key(), it.value());
        }
    }
    flight.paths = GeoZoneMissionPath::pathsFromSegments(flown);
    flight.distance = walk.distance;
    flight.repeatsIndefinitely = walk.repeatsIndefinitely;
    flight.returnItem = walk.returnItem;
    flight.returnFromItem = (walk.returnItem >= 0) ? walk.lastCoordinateItem : -1;
    return flight;
}

QList<GeoZoneMissionPath::Path> GeoZoneMissionPathBuilder::_coveragePaths(const GeoZoneMissionPath &path, const QList<Item> &items,
                                                                          const QList<int> &coordinateItems, const Parameters &parameters,
                                                                          int returnItem, int returnFromItem)
{
    QList<GeoZoneMissionPath::Path> paths;

    // Terrain at a position of the flight path, from the segment ending or starting there
    const auto terrainAtPathPosition = [&path](const QGeoCoordinate &coordinate) {
        for (const GeoZoneMissionPath::Segment &segment : path.segments()) {
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
    const auto appendPath = [&paths, &terrainAtPathPosition](QList<GeoZone::Position> positions, int itemIndex, const QString &label, double distance) {
        if (positions.count() < 2) {
            return;
        }
        if (qIsNaN(positions.first().terrainAMSL)) {
            positions.first().terrainAMSL = terrainAtPathPosition(positions.first().coordinate);
        }
        const QList<GeoZone::Position> withTerrain = GeoZoneTerrain::withTerrain(positions);
        paths.append(GeoZoneMissionPath::Path{ withTerrain, itemIndex, label, QList<double>(withTerrain.count(), distance) });
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

    const double transitionDistance = parameters.vtolTransitionDistance;
    for (int i = 0; i < items.count(); i++) {
        const Item &item = items[i];
        const int sequenceNumber = item.item.sequenceNumber;
        if ((item.loiterRadius > 0) && item.loiterCenter.isValid()) {
            const QString label = item.landingLoiter ? GeoZoneMissionPath::tr("the landing pattern loiter at item %1").arg(sequenceNumber)
                                                     : GeoZoneMissionPath::tr("the loiter at item %1").arg(sequenceNumber);
            appendPath(GeoZoneMissionPath::circle(item.loiterCenter, item.loiterRadius, item.item.entry.altitudeAMSL, qQNaN()), i, label,
                       path.itemFlightDistance(i));
        } else if (item.vtolTransition != VtolTransition::None) {
            // Transitions take about the transition distance, flying on past the waypoints they follow or lead to
            const QList<int> previous = previousCoordinateItems(i);
            const auto next = std::find_if(coordinateItems.cbegin(), coordinateItems.cend(), [i](int index) { return index > i; });
            if (previous.isEmpty() || (transitionDistance <= 0)) {
                continue;
            }
            const GeoZone::Position &startExit = items[previous.last()].item.exit;
            const QGeoCoordinate start = startExit.coordinate;
            QGeoCoordinate overshootFrom;
            double azimuth = qQNaN();
            double overshoot = 0;
            if ((item.vtolTransition == VtolTransition::ToMulticopter) && (previous.count() >= 2)) {
                // Slowing down to hover after the waypoint, along the leg to it
                const QGeoCoordinate before = items[previous[previous.count() - 2]].item.exit.coordinate;
                if (before.isValid() && (before.distanceTo(start) > 1)) {
                    overshootFrom = start;
                    azimuth = before.azimuthTo(start);
                    overshoot = transitionDistance;
                }
            } else if ((item.vtolTransition == VtolTransition::ToFixedWing) && (next != coordinateItems.cend())) {
                // Speeding up towards the next waypoint, past it when it is closer than the transition distance
                const QGeoCoordinate end = items[*next].item.entry.coordinate;
                if (end.isValid() && (start.distanceTo(end) > 1)) {
                    overshootFrom = end;
                    azimuth = start.azimuthTo(end);
                    overshoot = transitionDistance - start.distanceTo(end);
                }
            }
            if (overshootFrom.isValid() && (overshoot > 0)) {
                const double altitude = startExit.altitudeAMSL;
                appendPath({ GeoZone::Position{ overshootFrom, altitude }, GeoZone::Position{ overshootFrom.atDistanceAndAzimuth(overshoot, azimuth), altitude } }, i,
                           GeoZoneMissionPath::tr("the VTOL transition at item %1").arg(sequenceNumber), path.itemFlightDistance(i));
            }
        }
    }

    // The return the vehicle flies after the return to launch item, from the last item before it
    if ((returnItem < 0) || (returnFromItem < 0)) {
        return paths;
    }
    GeoZoneReturnModel::Start start;
    start.position = items[returnFromItem].item.exit;
    start.sequenceNumber = path.sequenceNumber(returnItem);
    const GeoZoneReturnModel::Path returnPath = GeoZoneReturnModel::returnPath(parameters.returnParameters, parameters.destinations, start);
    const bool flies = std::any_of(returnPath.positions.cbegin(), returnPath.positions.cend(), [&start](const GeoZone::Position &position) {
        return position.coordinate.distanceTo(start.position.coordinate) > 1;
    });
    if (!returnPath.isValid() || !flies) {
        return paths;
    }
    const QString label = GeoZoneMissionPath::returnDescription(returnPath, parameters.returnParameters, true);
    appendPath(_modelPositions(returnPath.positions, GeoZoneTerrain::defaultSpacing), returnItem, label, path.flightDistance());
    appendPath(returnPath.loiter, returnItem, label, path.flightDistance());
    return paths;
}

void GeoZoneMissionPathBuilder::_appendContingencies(GeoZoneMissionPath &path, const QHash<int, int> &outgoingSequenceNumbers, const Parameters &parameters)
{
    using Segment = GeoZoneMissionPath::Segment;
    using ContingencyStart = GeoZoneMissionPath::ContingencyStart;
    const QList<Segment> &segments = path._segments;

    // The flight path after the take-off from home, before the return home
    const auto flown = [&path, &segments](qsizetype index) {
        const Segment &segment = segments[index];
        return segment.isValid() && (segment.itemIndex > 0) && (index != path._returnSegment);
    };
    double length = 0;
    for (qsizetype k = 0; k < segments.count(); k++) {
        if (flown(k)) {
            length += segments[k].from.distanceTo(segments[k].to);
        }
    }
    const double spacing = (std::max)(GeoZoneMissionPath::minContingencySpacing, length / GeoZoneMissionPath::maxContingencyStarts);

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
    for (qsizetype k = 0; k < segments.count(); k++) {
        const Segment &segment = segments[k];
        const double segmentOffset = offset;
        offset += segment.distance;
        if (!flown(k)) {
            continue;
        }
        const int fromSequence = path._sequenceNumbers.value(segment.itemIndex, -1);
        const int toSequence = outgoingSequenceNumbers.value(static_cast<int>(k), fromSequence);
        // Where the vehicle first flies the segment
        double flightOffset = path.itemFlightDistance(segment.itemIndex) + (segmentOffset - path.itemDistance(segment.itemIndex));
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
    constexpr int maxStarts = GeoZoneMissionPath::maxContingencyStarts;
    if (starts.count() > maxStarts) {
        QList<ContingencyStart> spread;
        for (int i = 0; i < maxStarts; i++) {
            spread.append(starts[std::llround(static_cast<double>(i) * (starts.count() - 1) / (maxStarts - 1))]);
        }
        starts = spread;
    }
    path._contingencyStarts = starts;

    const auto appendContingency = [&path](const GeoZoneReturnModel::Path &returnPath, int start, const QString &label) {
        const bool flies = std::any_of(returnPath.positions.cbegin(), returnPath.positions.cend(), [&path, start](const GeoZone::Position &position) {
            return position.coordinate.distanceTo(path._contingencyStarts[start].position.coordinate) > 1;
        });
        if (!returnPath.isValid() || (!flies && returnPath.loiter.isEmpty())) {
            return;
        }
        const double distance = path._contingencyStarts[start].distance;
        constexpr double terrainSpacing = GeoZoneMissionPath::contingencyTerrainSpacing;
        for (const QList<GeoZone::Position> &positions : { _modelPositions(returnPath.positions, terrainSpacing),
                                                           GeoZoneTerrain::withTerrain(returnPath.loiter, terrainSpacing) }) {
            if (positions.count() >= 2) {
                path._contingencyPaths.append(GeoZoneMissionPath::ContingencyPath{ GeoZoneMissionPath::Path{ positions, -1, label, QList<double>(positions.count(), distance) },
                                                                                   start });
            }
        }
    };
    const GeoZoneReturnModel::Parameters &returnParameters = parameters.returnParameters;
    for (qsizetype i = 0; i < path._contingencyStarts.count(); i++) {
        const GeoZoneReturnModel::Start start{ path._contingencyStarts[i].position, path._contingencyStarts[i].toSequenceNumber };
        const GeoZoneReturnModel::Path returnPath = GeoZoneReturnModel::returnPath(returnParameters, parameters.destinations, start);
        appendContingency(returnPath, static_cast<int>(i), GeoZoneMissionPath::returnDescription(returnPath, returnParameters, false));
        if (returnParameters.breachAction == GeoZoneReturnModel::BreachAction::ReturnPoint) {
            const GeoZoneReturnModel::Path breachPath = GeoZoneReturnModel::breachPath(returnParameters, parameters.destinations, start);
            appendContingency(breachPath, static_cast<int>(i), GeoZoneMissionPath::returnDescription(breachPath, returnParameters, false, true));
        }
    }
}

QList<GeoZone::Position> GeoZoneMissionPathBuilder::_modelPositions(const QList<GeoZone::Position> &positions, double terrainSpacing)
{
    // Along the great circle on long legs
    QList<GeoZoneMissionPath::Segment> segments;
    for (qsizetype i = 1; i < positions.count(); i++) {
        GeoZoneMissionPath::Segment segment;
        segment.from = positions[i - 1].coordinate;
        segment.to = positions[i].coordinate;
        segment.fromAltitudeAMSL = positions[i - 1].altitudeAMSL;
        segment.toAltitudeAMSL = positions[i].altitudeAMSL;
        segment.distance = segment.from.distanceTo(segment.to);
        segments.append(segment);
    }
    QList<GeoZone::Position> path;
    for (const GeoZoneMissionPath::Path &segmentsPath : GeoZoneMissionPath::pathsFromSegments(segments)) {
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
