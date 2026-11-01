/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneIntentTest.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneOperationalIntent.h"
#include "GeoZoneTestHelpers.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QTimeZone>
#include <QtCore/QtMath>
#include <QtTest/QTest>

#include <cmath>

namespace {

using Intent = GeoZoneOperationalIntent;
using Path = GeoZonePathEvaluator::Path;
using Vertex = GeoZonePathEvaluator::Vertex;

const QDateTime kStart(QDate(2026, 6, 1), QTime(10, 0), QTimeZone::UTC);

QGeoCoordinate offset(const QGeoCoordinate &origin, double north, double east)
{
    return origin.atDistanceAndAzimuth(std::hypot(north, east), qRadiansToDegrees(std::atan2(east, north)));
}

/// Position meters north and east of the origin, at the altitude AMSL, flown seconds after kStart
struct Point {
    double north = 0;
    double east = 0;
    double altitude = 500;
    double seconds = 0;
};

Path path(const QGeoCoordinate &origin, const QList<Point> &points)
{
    Path result;
    for (const Point &point : points) {
        result.vertices.append(Vertex{ offset(origin, point.north, point.east), point.altitude, qQNaN(), kStart.addMSecs(std::llround(point.seconds * 1000)) });
    }
    return result;
}

/// Path through the positions at the speed in meters per second from kStart
Path flown(const QGeoCoordinate &origin, const QList<Point> &points, double speed)
{
    QList<Point> timed = points;
    double seconds = 0;
    for (qsizetype i = 0; i < timed.count(); i++) {
        if (i > 0) {
            seconds += std::hypot(timed[i].north - timed[i - 1].north, timed[i].east - timed[i - 1].east) / speed;
        }
        timed[i].seconds = seconds;
    }
    return path(origin, timed);
}

/// Lawnmower pattern of the lines across a square of the size north-east of the origin
QList<Point> survey(double size, int lines, double altitude)
{
    QList<Point> points;
    for (int i = 0; i < lines; i++) {
        const double east = (size * i) / (lines - 1);
        const bool up = (i % 2) == 0;
        points.append(Point{ up ? 0 : size, east, altitude });
        points.append(Point{ up ? size : 0, east, altitude });
    }
    return points;
}

/// @return Why the volumes don't contain the paths with the buffers every step meters, empty when they do: a volume
///         must contain each position with the circle of the horizontal buffer around it, the vertical buffer below
///         and above it, while the vehicle is estimated there within the time buffer
QString containmentError(const QList<Path> &paths, const QList<Intent::Volume> &volumes, const Intent::Parameters &parameters, double step = 25)
{
    const qint64 timeBuffer = qint64{ parameters.timeBuffer } * 1000;
    const auto contained = [&](const QGeoCoordinate &coordinate, double altitudeAMSL, qint64 time) {
        const double altitude = altitudeAMSL + GeoZoneGeometry::geoidHeight(coordinate);
        for (const Intent::Volume &volume : volumes) {
            if ((volume.timeStart.toMSecsSinceEpoch() > time - timeBuffer) || (volume.timeEnd.toMSecsSinceEpoch() < time + timeBuffer)) {
                continue;
            }
            if ((volume.altitudeLower > altitude - parameters.verticalBuffer + 1e-6) || (volume.altitudeUpper < altitude + parameters.verticalBuffer - 1e-6)) {
                continue;
            }
            bool inside = volume.contains(coordinate);
            for (int azimuth = 0; inside && (azimuth < 360); azimuth += 15) {
                inside = volume.contains(coordinate.atDistanceAndAzimuth(parameters.horizontalBuffer, azimuth));
            }
            if (inside) {
                return true;
            }
        }
        return false;
    };
    for (const Path &path : paths) {
        for (qsizetype i = 0; i < path.vertices.count(); i++) {
            const Vertex &to = path.vertices[i];
            const Vertex &from = path.vertices[(i > 0) ? (i - 1) : i];
            const double distance = from.coordinate.distanceTo(to.coordinate);
            const int steps = (std::max)(static_cast<int>(std::ceil(distance / step)), 1);
            for (int k = 0; k <= steps; k++) {
                const double fraction = static_cast<double>(k) / steps;
                const QGeoCoordinate coordinate = from.coordinate.atDistanceAndAzimuth(distance * fraction, from.coordinate.azimuthTo(to.coordinate));
                const double altitude = from.altitude + ((to.altitude - from.altitude) * fraction);
                const qint64 time = from.time.toMSecsSinceEpoch() + std::llround(static_cast<double>(from.time.msecsTo(to.time)) * fraction);
                if (!contained(coordinate, altitude, time)) {
                    return QStringLiteral("No volume contains %1 at %2 m AMSL at %3 with the buffers")
                        .arg(coordinate.toString(QGeoCoordinate::Degrees), QString::number(altitude), QDateTime::fromMSecsSinceEpoch(time, QTimeZone::UTC).toString(Qt::ISODate));
                }
            }
        }
    }
    return QString();
}

/// @return Why the volumes aren't valid ASTM F3548-21 volumes ordered by time, empty when they are
QString volumeError(const QList<Intent::Volume> &volumes)
{
    for (qsizetype i = 0; i < volumes.count(); i++) {
        const Intent::Volume &volume = volumes[i];
        if (!(volume.timeStart < volume.timeEnd) || !(volume.altitudeLower < volume.altitudeUpper)) {
            return QStringLiteral("Volume %1 has empty times or altitudes").arg(i);
        }
        if ((i > 0) && (volume.timeStart < volumes[i - 1].timeStart)) {
            return QStringLiteral("Volume %1 starts before the previous one").arg(i);
        }
        if (volume.isCircle()) {
            if (!(volume.circleRadius > 0) || !volume.outline.isEmpty()) {
                return QStringLiteral("Volume %1 has an invalid circle").arg(i);
            }
            continue;
        }
        if (volume.outline.count() < 3) {
            return QStringLiteral("Volume %1 has %2 vertices").arg(i).arg(volume.outline.count());
        }
        QSet<std::pair<double, double>> vertices;
        for (const QGeoCoordinate &coordinate : volume.outline) {
            if (!coordinate.isValid()) {
                return QStringLiteral("Volume %1 has an invalid vertex").arg(i);
            }
            vertices.insert({ coordinate.latitude(), coordinate.longitude() });
        }
        if (vertices.count() != volume.outline.count()) {
            return QStringLiteral("Volume %1 repeats vertices").arg(i);
        }
        const GeoZoneGeometry::LocalProjection projection(volume.outline.first());
        if (!GeoZoneGeometry::isSimple(projection.toLocal(volume.outline))) {
            return QStringLiteral("The outline of volume %1 crosses itself").arg(i);
        }
    }
    return QString();
}

} // namespace

