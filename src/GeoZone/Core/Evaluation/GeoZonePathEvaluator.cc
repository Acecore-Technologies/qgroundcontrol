/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePathEvaluator.h"
#include "GeoZoneBox.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneText.h"
#include "GeoZoneVertical.h"

#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QTimeZone>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace {

using Approximations = GeoZonePathEvaluator::Approximations;

/// Spans along a path touching within this fraction of a segment are continuous
constexpr double kContinuity = 1e-6;

/// Vertex of a path while its terrain is added, in the datum of the path
struct RawVertex {
    QGeoCoordinate coordinate;
    double altitude = qQNaN();
    double terrain = qQNaN();
    double msecs = qQNaN();         ///< Since the epoch, NaN when unknown
    double source = 0;              ///< Vertex index plus fraction along the input path
    Approximations approximations;  ///< Of the segment from the previous vertex, like a great circle not followed closely
};

double interpolate(double value1, double value2, double fraction)
{
    if (fraction <= 0) {
        return value1;
    }
    if (fraction >= 1) {
        return value2;
    }
    return value1 + ((value2 - value1) * fraction);
}

/// Along the straight line in latitude and longitude the shorter way, like the containment tests of zones
QGeoCoordinate interpolateCoordinate(const QGeoCoordinate &from, const QGeoCoordinate &to, double fraction)
{
    if ((fraction <= 0) || !to.isValid()) {
        return from;
    }
    if ((fraction >= 1) || !from.isValid()) {
        return to;
    }
    const double longitudeDelta = std::remainder(to.longitude() - from.longitude(), 360.0);
    return QGeoCoordinate(from.latitude() + ((to.latitude() - from.latitude()) * fraction),
                          std::remainder(from.longitude() + (longitudeDelta * fraction), 360.0));
}

double toMSecs(const QDateTime &time)
{
    return time.isValid() ? static_cast<double>(time.toMSecsSinceEpoch()) : qQNaN();
}

QDateTime fromMSecs(double msecs)
{
    return std::isnan(msecs) ? QDateTime() : QDateTime::fromMSecsSinceEpoch(std::llround(msecs), QTimeZone::UTC);
}

/// Fills the terrain of the vertices from the lookup and adds vertices every spacing meters between them. Where the
/// lookup has no elevation the terrain stays NaN, so that unknown stretches aren't bridged by interpolation.
/// @param coarse Set when the path is too long for maxPositions at the spacing, which is then widened
QList<RawVertex> addTerrain(const QList<RawVertex> &vertices, const GeoZonePathEvaluator::TerrainLookup &lookup, double spacing, int maxPositions,
                            bool *coarse = nullptr)
{
    if (coarse) {
        *coarse = false;
    }
    if (!lookup) {
        return vertices;
    }
    const auto elevationAt = [&lookup](const QGeoCoordinate &coordinate) {
        return coordinate.isValid() ? lookup(coordinate) : qQNaN();
    };

    double totalLength = 0;
    for (qsizetype i = 1; i < vertices.count(); i++) {
        if (vertices[i - 1].coordinate.isValid() && vertices[i].coordinate.isValid()) {
            totalLength += vertices[i - 1].coordinate.distanceTo(vertices[i].coordinate);
        }
    }
    const double minSpacing = totalLength / (std::max)(maxPositions, 1);
    if (minSpacing > spacing) {
        spacing = minSpacing;
        if (coarse) {
            *coarse = true;
        }
    }

    QList<RawVertex> result;
    result.reserve(vertices.count());
    for (qsizetype i = 0; i < vertices.count(); i++) {
        if (i > 0) {
            const RawVertex &from = vertices[i - 1];
            const RawVertex &to = vertices[i];
            if (from.coordinate.isValid() && to.coordinate.isValid() && (spacing > 0)) {
                const double length = from.coordinate.distanceTo(to.coordinate);
                const double azimuth = from.coordinate.azimuthTo(to.coordinate);
                for (double along = spacing; along < (length - (spacing / 2)); along += spacing) {
                    RawVertex vertex;
                    vertex.coordinate = from.coordinate.atDistanceAndAzimuth(along, azimuth);
                    vertex.terrain = elevationAt(vertex.coordinate);
                    const double fraction = along / length;
                    vertex.altitude = interpolate(from.altitude, to.altitude, fraction);
                    vertex.msecs = interpolate(from.msecs, to.msecs, fraction);
                    vertex.source = interpolate(from.source, to.source, fraction);
                    result.append(vertex);
                }
            }
        }
        RawVertex vertex = vertices[i];
        if (std::isnan(vertex.terrain)) {
            vertex.terrain = elevationAt(vertex.coordinate);
        }
        result.append(vertex);
    }
    return result;
}

