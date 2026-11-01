/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneOperationalIntent.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneVector.h"

#include <QtCore/QJsonArray>
#include <QtCore/QTimeZone>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <queue>
#include <vector>

namespace {

using GeoZoneGeometry::LocalProjection;
using Parameters = GeoZoneOperationalIntent::Parameters;
using Volume = GeoZoneOperationalIntent::Volume;

/// Position of the flight, in the projection of the flight
struct Sample {
    QGeoCoordinate coordinate;
    QPointF point;
    double altitude = qQNaN();  ///< Meters above the ellipsoid, NaN when unknown
    qint64 time = 0;            ///< Milliseconds since the epoch
};

/// Part of the flight a volume contains: consecutive legs in time
struct Group {
    QList<int> hull;            ///< Samples on the convex hull, counterclockwise
    double altitudeLower = 0;
    double altitudeUpper = 0;
    qint64 timeStart = 0;
    qint64 timeEnd = 0;
    int previous = -1;
    int next = -1;
    int version = 0;
    bool removed = false;
};

struct Merge {
    double cost = 0;
    int left = -1;
    int right = -1;
    int leftVersion = 0;
    int rightVersion = 0;

    bool operator>(const Merge &other) const { return cost > other.cost; }
};

/// @return Indexes of the points on their convex hull, counterclockwise from the lowest x, without collinear points.
///         Duplicates count once, so one or two points give themselves.
QList<int> convexHull(const QList<QPointF> &points)
{
    QList<int> order(points.count());
    for (int i = 0; i < order.count(); i++) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&points](int a, int b) {
        return (points[a].x() < points[b].x()) || ((points[a].x() == points[b].x()) && (points[a].y() < points[b].y()));
    });
    order.erase(std::unique(order.begin(), order.end(), [&points](int a, int b) { return points[a] == points[b]; }), order.end());
    if (order.count() <= 2) {
        return order;
    }
    const auto turn = [&points](int o, int a, int b) { return GeoZoneVector::cross(points[a] - points[o], points[b] - points[o]); };
    QList<int> hull;
    hull.reserve(order.count() + 1);
    for (int index : std::as_const(order)) {
        while ((hull.count() >= 2) && (turn(hull[hull.count() - 2], hull.last(), index) <= 0)) {
            hull.removeLast();
        }
        hull.append(index);
    }
    const qsizetype lowerCount = hull.count() + 1;
    for (qsizetype i = order.count() - 2; i >= 0; i--) {
        while ((hull.count() >= lowerCount) && (turn(hull[hull.count() - 2], hull.last(), order[i]) <= 0)) {
            hull.removeLast();
        }
        hull.append(order[i]);
    }
    hull.removeLast();
    return hull;
}

QList<int> sampleHull(const QList<int> &indexes, const QList<Sample> &samples)
{
    QList<QPointF> points;
    points.reserve(indexes.count());
    for (int index : indexes) {
        points.append(samples[index].point);
    }
    QList<int> hull;
    for (int i : convexHull(points)) {
        hull.append(indexes[i]);
    }
    return hull;
}

/// @return Sum of the 4D extents of the group's volume: its area, the hull widened by the buffer (Steiner's formula),
///         times its height and duration with the buffers
double extent(const Group &group, const QList<Sample> &samples, const Parameters &parameters)
{
    double area = 0;
    double perimeter = 0;
    const qsizetype count = group.hull.count();
    for (qsizetype i = 0; (count > 1) && (i < count); i++) {
        const QPointF &a = samples[group.hull[i]].point;
        const QPointF &b = samples[group.hull[(i + 1) % count]].point;
        area += GeoZoneVector::cross(a, b) / 2;
        perimeter += GeoZoneVector::length(b - a);
    }
    const double buffer = parameters.horizontalBuffer;
    const double planArea = std::abs(area) + (perimeter * buffer) + (std::numbers::pi * buffer * buffer);
    const double height = (group.altitudeUpper - group.altitudeLower) + (2 * parameters.verticalBuffer);
    const double duration = (static_cast<double>(group.timeEnd - group.timeStart) / 1000) + (2.0 * parameters.timeBuffer);
    return planArea * (std::max)(height, 1.0) * (std::max)(duration, 1.0);
}