void GeoZoneIntentTest::_testVolumes_data()
{
    QTest::addColumn<QList<Path>>("paths");
    QTest::addColumn<int>("maxVolumes");

    const QGeoCoordinate zurich(47.4, 8.5);
    QTest::newRow("straight") << QList<Path>{ flown(zurich, { { 0, 0 }, { 0, 3000 } }, 10) } << Intent::defaultMaxVolumes;
    QTest::newRow("turns") << QList<Path>{ flown(zurich, { { 0, 0 }, { 0, 1000 }, { 1000, 1000 }, { 1000, 2000 }, { -500, 2500 }, { 0, 0 } }, 12) }
                           << Intent::defaultMaxVolumes;
    QTest::newRow("survey") << QList<Path>{ flown(zurich, survey(300, 10, 520), 8) } << Intent::defaultMaxVolumes;
    QTest::newRow("climb and return") << QList<Path>{ path(zurich, { { 0, 0, 450, 0 }, { 0, 0, 570, 30 }, { 2000, 0, 570, 230 }, { 0, 0, 570, 430 }, { 0, 0, 450, 460 } }) }
                                      << Intent::defaultMaxVolumes;
    QTest::newRow("long legs") << QList<Path>{ flown(zurich, { { 0, 0 }, { 0, 60000 }, { 40000, 60000 } }, 25) } << Intent::defaultMaxVolumes;
    QTest::newRow("antimeridian") << QList<Path>{ flown(QGeoCoordinate(-17, 179.98), { { 0, 0 }, { 0, 5000 }, { 2000, 5000 } }, 10) } << Intent::defaultMaxVolumes;
    QTest::newRow("high latitude") << QList<Path>{ flown(QGeoCoordinate(69.6, 18.9), { { 0, 0 }, { 0, -20000 }, { 3000, -20000 }, { 3000, 0 } }, 20) } << 3;
    // A loiter path of its own while the main path waits at its item
    Path loiter = path(zurich, { { 1000, 0, 550, 100 }, { 1100, 100, 550, 160 }, { 1000, 200, 550, 220 }, { 900, 100, 550, 280 }, { 1000, 0, 550, 340 } });
    loiter.label = QStringLiteral("the loiter at item 2");
    QTest::newRow("loiter") << QList<Path>{ path(zurich, { { 0, 0, 500, 0 }, { 1000, 0, 550, 100 }, { 1000, 0, 550, 340 }, { 2000, 0, 500, 440 } }), loiter }
                            << Intent::defaultMaxVolumes;
    // Random legs merged into few volumes
    QList<Point> walk = { { 0, 0, 500 } };
    QRandomGenerator random(7);
    for (int i = 0; i < 300; i++) {
        const double azimuth = random.bounded(2 * M_PI);
        walk.append(Point{ walk.last().north + (500 * std::cos(azimuth)), walk.last().east + (500 * std::sin(azimuth)), 400 + random.bounded(300.0) });
    }
    QTest::newRow("random walk capped") << QList<Path>{ flown(zurich, walk, 15) } << 10;
    QTest::newRow("random walk single volume") << QList<Path>{ flown(zurich, walk, 15) } << 1;
}