/// Adds vertices on the great circles between the vertices, which vehicles fly along, where the edges straight in
/// latitude and longitude, which the zone checks use, would be farther from them than kPathGreatCircleToleranceMeters.
/// Altitudes, terrain and times are interpolated in proportion to the distance.
QList<RawVertex> followGreatCircles(const QList<RawVertex> &vertices)
{
    QList<RawVertex> result;
    result.reserve(vertices.count());
    for (qsizetype i = 0; i < vertices.count(); i++) {
        RawVertex vertex = vertices[i];
        if ((i > 0) && vertices[i - 1].coordinate.isValid() && vertex.coordinate.isValid()) {
            const RawVertex &from = vertices[i - 1];
            const QPointF a(from.coordinate.longitude(), from.coordinate.latitude());
            const QPointF b(vertex.coordinate.longitude(), vertex.coordinate.latitude());
            bool exceeded = false;
            const QList<double> fractions = GeoZoneGeometry::greatCircleFractions(a, b, GeoZoneGeometry::kPathGreatCircleToleranceMeters, &exceeded);
            const Approximations approximations = exceeded ? Approximations(GeoZonePathEvaluator::GreatCircleDeviation) : Approximations();
            for (qsizetype k = 1; k < (fractions.count() - 1); k++) {
                const double fraction = fractions[k];
                result.append(RawVertex{ GeoZone::toCoordinate(GeoZoneGeometry::greatCircleInterpolate(a, b, fraction)), interpolate(from.altitude, vertex.altitude, fraction),
                                         interpolate(from.terrain, vertex.terrain, fraction), interpolate(from.msecs, vertex.msecs, fraction),
                                         interpolate(from.source, vertex.source, fraction), approximations });
            }
            vertex.approximations |= approximations;
        }
        result.append(vertex);
    }
    return result;
}

const GeoZonePolicy &anyZonePolicy()
{
    static const GeoZonePolicy policy({ GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::NoRestriction, GeoZone::USpace,
                                        GeoZone::Unknown });
    return policy;
}

/// Span of the parameter along a prepared path, the segment index plus the fraction along it
struct Span {
    int path = 0;
    double start = 0;
    double end = 0;
    Approximations approximations;
};

struct PartSpan {
    double start = 0;
    double end = 0;
    GeoZone::VerticalPosition position = GeoZone::VerticalPosition::Inside;
};

struct StretchSpan {
    Span span;
    QList<PartSpan> parts;
};

/// @return Why the comparison of the altitudes between the positions with the limits of the volume is approximate
/// @param followsTerrain The altitudes are above the terrain, so they are unknown where it is
Approximations verticalApproximations(const GeoZone::Volume &volume, const GeoZone::Position &from, const GeoZone::Position &to, bool followsTerrain)
{
    const GeoZoneVertical::Approximations vertical = GeoZoneVertical::approximations(volume, from, to);
    Approximations result;
    if (vertical.testFlag(GeoZoneVertical::TerrainUnknown)) {
        result |= GeoZonePathEvaluator::TerrainUnknown;
    }
    if (vertical.testFlag(GeoZoneVertical::FlightLevel)) {
        result |= GeoZonePathEvaluator::FlightLevel;
    }
    if (vertical.testFlag(GeoZoneVertical::ReferenceUnknown)) {
        result |= GeoZonePathEvaluator::AltitudeReferenceUnknown;
    }
    if (vertical.testFlag(GeoZoneVertical::AltitudeUnknown)) {
        const bool terrainUnknown = followsTerrain && (std::isnan(from.terrainAMSL) || std::isnan(to.terrainAMSL));
        result |= terrainUnknown ? GeoZonePathEvaluator::TerrainUnknown : GeoZonePathEvaluator::AltitudeReferenceUnknown;
    }
    return result;
}