Group merged(const Group &a, const Group &b, const QList<Sample> &samples)
{
    Group group = a;
    group.hull = sampleHull(a.hull + b.hull, samples);
    group.altitudeLower = (std::min)(a.altitudeLower, b.altitudeLower);
    group.altitudeUpper = (std::max)(a.altitudeUpper, b.altitudeUpper);
    group.timeStart = (std::min)(a.timeStart, b.timeStart);
    group.timeEnd = (std::max)(a.timeEnd, b.timeEnd);
    return group;
}

/// @return Altitude above mean sea level of the vertex of the path, NaN when unknown
double altitudeAMSL(const GeoZonePathEvaluator::Path &path, const GeoZonePathEvaluator::Vertex &vertex)
{
    switch (path.altitudeReference) {
    case GeoZonePathEvaluator::AltitudeReference::MeanSeaLevel:
        return vertex.altitude;
    case GeoZonePathEvaluator::AltitudeReference::Relative:
        return vertex.altitude + path.referenceAltitudeAMSL;
    case GeoZonePathEvaluator::AltitudeReference::Terrain:
        return vertex.altitude + vertex.terrainAMSL;
    }
    return qQNaN();
}

/// @return The outline of the group: the convex hull of the polygons circumscribing the buffer around its positions,
///         with no edge longer than maxEdgeLength
QList<QGeoCoordinate> outline(const Group &group, const QList<Sample> &samples, const LocalProjection &projection, double buffer)
{
    constexpr int vertexCount = GeoZoneOperationalIntent::circleVertexCount;
    const double radius = buffer * GeoZoneOperationalIntent::bufferMargin / std::cos(std::numbers::pi / vertexCount);
    QList<QPointF> points;
    points.reserve(group.hull.count() * vertexCount);
    for (int index : group.hull) {
        const Sample &sample = samples[index];
        for (int i = 0; i < vertexCount; i++) {
            const QGeoCoordinate vertex = sample.coordinate.atDistanceAndAzimuth(radius, (360.0 * i) / vertexCount);
            points.append(projection.toLocal(vertex, sample.point));
        }
    }
    const QList<int> hull = convexHull(points);
    QList<QGeoCoordinate> coordinates;
    for (qsizetype i = 0; i < hull.count(); i++) {
        const QPointF &from = points[hull[i]];
        const QPointF &to = points[hull[(i + 1) % hull.count()]];
        const QGeoCoordinate fromCoordinate = projection.toGeo(from);
        coordinates.append(fromCoordinate);
        const int steps = static_cast<int>(std::ceil(fromCoordinate.distanceTo(projection.toGeo(to)) / GeoZoneOperationalIntent::maxEdgeLength));
        for (int step = 1; step < steps; step++) {
            coordinates.append(projection.toGeo(from + ((to - from) * (static_cast<double>(step) / steps))));
        }
    }
    return coordinates;
}

QJsonValue jsonNumber(double value)
{
    return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}

QJsonValue jsonTime(const QDateTime &dateTime)
{
    return dateTime.isValid() ? QJsonValue(dateTime.toUTC().toString(Qt::ISODate)) : QJsonValue(QJsonValue::Null);
}

QJsonObject latLngJson(const QGeoCoordinate &coordinate)
{
    return QJsonObject{ { QStringLiteral("lat"), coordinate.latitude() }, { QStringLiteral("lng"), coordinate.longitude() } };
}

QJsonObject altitudeJson(double altitude)
{
    return QJsonObject{ { QStringLiteral("value"), altitude }, { QStringLiteral("reference"), QStringLiteral("W84") }, { QStringLiteral("units"), QStringLiteral("M") } };
}

} // namespace

bool GeoZoneOperationalIntent::Volume::contains(const QGeoCoordinate &coordinate) const
{
    if (isCircle()) {
        return circleCenter.distanceTo(coordinate) <= circleRadius;
    }
    if (outline.count() < 3) {
        return false;
    }
    const LocalProjection projection(outline.first());
    return GeoZoneGeometry::ringContains(projection.toLocal(outline), projection.toLocal(coordinate, projection.toLocal(outline.first())));
}