void GeoZoneIntentTest::_testVolumes()
{
    QFETCH(QList<Path>, paths);
    QFETCH(int, maxVolumes);

    Intent::Parameters parameters;
    parameters.maxVolumes = maxVolumes;
    QString errorString;
    const std::optional<QList<Intent::Volume>> volumes = Intent::buildVolumes(paths, parameters, errorString);
    QVERIFY2(volumes, qPrintable(errorString));
    QVERIFY(!volumes->isEmpty());
    QVERIFY2(volumes->count() <= maxVolumes, qPrintable(QString::number(volumes->count())));
    const QString invalid = volumeError(*volumes);
    QVERIFY2(invalid.isEmpty(), qPrintable(invalid));
    const QString uncovered = containmentError(paths, *volumes, parameters);
    QVERIFY2(uncovered.isEmpty(), qPrintable(uncovered));

    // From the time buffer before the start to the time buffer after the end
    QDateTime end;
    for (const Path &path : std::as_const(paths)) {
        for (const Vertex &vertex : path.vertices) {
            end = (end.isValid() && (end > vertex.time)) ? end : vertex.time;
        }
    }
    Intent intent;
    intent.volumes = *volumes;
    QCOMPARE(intent.timeStart(), kStart.addSecs(-parameters.timeBuffer));
    QCOMPARE(intent.timeEnd(), QDateTime::fromSecsSinceEpoch((end.toMSecsSinceEpoch() + 999) / 1000, QTimeZone::UTC).addSecs(parameters.timeBuffer));
}