void appendPart(QList<PartSpan> &parts, const PartSpan &part)
{
    if (!parts.isEmpty() && (parts.last().position == part.position) && (part.start <= (parts.last().end + kContinuity))) {
        parts.last().end = (std::max)(parts.last().end, part.end);
    } else {
        parts.append(part);
    }
}

} // namespace

/// Path ready for evaluation: altitudes above mean sea level, terrain added, distances measured
struct GeoZonePathEvaluator::Prepared {
    QList<GeoZone::Position> positions;
    QList<double> distances;
    QList<double> msecs;
    QList<double> sources;
    QList<Approximations> approximations;   ///< Of the segment ending at each position
    qsizetype inputCount = 0;
    QString label;
    bool followsTerrain = false;            ///< Altitudes above the terrain
    bool coarseTerrain = false;             ///< Terrain sampled farther apart than the spacing, see TerrainCoarse

    qsizetype segmentCount() const { return (std::max)(positions.count() - 1, qsizetype{ 0 }); }

    PathPoint pointAt(int pathIndex, double parameter) const
    {
        const qsizetype segment = std::clamp<qsizetype>(static_cast<qsizetype>(std::floor(parameter)), 0, segmentCount() - 1);
        const double fraction = std::clamp(parameter - static_cast<double>(segment), 0.0, 1.0);
        const GeoZone::Position &from = positions[segment];
        const GeoZone::Position &to = positions[segment + 1];

        PathPoint point;
        point.path = pathIndex;
        point.position.coordinate = interpolateCoordinate(from.coordinate, to.coordinate, fraction);
        point.position.altitudeAMSL = interpolate(from.altitudeAMSL, to.altitudeAMSL, fraction);
        point.position.terrainAMSL = interpolate(from.terrainAMSL, to.terrainAMSL, fraction);
        point.distance = interpolate(distances[segment], distances[segment + 1], fraction);
        point.time = fromMSecs(interpolate(msecs[segment], msecs[segment + 1], fraction));

        // The vertex indexes of the input path
        const double source = interpolate(sources[segment], sources[segment + 1], fraction);
        if (inputCount < 2) {
            point.segment = 0;
            point.fraction = 0;
        } else {
            point.segment = std::clamp<qsizetype>(static_cast<qsizetype>(std::floor(source)), 0, inputCount - 2);
            point.fraction = std::clamp(source - static_cast<double>(point.segment), 0.0, 1.0);
        }
        return point;
    }
};

GeoZonePathEvaluator::Path GeoZonePathEvaluator::Path::fromPositions(const QList<GeoZone::Position> &positions, const QString &label)
{
    Path path;
    path.label = label;
    path.vertices.reserve(positions.count());
    for (const GeoZone::Position &position : positions) {
        path.vertices.append(Vertex{ position.coordinate, position.altitudeAMSL, position.terrainAMSL, QDateTime() });
    }
    return path;
}

void GeoZonePathEvaluator::Path::setTimes(const QDateTime &start, const QDateTime &end)
{
    double total = 0;
    QList<double> distances;
    distances.reserve(vertices.count());
    for (qsizetype i = 0; i < vertices.count(); i++) {
        if ((i > 0) && vertices[i - 1].coordinate.isValid() && vertices[i].coordinate.isValid()) {
            total += vertices[i - 1].coordinate.distanceTo(vertices[i].coordinate);
        }
        distances.append(total);
    }
    const qint64 duration = (start.isValid() && end.isValid()) ? (std::max)(start.msecsTo(end), qint64{ 0 }) : 0;
    for (qsizetype i = 0; i < vertices.count(); i++) {
        vertices[i].time = (start.isValid() && (total > 0)) ? start.addMSecs(std::llround(duration * (distances[i] / total))) : start;
    }
}

bool GeoZonePathEvaluator::ZoneResult::active() const
{
    if (entered()) {
        return (!areas.isEmpty() && areasActive) || std::any_of(crossings.cbegin(), crossings.cend(), [](const Crossing &crossing) { return crossing.active; });
    }
    return std::any_of(stretches.cbegin(), stretches.cend(), [](const Stretch &stretch) { return stretch.active; });
}