std::optional<QList<GeoZoneOperationalIntent::Volume>> GeoZoneOperationalIntent::buildVolumes(const QList<GeoZonePathEvaluator::Path> &paths,
                                                                                               const Parameters &requested, QString &errorString)
{
    Parameters parameters = requested;
    parameters.horizontalBuffer = (std::max)(parameters.horizontalBuffer, 1.0);
    parameters.verticalBuffer = (std::max)(parameters.verticalBuffer, 0.0);
    parameters.timeBuffer = (std::max)(parameters.timeBuffer, 0);
    parameters.maxVolumes = (std::max)(parameters.maxVolumes, 1);

    // Legs between the positions, split along their great circles, with the altitudes and times interpolated
    std::optional<LocalProjection> projection;
    QList<Sample> samples;
    QList<std::pair<int, int>> legs;
    for (const GeoZonePathEvaluator::Path &path : paths) {
        int previous = -1;
        double previousAMSL = qQNaN();
        for (const GeoZonePathEvaluator::Vertex &vertex : path.vertices) {
            if (!vertex.coordinate.isValid()) {
                previous = -1;
                continue;
            }
            if (!vertex.time.isValid()) {
                errorString = tr("The flight times are unknown, set the planned start of the flight.");
                return std::nullopt;
            }
            if (!projection) {
                projection.emplace(vertex.coordinate);
            }
            const double amsl = altitudeAMSL(path, vertex);
            const qint64 time = vertex.time.toMSecsSinceEpoch();
            if (previous < 0) {
                samples.append(Sample{ vertex.coordinate, projection->toLocal(vertex.coordinate), amsl + GeoZoneGeometry::geoidHeight(vertex.coordinate), time });
                previous = static_cast<int>(samples.count() - 1);
                previousAMSL = amsl;
                // A path of a single position is a leg of its own
                legs.append({ previous, previous });
                continue;
            }
            if ((legs.last().first == previous) && (legs.last().second == previous)) {
                legs.removeLast();
            }
            const Sample from = samples[previous];
            const double distance = from.coordinate.distanceTo(vertex.coordinate);
            const double azimuth = from.coordinate.azimuthTo(vertex.coordinate);
            const int steps = (std::max)(static_cast<int>(std::ceil(distance / maxLegLength)), 1);
            for (int step = 1; step <= steps; step++) {
                const double fraction = static_cast<double>(step) / steps;
                const QGeoCoordinate coordinate = (step < steps) ? from.coordinate.atDistanceAndAzimuth(distance * fraction, azimuth) : vertex.coordinate;
                const double altitude = previousAMSL + ((amsl - previousAMSL) * fraction) + GeoZoneGeometry::geoidHeight(coordinate);
                const qint64 sampleTime = from.time + std::llround(static_cast<double>(time - from.time) * fraction);
                samples.append(Sample{ coordinate, projection->toLocal(coordinate, samples[previous].point), altitude, sampleTime });
                legs.append({ previous, static_cast<int>(samples.count() - 1) });
                previous = static_cast<int>(samples.count() - 1);
            }
            previousAMSL = amsl;
        }
    }
    if (legs.isEmpty()) {
        errorString = tr("The mission has no flight path.");
        return std::nullopt;
    }

    // Unknown altitudes span those of the flight
    double lowest = qQNaN();
    double highest = qQNaN();
    for (const Sample &sample : std::as_const(samples)) {
        if (!qIsNaN(sample.altitude)) {
            lowest = qIsNaN(lowest) ? sample.altitude : (std::min)(lowest, sample.altitude);
            highest = qIsNaN(highest) ? sample.altitude : (std::max)(highest, sample.altitude);
        }
    }
    if (qIsNaN(lowest)) {
        errorString = tr("The altitudes of the flight path are unknown.");
        return std::nullopt;
    }

    QList<Group> groups;
    groups.reserve(legs.count());
    for (const auto &[from, to] : std::as_const(legs)) {
        const Sample &a = samples[from];
        const Sample &b = samples[to];
        Group group;
        group.hull = sampleHull({ from, to }, samples);
        const bool known = !qIsNaN(a.altitude) && !qIsNaN(b.altitude);
        group.altitudeLower = known ? (std::min)(a.altitude, b.altitude) : lowest;
        group.altitudeUpper = known ? (std::max)(a.altitude, b.altitude) : highest;
        group.timeStart = (std::min)(a.time, b.time);
        group.timeEnd = (std::max)(a.time, b.time);
        groups.append(group);
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
        return (a.timeStart < b.timeStart) || ((a.timeStart == b.timeStart) && (a.timeEnd < b.timeEnd));
    });
    QList<double> extents;
    extents.reserve(groups.count());
    for (int i = 0; i < groups.count(); i++) {
        groups[i].previous = i - 1;
        groups[i].next = (i + 1 < groups.count()) ? (i + 1) : -1;
        extents.append(extent(groups[i], samples, parameters));
    }

    // Adjacent groups merge while it doesn't add to their extents, then the cheapest until few enough remain
    std::priority_queue<Merge, std::vector<Merge>, std::greater<Merge>> merges;
    const auto pushMerge = [&](int left, int right) {
        if ((left < 0) || (right < 0)) {
            return;
        }
        const double cost = extent(merged(groups[left], groups[right], samples), samples, parameters) - extents[left] - extents[right];
        merges.push(Merge{ cost, left, right, groups[left].version, groups[right].version });
    };
    for (int i = 0; i + 1 < groups.count(); i++) {
        pushMerge(i, i + 1);
    }
    qsizetype count = groups.count();
    while (!merges.empty()) {
        const Merge merge = merges.top();
        merges.pop();
        Group &left = groups[merge.left];
        Group &right = groups[merge.right];
        if (left.removed || right.removed || (left.version != merge.leftVersion) || (right.version != merge.rightVersion)) {
            continue;
        }
        if ((merge.cost > 0) && (count <= parameters.maxVolumes)) {
            break;
        }
        const int version = left.version + 1;
        const int next = right.next;
        left = merged(left, right, samples);
        left.version = version;
        left.next = next;
        right.removed = true;
        if (next >= 0) {
            groups[next].previous = merge.left;
        }
        extents[merge.left] = extent(left, samples, parameters);
        count--;
        pushMerge(left.previous, merge.left);
        pushMerge(merge.left, next);
    }

    QList<Volume> volumes;
    volumes.reserve(count);
    const qint64 timeBuffer = qint64{ parameters.timeBuffer } * 1000;
    for (const Group &group : std::as_const(groups)) {
        if (group.removed) {
            continue;
        }
        Volume volume;
        const QGeoCoordinate first = samples[group.hull.first()].coordinate;
        double spread = 0;
        for (int index : group.hull) {
            spread = (std::max)(spread, first.distanceTo(samples[index].coordinate));
        }
        if (spread < 0.01) {
            volume.circleCenter = first;
            volume.circleRadius = (parameters.horizontalBuffer + spread) * bufferMargin;
        } else {
            volume.outline = outline(group, samples, *projection, parameters.horizontalBuffer);
        }
        volume.altitudeLower = std::floor(group.altitudeLower - parameters.verticalBuffer);
        volume.altitudeUpper = std::ceil(group.altitudeUpper + parameters.verticalBuffer);
        const auto floorSeconds = [](qint64 msecs) { return (msecs >= 0) ? ((msecs / 1000) * 1000) : -((((-msecs) + 999) / 1000) * 1000); };
        volume.timeStart = QDateTime::fromMSecsSinceEpoch(floorSeconds(group.timeStart - timeBuffer), QTimeZone::UTC);
        volume.timeEnd = QDateTime::fromMSecsSinceEpoch(-floorSeconds(-(group.timeEnd + timeBuffer)), QTimeZone::UTC);
        if (volume.timeEnd <= volume.timeStart) {
            volume.timeEnd = volume.timeStart.addSecs(1);
        }
        volumes.append(volume);
    }
    std::stable_sort(volumes.begin(), volumes.end(), [](const Volume &a, const Volume &b) {
        return (a.timeStart < b.timeStart) || ((a.timeStart == b.timeStart) && (a.timeEnd < b.timeEnd));
    });
    return volumes;
}