void GeoZoneIntentTest::_testVolumeCount()
{
    const QGeoCoordinate origin(47.4, 8.5);
    const Intent::Parameters parameters;
    const auto count = [&parameters](const QList<Path> &paths) {
        QString errorString;
        const std::optional<QList<Intent::Volume>> volumes = Intent::buildVolumes(paths, parameters, errorString);
        return volumes ? volumes->count() : -1;
    };

    // The positions every 50 m along a straight leg, like terrain samples, and the lines of a survey merge
    QList<Point> straight;
    for (int i = 0; i <= 10; i++) {
        straight.append(Point{ 0, 50.0 * i });
    }
    QCOMPARE(count({ flown(origin, straight, 10) }), 1);
    QCOMPARE(count({ flown(origin, survey(300, 10, 520), 8) }), 1);
    // A circle of a hovering vehicle
    QString errorString;
    const std::optional<QList<Intent::Volume>> hover = Intent::buildVolumes({ path(origin, { { 0, 0, 500, 0 }, { 0, 0, 520, 60 } }) }, parameters, errorString);
    QVERIFY2(hover, qPrintable(errorString));
    QCOMPARE(hover->count(), 1);
    QVERIFY(hover->first().isCircle());
    QCOMPARE(hover->first().circleRadius, parameters.horizontalBuffer * Intent::bufferMargin);

    // Long legs flown over hours aren't one volume reserving the whole corridor for the whole flight
    const qsizetype corridor = count({ flown(origin, { { 0, 0 }, { 0, 30000 }, { 30000, 30000 } }, 10) });
    QVERIFY2((corridor > 2) && (corridor <= parameters.maxVolumes), qPrintable(QString::number(corridor)));
    // Two surveys far apart in space and time aren't in one volume
    QList<Point> twoSurveys = survey(300, 6, 520);
    for (const Point &point : survey(300, 6, 520)) {
        twoSurveys.append(Point{ point.north + 20000, point.east, point.altitude });
    }
    const std::optional<QList<Intent::Volume>> surveys = Intent::buildVolumes({ flown(origin, twoSurveys, 10) }, parameters, errorString);
    QVERIFY2(surveys, qPrintable(errorString));
    for (const Intent::Volume &volume : std::as_const(*surveys)) {
        QVERIFY(!volume.contains(offset(origin, 150, 150)) || !volume.contains(offset(origin, 20150, 150)));
    }
}

void GeoZoneIntentTest::_testAltitudes()
{
    const QGeoCoordinate origin(47.4, 8.5);
    const double geoid = GeoZoneGeometry::geoidHeight(origin);
    QVERIFY((geoid > 40) && (geoid < 55));
    Intent::Parameters parameters;
    parameters.verticalBuffer = 25;
    QString errorString;

    // Relative and terrain altitudes above mean sea level, then above the ellipsoid
    Path relative = path(origin, { { 0, 0, 30, 0 } });
    relative.altitudeReference = GeoZonePathEvaluator::AltitudeReference::Relative;
    relative.referenceAltitudeAMSL = 400;
    std::optional<QList<Intent::Volume>> volumes = Intent::buildVolumes({ relative }, parameters, errorString);
    QVERIFY2(volumes, qPrintable(errorString));
    QCOMPARE(volumes->count(), 1);
    QCOMPARE(volumes->first().altitudeLower, std::floor(430 + geoid - 25));
    QCOMPARE(volumes->first().altitudeUpper, std::ceil(430 + geoid + 25));

    Path terrain = path(origin, { { 0, 0, 100, 0 } });
    terrain.altitudeReference = GeoZonePathEvaluator::AltitudeReference::Terrain;
    terrain.vertices[0].terrainAMSL = 300;
    volumes = Intent::buildVolumes({ terrain }, parameters, errorString);
    QVERIFY2(volumes, qPrintable(errorString));
    QCOMPARE(volumes->first().altitudeLower, std::floor(400 + geoid - 25));

    // Legs with an unknown altitude span the altitudes of the flight
    const Path unknown = path(origin, { { 0, 0, 500, 0 }, { 0, 5000, qQNaN(), 1000 }, { 0, 10000, 650, 2000 }, { 0, 20000, 650, 4000 } });
    volumes = Intent::buildVolumes({ unknown }, parameters, errorString);
    QVERIFY2(volumes, qPrintable(errorString));
    for (const Intent::Volume &volume : std::as_const(*volumes)) {
        if (volume.timeStart < kStart.addSecs(2000 - parameters.timeBuffer)) {
            QVERIFY(volume.altitudeLower <= 500 + geoid - 25);
            QVERIFY(volume.altitudeUpper >= 650 + geoid + 25);
        }
    }
}

void GeoZoneIntentTest::_testErrors()
{
    const QGeoCoordinate origin(47.4, 8.5);
    const Intent::Parameters parameters;
    QString errorString;
    QVERIFY(!Intent::buildVolumes({}, parameters, errorString));
    QVERIFY(!errorString.isEmpty());

    errorString.clear();
    Path invalid;
    invalid.vertices.append(Vertex{ QGeoCoordinate(), 500, qQNaN(), kStart });
    QVERIFY(!Intent::buildVolumes({ invalid }, parameters, errorString));
    QVERIFY(!errorString.isEmpty());

    // Without times, like without a planned start
    errorString.clear();
    Path untimed = path(origin, { { 0, 0 }, { 0, 1000 } });
    untimed.vertices[1].time = QDateTime();
    QVERIFY(!Intent::buildVolumes({ untimed }, parameters, errorString));
    QVERIFY(errorString.contains(QStringLiteral("planned start")));

    errorString.clear();
    QVERIFY(!Intent::buildVolumes({ path(origin, { { 0, 0, qQNaN(), 0 }, { 0, 1000, qQNaN(), 100 } }) }, parameters, errorString));
    QVERIFY(!errorString.isEmpty());
}