const GeoZonePathEvaluator::Crossing *GeoZonePathEvaluator::ZoneResult::firstCrossing() const
{
    for (const Crossing &crossing : crossings) {
        if (crossing.active) {
            return &crossing;
        }
    }
    return crossings.isEmpty() ? nullptr : &crossings.constFirst();
}

GeoZonePathEvaluator::Approximations GeoZonePathEvaluator::ZoneResult::approximations() const
{
    Approximations result;
    for (const Crossing &crossing : crossings) {
        result |= crossing.approximations;
    }
    for (const Stretch &stretch : stretches) {
        result |= stretch.approximations;
    }
    return result;
}

const GeoZonePathEvaluator::ZoneResult *GeoZonePathEvaluator::Result::find(const GeoZone *zone) const
{
    for (const ZoneResult &result : zones) {
        if (result.zone == zone) {
            return &result;
        }
    }
    return nullptr;
}

QList<const GeoZone*> GeoZonePathEvaluator::Result::activeEnteredZones() const
{
    QList<const GeoZone*> entered;
    for (const ZoneResult &result : zones) {
        if (result.entered() && result.active()) {
            entered.append(result.zone);
        }
    }
    return entered;
}

GeoZonePathEvaluator::Approximations GeoZonePathEvaluator::Result::approximations() const
{
    Approximations result = pathApproximations;
    for (const ZoneResult &zoneResult : zones) {
        result |= zoneResult.approximations();
    }
    return result;
}

GeoZonePathEvaluator::GeoZonePathEvaluator(const GeoZoneCatalog &catalog, const GeoZonePolicy &policy, const QDateTime &time)
    : _catalog(catalog)
    , _policy(policy)
    , _time(time)
{
}

void GeoZonePathEvaluator::setTerrainLookup(const TerrainLookup &lookup, double spacing, int maxPositions)
{
    _terrainLookup = lookup;
    _terrainSpacing = spacing;
    _maxTerrainPositions = maxPositions;
}

QList<GeoZone::Position> GeoZonePathEvaluator::withTerrain(const QList<GeoZone::Position> &path, const TerrainLookup &lookup, double spacing, int maxPositions)
{
    QList<RawVertex> vertices;
    vertices.reserve(path.count());
    for (qsizetype i = 0; i < path.count(); i++) {
        vertices.append(RawVertex{ path[i].coordinate, path[i].altitudeAMSL, path[i].terrainAMSL, qQNaN(), static_cast<double>(i) });
    }
    QList<GeoZone::Position> result;
    for (const RawVertex &vertex : addTerrain(vertices, lookup, spacing, maxPositions)) {
        result.append(GeoZone::Position{ vertex.coordinate, vertex.altitude, vertex.terrain });
    }
    return result;
}

QList<GeoZonePathEvaluator::Prepared> GeoZonePathEvaluator::_prepare(const QList<Path> &paths) const
{
    QList<Prepared> preparedPaths;
    preparedPaths.reserve(paths.count());
    for (const Path &path : paths) {
        Prepared prepared;
        prepared.label = path.label;
        prepared.inputCount = path.vertices.count();
        prepared.followsTerrain = path.altitudeReference == AltitudeReference::Terrain;

        QList<RawVertex> vertices;
        vertices.reserve(path.vertices.count());
        for (qsizetype i = 0; i < path.vertices.count(); i++) {
            const Vertex &vertex = path.vertices[i];
            vertices.append(RawVertex{ vertex.coordinate, vertex.altitude, vertex.terrainAMSL, toMSecs(vertex.time), static_cast<double>(i) });
        }
        vertices = followGreatCircles(addTerrain(vertices, _terrainLookup, _terrainSpacing, _maxTerrainPositions, &prepared.coarseTerrain));
        // A single position is evaluated as a segment without length
        if (vertices.count() == 1) {
            vertices.append(vertices.constFirst());
        }

        double distance = 0;
        for (qsizetype i = 0; i < vertices.count(); i++) {
            const RawVertex &vertex = vertices[i];
            double altitudeAMSL = vertex.altitude;
            switch (path.altitudeReference) {
            case AltitudeReference::MeanSeaLevel:
                break;
            case AltitudeReference::Relative:
                altitudeAMSL += path.referenceAltitudeAMSL;
                break;
            case AltitudeReference::Terrain:
                altitudeAMSL += vertex.terrain;
                break;
            }
            if ((i > 0) && vertices[i - 1].coordinate.isValid() && vertex.coordinate.isValid()) {
                distance += vertices[i - 1].coordinate.distanceTo(vertex.coordinate);
            }
            prepared.positions.append(GeoZone::Position{ vertex.coordinate, altitudeAMSL, vertex.terrain });
            prepared.distances.append(distance);
            prepared.msecs.append(vertex.msecs);
            prepared.sources.append(vertex.source);
            prepared.approximations.append(vertex.approximations);
        }
        preparedPaths.append(prepared);
    }
    return preparedPaths;
}