QDateTime GeoZoneOperationalIntent::timeStart() const
{
    QDateTime start;
    for (const Volume &volume : volumes) {
        if (!start.isValid() || (volume.timeStart < start)) {
            start = volume.timeStart;
        }
    }
    return start;
}

QDateTime GeoZoneOperationalIntent::timeEnd() const
{
    QDateTime end;
    for (const Volume &volume : volumes) {
        if (!end.isValid() || (volume.timeEnd > end)) {
            end = volume.timeEnd;
        }
    }
    return end;
}

QJsonObject GeoZoneOperationalIntent::timeJson(const QDateTime &time)
{
    return QJsonObject{ { QStringLiteral("value"), time.toUTC().toString(Qt::ISODate) }, { QStringLiteral("format"), QStringLiteral("RFC3339") } };
}

QJsonObject GeoZoneOperationalIntent::volumeJson(const Volume &volume)
{
    QJsonObject volume3D;
    if (volume.isCircle()) {
        const QJsonObject radius{ { QStringLiteral("value"), volume.circleRadius }, { QStringLiteral("units"), QStringLiteral("M") } };
        volume3D.insert(QStringLiteral("outline_circle"), QJsonObject{ { QStringLiteral("center"), latLngJson(volume.circleCenter) }, { QStringLiteral("radius"), radius } });
    } else {
        QJsonArray vertices;
        for (const QGeoCoordinate &coordinate : volume.outline) {
            vertices.append(latLngJson(coordinate));
        }
        volume3D.insert(QStringLiteral("outline_polygon"), QJsonObject{ { QStringLiteral("vertices"), vertices } });
    }
    volume3D.insert(QStringLiteral("altitude_lower"), altitudeJson(volume.altitudeLower));
    volume3D.insert(QStringLiteral("altitude_upper"), altitudeJson(volume.altitudeUpper));
    return QJsonObject{
        { QStringLiteral("volume"), volume3D },
        { QStringLiteral("time_start"), timeJson(volume.timeStart) },
        { QStringLiteral("time_end"), timeJson(volume.timeEnd) },
    };
}