void GeoZoneIntentTest::_testJson()
{
    const QGeoCoordinate origin(47.4, 8.5);
    Intent intent;
    intent.flight.name = QStringLiteral("Survey");
    intent.flight.planFile = QStringLiteral("/plans/Survey.plan");
    intent.flight.plannedStart = kStart;
    intent.flight.plannedEnd = kStart.addSecs(600);
    intent.flight.distance = 3000;
    intent.generatedAt = kStart.addDays(-1);
    intent.uSpaceAirspaces.append(Intent::Airspace{ QStringLiteral("CH:U1"), QStringLiteral("U1"), QStringLiteral("CHE"), QStringLiteral("Zurich U-space") });
    intent.authorisations.append(GeoZoneAuthorisation{ QStringLiteral("CHE:U1"), QStringLiteral("Zurich U-space"), QStringLiteral("FA-2026-0042"), kStart.addDays(-2), kStart.addDays(1) });
    QString errorString;
    const std::optional<QList<Intent::Volume>> volumes = Intent::buildVolumes({ flown(origin, { { 0, 0 }, { 0, 500 } }, 5), path(origin, { { 2000, 0, 500, 900 } }) },
                                                                              intent.parameters, errorString);
    QVERIFY2(volumes, qPrintable(errorString));
    intent.volumes = *volumes;
    QCOMPARE(intent.volumes.count(), 2);

    const QJsonObject json = QJsonDocument::fromJson(QJsonDocument(intent.toJson()).toJson()).object();
    QCOMPARE(json[QStringLiteral("schema")].toString(), QStringLiteral("qgc-operational-intent"));
    QCOMPARE(json[QStringLiteral("version")].toInt(), Intent::jsonVersion);
    QCOMPARE(json[QStringLiteral("standard")].toString(), QStringLiteral("ASTM F3548-21"));
    QCOMPARE(json[QStringLiteral("flight")].toObject()[QStringLiteral("plannedStart")].toString(), QStringLiteral("2026-06-01T10:00:00Z"));
    QCOMPARE(json[QStringLiteral("parameters")].toObject()[QStringLiteral("horizontalBufferMeters")].toDouble(), Intent::defaultHorizontalBuffer);
    QCOMPARE(json[QStringLiteral("uSpaceAirspaces")].toArray()[0].toObject()[QStringLiteral("identifier")].toString(), QStringLiteral("U1"));
    QCOMPARE(json[QStringLiteral("authorisations")].toArray()[0].toObject()[QStringLiteral("permit")].toString(), QStringLiteral("FA-2026-0042"));

    // The field names and enumerations of the ASTM F3548-21 schemas, utm.yaml
    const QRegularExpression rfc3339(QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d(\\.\\d+)?Z$"));
    const auto verifyTime = [&rfc3339](const QJsonValue &value) {
        const QJsonObject time = value.toObject();
        QCOMPARE(time.keys(), (QStringList{ QStringLiteral("format"), QStringLiteral("value") }));
        QCOMPARE(time[QStringLiteral("format")].toString(), QStringLiteral("RFC3339"));
        QVERIFY2(rfc3339.match(time[QStringLiteral("value")].toString()).hasMatch(), qPrintable(time[QStringLiteral("value")].toString()));
    };
    const auto verifyAltitude = [](const QJsonValue &value) {
        const QJsonObject altitude = value.toObject();
        QCOMPARE(altitude.keys(), (QStringList{ QStringLiteral("reference"), QStringLiteral("units"), QStringLiteral("value") }));
        QCOMPARE(altitude[QStringLiteral("reference")].toString(), QStringLiteral("W84"));
        QCOMPARE(altitude[QStringLiteral("units")].toString(), QStringLiteral("M"));
        QVERIFY(altitude[QStringLiteral("value")].isDouble());
    };
    const auto verifyLatLng = [](const QJsonValue &value) {
        const QJsonObject point = value.toObject();
        QCOMPARE(point.keys(), (QStringList{ QStringLiteral("lat"), QStringLiteral("lng") }));
        QVERIFY(std::abs(point[QStringLiteral("lat")].toDouble()) <= 90);
        QVERIFY(std::abs(point[QStringLiteral("lng")].toDouble()) <= 180);
    };

    const QJsonObject operationalIntent = json[QStringLiteral("operational_intent")].toObject();
    QCOMPARE(operationalIntent.keys(), (QStringList{ QStringLiteral("details"), QStringLiteral("reference") }));
    const QJsonObject reference = operationalIntent[QStringLiteral("reference")].toObject();
    QCOMPARE(reference[QStringLiteral("state")].toString(), QStringLiteral("Accepted"));
    verifyTime(reference[QStringLiteral("time_start")]);
    verifyTime(reference[QStringLiteral("time_end")]);
    QCOMPARE(reference[QStringLiteral("time_start")].toObject()[QStringLiteral("value")].toString(), QStringLiteral("2026-06-01T09:45:00Z"));

    const QJsonObject details = operationalIntent[QStringLiteral("details")].toObject();
    QCOMPARE(details.keys(), (QStringList{ QStringLiteral("off_nominal_volumes"), QStringLiteral("priority"), QStringLiteral("volumes") }));
    QCOMPARE(details[QStringLiteral("off_nominal_volumes")].toArray().count(), 0);
    QCOMPARE(details[QStringLiteral("priority")].toInt(-1), 0);
    const QJsonArray volumesJson = details[QStringLiteral("volumes")].toArray();
    QCOMPARE(volumesJson.count(), 2);
    int polygons = 0;
    int circles = 0;
    for (const QJsonValue &value : volumesJson) {
        const QJsonObject volume4D = value.toObject();
        QCOMPARE(volume4D.keys(), (QStringList{ QStringLiteral("time_end"), QStringLiteral("time_start"), QStringLiteral("volume") }));
        verifyTime(volume4D[QStringLiteral("time_start")]);
        verifyTime(volume4D[QStringLiteral("time_end")]);
        const QJsonObject volume3D = volume4D[QStringLiteral("volume")].toObject();
        verifyAltitude(volume3D[QStringLiteral("altitude_lower")]);
        verifyAltitude(volume3D[QStringLiteral("altitude_upper")]);
        QVERIFY(volume3D[QStringLiteral("altitude_lower")].toObject()[QStringLiteral("value")].toDouble() <
                volume3D[QStringLiteral("altitude_upper")].toObject()[QStringLiteral("value")].toDouble());
        // Exactly one outline
        QCOMPARE(volume3D.count(), 3);
        if (volume3D.contains(QStringLiteral("outline_polygon"))) {
            polygons++;
            const QJsonObject polygon = volume3D[QStringLiteral("outline_polygon")].toObject();
            QCOMPARE(polygon.keys(), QStringList{ QStringLiteral("vertices") });
            const QJsonArray vertices = polygon[QStringLiteral("vertices")].toArray();
            QVERIFY(vertices.count() >= 3);
            QVERIFY(vertices.first() != vertices.last());
            for (const QJsonValue &vertex : vertices) {
                verifyLatLng(vertex);
            }
        } else {
            circles++;
            const QJsonObject circle = volume3D[QStringLiteral("outline_circle")].toObject();
            QCOMPARE(circle.keys(), (QStringList{ QStringLiteral("center"), QStringLiteral("radius") }));
            verifyLatLng(circle[QStringLiteral("center")]);
            const QJsonObject radius = circle[QStringLiteral("radius")].toObject();
            QCOMPARE(radius.keys(), (QStringList{ QStringLiteral("units"), QStringLiteral("value") }));
            QCOMPARE(radius[QStringLiteral("units")].toString(), QStringLiteral("M"));
            QCOMPARE(radius[QStringLiteral("value")].toDouble(), Intent::defaultHorizontalBuffer * Intent::bufferMargin);
        }
    }
    QCOMPARE(polygons, 1);
    QCOMPARE(circles, 1);
}