bool GeoZonePathEvaluator::_accepts(const GeoZone *zone) const
{
    return _policy.acceptsRestriction(zone->restriction()) && !_policy.authorised(zone);
}

GeoZonePathEvaluator::Approximations GeoZonePathEvaluator::_scheduleApproximations(const GeoZone *zone, bool active) const
{
    return (active && (_policy.schedule() != GeoZonePolicy::Schedule::Any) && zone->scheduleAssumed()) ? Approximations(ScheduleAssumed) : Approximations();
}

bool GeoZonePathEvaluator::_activeDuring(const GeoZone *zone, const QDateTime &start, const QDateTime &end) const
{
    if (_policy.schedule() == GeoZonePolicy::Schedule::Any) {
        return true;
    }
    if (start.isValid()) {
        // Estimated times are uncertain, so zones active a little before or after them count too
        const QDateTime last = (end.isValid() && (end > start)) ? end : start;
        return zone->isActiveDuring(start.addSecs(-_scheduleToleranceSecs), last.addSecs(_scheduleToleranceSecs));
    }
    if (_policy.schedule() == GeoZonePolicy::Schedule::Current) {
        return _time.isValid() ? zone->isActiveAt(_time) : zone->active();
    }
    return _policy.acceptsSchedule(zone);
}

GeoZonePathEvaluator::ZoneResult GeoZonePathEvaluator::_evaluateZone(const GeoZone *zone, const QList<Prepared> &paths,
                                                                     const QList<std::pair<int, qsizetype>> &segments) const
{
    const QList<GeoZone::Volume> &volumes = zone->data().volumes;
    QList<QList<StretchSpan>> volumeStretches(volumes.count());
    QList<Span> crossingSpans;

    for (const auto &[pathIndex, segment] : segments) {
        const Prepared &path = paths[pathIndex];
        const GeoZone::Position &from = path.positions[segment];
        const GeoZone::Position &to = path.positions[segment + 1];
        // The segment takes the shorter way, which may cross the antimeridian
        if (!from.coordinate.isValid() || !to.coordinate.isValid() || !zone->box().intersects(GeoZoneBox::fromPath({ from.coordinate, to.coordinate }))) {
            continue;
        }
        const auto positionAt = [&from, &to](double fraction) {
            return GeoZone::Position{ interpolateCoordinate(from.coordinate, to.coordinate, fraction), interpolate(from.altitudeAMSL, to.altitudeAMSL, fraction),
                                      interpolate(from.terrainAMSL, to.terrainAMSL, fraction) };
        };

        for (qsizetype v = 0; v < volumes.count(); v++) {
            const QList<GeoZoneGeometry::Interval> horizontal = zone->horizontalIntervals(v, from.coordinate, to.coordinate);
            if (horizontal.isEmpty()) {
                continue;
            }
            const GeoZone::Volume &volume = volumes[v];
            const auto vertical = GeoZoneVertical::interval(volume, from, to);
            Approximations approximations = verticalApproximations(volume, from, to, path.followsTerrain) | path.approximations[segment + 1];
            if (path.coarseTerrain && (path.followsTerrain || GeoZoneVertical::aboveGround(volume))) {
                approximations |= TerrainCoarse;
            }
            const auto outsidePart = [&volume, &positionAt, segment](double first, double last) {
                return PartSpan{ segment + first, segment + last, GeoZoneVertical::position(volume, positionAt((first + last) / 2)) };
            };

            for (const GeoZoneGeometry::Interval &interval : horizontal) {
                QList<PartSpan> parts;
                const double inFirst = vertical ? (std::max)(interval.first, vertical->first) : 1;
                const double inLast = vertical ? (std::min)(interval.second, vertical->second) : 0;
                if (inFirst <= inLast) {
                    if (interval.first < inFirst) {
                        parts.append(outsidePart(interval.first, inFirst));
                    }
                    parts.append(PartSpan{ segment + inFirst, segment + inLast, GeoZone::VerticalPosition::Inside });
                    if (inLast < interval.second) {
                        parts.append(outsidePart(inLast, interval.second));
                    }
                    crossingSpans.append(Span{ pathIndex, segment + inFirst, segment + inLast, approximations });
                } else {
                    parts.append(outsidePart(interval.first, interval.second));
                }

                // Stretches continue across the vertices of the path
                QList<StretchSpan> &stretches = volumeStretches[v];
                const Span span{ pathIndex, segment + interval.first, segment + interval.second, approximations };
                if (!stretches.isEmpty() && (stretches.last().span.path == pathIndex) && (span.start <= (stretches.last().span.end + kContinuity))) {
                    StretchSpan &last = stretches.last();
                    last.span.end = (std::max)(last.span.end, span.end);
                    last.span.approximations |= span.approximations;
                    for (const PartSpan &part : std::as_const(parts)) {
                        appendPart(last.parts, part);
                    }
                } else {
                    stretches.append(StretchSpan{ span, parts });
                }
            }
        }
    }

    ZoneResult result;
    result.zone = zone;
    result.severity = GeoZone::restrictionSeverity(zone->restriction());

    // Crossings of overlapping volumes are merged
    std::sort(crossingSpans.begin(), crossingSpans.end(), [](const Span &a, const Span &b) {
        return (a.path != b.path) ? (a.path < b.path) : (a.start < b.start);
    });
    QList<Span> merged;
    for (const Span &span : std::as_const(crossingSpans)) {
        if (!merged.isEmpty() && (merged.last().path == span.path) && (span.start <= (merged.last().end + kContinuity))) {
            merged.last().end = (std::max)(merged.last().end, span.end);
            merged.last().approximations |= span.approximations;
        } else {
            merged.append(span);
        }
    }
    for (const Span &span : std::as_const(merged)) {
        const Prepared &path = paths[span.path];
        Crossing crossing{ path.pointAt(span.path, span.start), path.pointAt(span.path, span.end) };
        crossing.active = _activeDuring(zone, crossing.entry.time, crossing.exit.time);
        crossing.approximations = span.approximations | _scheduleApproximations(zone, crossing.active);
        result.crossings.append(crossing);
    }

    for (qsizetype v = 0; v < volumeStretches.count(); v++) {
        for (const StretchSpan &stretchSpan : std::as_const(volumeStretches[v])) {
            const Prepared &path = paths[stretchSpan.span.path];
            Stretch stretch;
            stretch.volume = static_cast<int>(v);
            stretch.entry = path.pointAt(stretchSpan.span.path, stretchSpan.span.start);
            stretch.exit = path.pointAt(stretchSpan.span.path, stretchSpan.span.end);
            for (const PartSpan &part : stretchSpan.parts) {
                stretch.parts.append(VerticalPart{ path.pointAt(stretchSpan.span.path, part.start), path.pointAt(stretchSpan.span.path, part.end), part.position });
            }
            stretch.active = _activeDuring(zone, stretch.entry.time, stretch.exit.time);
            stretch.approximations = stretchSpan.span.approximations | _scheduleApproximations(zone, stretch.active);
            result.stretches.append(stretch);
        }
    }
    std::stable_sort(result.stretches.begin(), result.stretches.end(), [](const Stretch &a, const Stretch &b) {
        return (a.entry.path != b.entry.path) ? (a.entry.path < b.entry.path) : (a.entry.distance < b.entry.distance);
    });

    // What enters the zone besides the main flight path, when only that does
    bool unlabelled = false;
    for (const Crossing &crossing : std::as_const(result.crossings)) {
        if (!crossing.active) {
            continue;
        }
        const QString &label = paths[crossing.entry.path].label;
        if (label.isEmpty()) {
            unlabelled = true;
        } else if (!result.labels.contains(label)) {
            result.labels.append(label);
        }
    }
    if (unlabelled) {
        result.labels.clear();
    }
    return result;
}

