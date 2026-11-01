/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMissionPath.h"
#include "GeoZoneBox.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneMissionReader.h"
#include "GeoZoneTerrain.h"

#include <algorithm>
#include <cmath>

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
    return GeoZoneMissionReader::read(missionController, withContingencies);
}

GeoZoneMissionPath GeoZoneMissionPath::fromMission(MissionController *missionController, const GeoZoneReturnModel::Parameters &returnParameters,
                                                   bool withContingencies)
{
    return GeoZoneMissionReader::read(missionController, returnParameters, withContingencies);
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

void GeoZoneMissionPath::_appendContingencies(const QHash<int, int> &outgoingSequenceNumbers, const GeoZoneReturnModel::Parameters &returnParameters,
                                              const GeoZoneReturnModel::Destinations &destinations)
{
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
    return GeoZoneMissionReader::returnParameters(vehicle);
}

double GeoZoneMissionPath::returnAltitude(Vehicle *vehicle)
{
    return GeoZoneMissionReader::returnAltitude(vehicle);
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