QJsonObject GeoZoneOperationalIntent::detailsJson() const
{
    QJsonArray volumesJson;
    for (const Volume &volume : volumes) {
        volumesJson.append(volumeJson(volume));
    }
    return QJsonObject{
        { QStringLiteral("volumes"), volumesJson },
        { QStringLiteral("off_nominal_volumes"), QJsonArray() },
        { QStringLiteral("priority"), priority },
    };
}

QJsonObject GeoZoneOperationalIntent::toJson() const
{
    QJsonObject flightJson;
    flightJson.insert(QStringLiteral("name"), flight.name);
    flightJson.insert(QStringLiteral("planFile"), flight.planFile);
    flightJson.insert(QStringLiteral("vehicle"), flight.vehicle);
    flightJson.insert(QStringLiteral("plannedStart"), jsonTime(flight.plannedStart));
    flightJson.insert(QStringLiteral("plannedEnd"), jsonTime(flight.plannedEnd));
    flightJson.insert(QStringLiteral("distanceMeters"), jsonNumber(flight.distance));
    flightJson.insert(QStringLiteral("repeatsIndefinitely"), flight.repeatsIndefinitely);

    QJsonObject parametersJson;
    parametersJson.insert(QStringLiteral("horizontalBufferMeters"), parameters.horizontalBuffer);
    parametersJson.insert(QStringLiteral("verticalBufferMeters"), parameters.verticalBuffer);
    parametersJson.insert(QStringLiteral("timeBufferSeconds"), parameters.timeBuffer);
    parametersJson.insert(QStringLiteral("maxVolumes"), parameters.maxVolumes);

    QJsonArray airspaces;
    for (const Airspace &airspace : uSpaceAirspaces) {
        airspaces.append(QJsonObject{
            { QStringLiteral("key"), airspace.key },
            { QStringLiteral("identifier"), airspace.identifier },
            { QStringLiteral("country"), airspace.country },
            { QStringLiteral("name"), airspace.name },
        });
    }
    QJsonArray authorisationsJson;
    for (const GeoZoneAuthorisation &authorisation : authorisations) {
        authorisationsJson.append(authorisation.toJson());
    }

    const QJsonValue start = timeStart().isValid() ? QJsonValue(timeJson(timeStart())) : QJsonValue(QJsonValue::Null);
    const QJsonValue end = timeEnd().isValid() ? QJsonValue(timeJson(timeEnd())) : QJsonValue(QJsonValue::Null);
    const QJsonObject reference{
        { QStringLiteral("state"), QStringLiteral("Accepted") },
        { QStringLiteral("time_start"), start },
        { QStringLiteral("time_end"), end },
    };

    QJsonObject json;
    json.insert(QStringLiteral("schema"), QString::fromLatin1(jsonSchema));
    json.insert(QStringLiteral("version"), jsonVersion);
    json.insert(QStringLiteral("standard"), QStringLiteral("ASTM F3548-21"));
    json.insert(QStringLiteral("generatedAt"), jsonTime(generatedAt));
    json.insert(QStringLiteral("generator"), QStringLiteral("%1 %2").arg(QCoreApplication::applicationName(), QCoreApplication::applicationVersion()).trimmed());
    json.insert(QStringLiteral("flight"), flightJson);
    json.insert(QStringLiteral("parameters"), parametersJson);
    json.insert(QStringLiteral("uSpaceAirspaces"), airspaces);
    json.insert(QStringLiteral("authorisations"), authorisationsJson);
    json.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    json.insert(QStringLiteral("operational_intent"), QJsonObject{ { QStringLiteral("reference"), reference }, { QStringLiteral("details"), detailsJson() } });
    return json;
}