GeoZonePathEvaluator::ZoneResult GeoZonePathEvaluator::evaluateZone(const GeoZone *zone, const QList<Path> &paths) const
{
    if (!zone) {
        return ZoneResult();
    }
    const QList<Prepared> prepared = _prepare(paths);
    QList<std::pair<int, qsizetype>> segments;
    for (int p = 0; p < prepared.count(); p++) {
        for (qsizetype i = 0; i < prepared[p].segmentCount(); i++) {
            segments.append({ p, i });
        }
    }
    ZoneResult result = _evaluateZone(zone, prepared, segments);
    if (result.stretches.isEmpty()) {
        result.zone = nullptr;
    }
    return result;
}

GeoZonePathEvaluator::Result GeoZonePathEvaluator::evaluate(const QList<Path> &paths, const QList<GeoZone::Area> &areas) const
{
    const QList<Prepared> prepared = _prepare(paths);

    // Zones near each segment, in the order the paths come near them
    QList<const GeoZone*> candidates;
    QHash<const GeoZone*, qsizetype> candidateIndexes;
    QSet<const GeoZone*> rejected;
    QList<QList<std::pair<int, qsizetype>>> candidateSegments;
    const auto accept = [this, &rejected, &candidateIndexes](const GeoZone *zone) {
        if (candidateIndexes.contains(zone)) {
            return true;
        }
        if (rejected.contains(zone)) {
            return false;
        }
        if (!_accepts(zone)) {
            rejected.insert(zone);
            return false;
        }
        return true;
    };
    for (int p = 0; p < prepared.count(); p++) {
        const QList<GeoZone::Position> &positions = prepared[p].positions;
        for (qsizetype i = 0; i < prepared[p].segmentCount(); i++) {
            const QGeoCoordinate &from = positions[i].coordinate;
            const QGeoCoordinate &to = positions[i + 1].coordinate;
            if (!from.isValid() || !to.isValid()) {
                continue;
            }
            for (const GeoZone *zone : _catalog.zonesInArea(GeoZoneBox::fromPath({ from, to }), anyZonePolicy())) {
                if (!accept(zone)) {
                    continue;
                }
                auto it = candidateIndexes.constFind(zone);
                if (it == candidateIndexes.cend()) {
                    it = candidateIndexes.insert(zone, candidates.count());
                    candidates.append(zone);
                    candidateSegments.append(QList<std::pair<int, qsizetype>>());
                }
                candidateSegments[it.value()].append({ p, i });
            }
        }
    }

    Result result;
    for (const Prepared &path : prepared) {
        for (const Approximations approximations : path.approximations) {
            result.pathApproximations |= approximations;
        }
    }
    QHash<const GeoZone*, qsizetype> resultIndexes;
    for (qsizetype c = 0; c < candidates.count(); c++) {
        ZoneResult zoneResult = _evaluateZone(candidates[c], prepared, candidateSegments[c]);
        if (!zoneResult.stretches.isEmpty()) {
            resultIndexes.insert(candidates[c], result.zones.count());
            result.zones.append(zoneResult);
        }
    }

    // Areas, like patterns whose camera footprints and turns extend past their flight lines, flown at no known time
    for (int a = 0; a < areas.count(); a++) {
        for (const GeoZone *zone : _catalog.zonesIntersectingAreas({ areas[a] }, anyZonePolicy())) {
            if (!accept(zone)) {
                continue;
            }
            auto it = resultIndexes.constFind(zone);
            if (it == resultIndexes.cend()) {
                ZoneResult zoneResult;
                zoneResult.zone = zone;
                zoneResult.severity = GeoZone::restrictionSeverity(zone->restriction());
                it = resultIndexes.insert(zone, result.zones.count());
                result.zones.append(zoneResult);
            }
            ZoneResult &zoneResult = result.zones[it.value()];
            if (zoneResult.areas.isEmpty()) {
                zoneResult.areasActive = _activeDuring(zone, QDateTime(), QDateTime());
                if (zoneResult.areasActive) {
                    zoneResult.labels.clear();
                }
            }
            zoneResult.areas.append(a);
        }
    }

    // Entered by a path first, by where the first path enters, then entered by areas, then passed above or below
    const auto orderKey = [](const ZoneResult &zoneResult) {
        if (!zoneResult.crossings.isEmpty()) {
            const PathPoint &entry = zoneResult.crossings.constFirst().entry;
            return std::make_tuple(0, entry.path, entry.distance);
        }
        if (!zoneResult.areas.isEmpty()) {
            return std::make_tuple(1, zoneResult.areas.constFirst(), 0.0);
        }
        const PathPoint &entry = zoneResult.stretches.constFirst().entry;
        return std::make_tuple(2, entry.path, entry.distance);
    };
    std::stable_sort(result.zones.begin(), result.zones.end(), [&orderKey](const ZoneResult &a, const ZoneResult &b) { return orderKey(a) < orderKey(b); });
    return result;
}

QStringList GeoZonePathEvaluator::approximationTexts(Approximations approximations)
{
    QStringList texts;
    if (approximations.testFlag(TerrainUnknown)) {
        texts.append(tr("Terrain unknown along part of the path, limits above ground taken as reached"));
    }
    if (approximations.testFlag(TerrainCoarse)) {
        texts.append(tr("Terrain sampled coarsely along a long path"));
    }
    if (approximations.testFlag(GreatCircleDeviation)) {
        texts.append(tr("Leg too long to follow its great circle closely"));
    }
    if (approximations.testFlag(FlightLevel)) {
        texts.append(tr("Flight level limits converted with the QNH setting"));
    }
    if (approximations.testFlag(ScheduleAssumed)) {
        texts.append(tr("Zone schedule not fully known, taken as active"));
    }
    if (approximations.testFlag(AltitudeReferenceUnknown)) {
        texts.append(tr("Altitude or its reference unknown, limits taken as reached"));
    }
    return texts;
}

QStringList GeoZonePathEvaluator::approximationCodes(Approximations approximations)
{
    static const QList<std::pair<Approximation, QString>> codes = {
        { TerrainUnknown, QStringLiteral("terrain_unknown") },
        { TerrainCoarse, QStringLiteral("terrain_coarse") },
        { GreatCircleDeviation, QStringLiteral("great_circle_deviation") },
        { FlightLevel, QStringLiteral("flight_level") },
        { ScheduleAssumed, QStringLiteral("schedule_assumed") },
        { AltitudeReferenceUnknown, QStringLiteral("altitude_reference_unknown") },
    };
    QStringList result;
    for (const auto &[approximation, code] : codes) {
        if (approximations.testFlag(approximation)) {
            result.append(code);
        }
    }
    return result;
}

void GeoZonePathEvaluator::sortBySeverity(QList<const GeoZone*> &zones)
{
    std::stable_sort(zones.begin(), zones.end(), [](const GeoZone *a, const GeoZone *b) {
        return GeoZone::restrictionSeverity(a->restriction()) < GeoZone::restrictionSeverity(b->restriction());
    });
}

QString GeoZonePathEvaluator::zoneNames(const QList<const GeoZone*> &zones, int maxListed, bool withRestriction)
{
    QStringList names;
    for (qsizetype i = 0; i < (std::min)(zones.count(), qsizetype{ maxListed }); i++) {
        const GeoZone *const zone = zones[i];
        names.append(withRestriction ? QStringLiteral("%1 (%2)").arg(zone->displayName(), GeoZoneText::restriction(zone->restriction())) : zone->displayName());
    }
    QString text = names.join(QStringLiteral(", "));
    if (zones.count() > maxListed) {
        text += tr(" and %n more", nullptr, static_cast<int>(zones.count() - maxListed));
    }
    return text;
}
