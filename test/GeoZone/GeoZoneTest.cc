/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneTest.h"
#include "GeoFenceController.h"
#include "GeoZone.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneManager.h"
#include "GeoZoneMapModel.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMonitor.h"
#include "GeoZoneParser.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "UnitsSettings.h"
#include "Vehicle.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QScopeGuard>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimeZone>
#include <QtCore/QUrl>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

GeoZoneParser::Result _parseResource(const QString &name)
{
    GeoZoneParser::Result result;
    QString errorString;
    if (!GeoZoneParser::parseFile(QStringLiteral(":/unittest/%1").arg(name), result, errorString)) {
        qWarning() << "Parse failed" << name << errorString;
    }
    return result;
}

QDateTime _utc(const QDate &date, const QTime &time)
{
    return QDateTime(date, time, QTimeZone::UTC);
}

QJsonObject _circleFeature(const QString &identifier, const QString &restriction, const QGeoCoordinate &center, double radius)
{
    return QJsonObject{
        { "identifier", identifier },
        { "restriction", restriction },
        { "geometry", QJsonArray{ QJsonObject{ { "horizontalProjection", QJsonObject{
            { "type", "Circle" }, { "center", QJsonArray{ center.longitude(), center.latitude() } }, { "radius", radius } } } } } },
    };
}

QJsonObject _polygonFeature(const QString &identifier, const QString &restriction, const QList<QGeoCoordinate> &ring)
{
    QJsonArray positions;
    for (const QGeoCoordinate &coordinate : ring) {
        positions.append(QJsonArray{ coordinate.longitude(), coordinate.latitude() });
    }
    positions.append(positions.first());
    return QJsonObject{
        { "identifier", identifier },
        { "restriction", restriction },
        { "geometry", QJsonArray{ QJsonObject{ { "horizontalProjection", QJsonObject{
            { "type", "Polygon" }, { "coordinates", QJsonArray{ positions } } } } } } },
    };
}

/// Writes an ED-269 file with the features to the directory
QString _writeFeatures(const QTemporaryDir &dir, const QString &name, const QJsonArray &features)
{
    const QString fileName = dir.filePath(name);
    QFile file(fileName);
    if (file.open(QIODevice::WriteOnly)) {
        (void) file.write(QJsonDocument(QJsonObject{ { "features", features } }).toJson(QJsonDocument::Compact));
    }
    return fileName;
}

QList<QGeoCoordinate> _circleRing(const QGeoCoordinate &center, double radius, int vertexCount)
{
    QList<QGeoCoordinate> ring;
    for (int i = 0; i < vertexCount; i++) {
        ring.append(center.atDistanceAndAzimuth(radius, i * 360.0 / vertexCount));
    }
    return ring;
}

double _distanceToRing(const QPointF &point, const QList<QGeoCoordinate> &ring)
{
    double minDistance = std::numeric_limits<double>::max();
    for (qsizetype i = 0; i < ring.count(); i++) {
        const QPointF a = GeoZoneShape::toMercator(ring[i]);
        const QPointF b = GeoZoneShape::toMercator(ring[(i + 1) % ring.count()]);
        const QPointF ab = b - a;
        const double lengthSquared = QPointF::dotProduct(ab, ab);
        const double t = (lengthSquared > 0) ? std::clamp(QPointF::dotProduct(point - a, ab) / lengthSquared, 0.0, 1.0) : 0.0;
        const QPointF offset = a + (t * ab) - point;
        minDistance = qMin(minDistance, std::hypot(offset.x(), offset.y()));
    }
    return minDistance;
}

} // namespace

void GeoZoneTest::_testParseED269()
{
    GeoZoneParser::Result result;
    QString errorString;
    // File starts with a UTF-8 byte order mark like several national feeds
    QVERIFY2(GeoZoneParser::parseFile(QStringLiteral(":/unittest/ed269_test.json"), result, errorString), qPrintable(errorString));
    QCOMPARE(result.format, GeoZoneParser::Format::ED269);
    QCOMPARE(result.title, QStringLiteral("Test UASZoneVersion"));
    QCOMPARE(result.zones.count(), 3);
    QCOMPARE(result.skippedFeatures, 1);

    const GeoZone::Data &prohibited = result.zones[0];
    QCOMPARE(prohibited.identifier, QStringLiteral("T-PROH"));
    QCOMPARE(prohibited.restriction, GeoZone::Prohibited);
    QCOMPARE(prohibited.reasons, QStringList({ QStringLiteral("AIR_TRAFFIC"), QStringLiteral("SENSITIVE") }));
    QCOMPARE(prohibited.volumes.count(), 2);
    const GeoZone::Volume &polygon = prohibited.volumes[0];
    QVERIFY(!polygon.isCircle);
    QCOMPARE(polygon.rings.count(), 1);
    QCOMPARE(polygon.rings[0].count(), 4); // Closing vertex removed
    QCOMPARE(polygon.rings[0][1], QGeoCoordinate(47.0, 8.1)); // [lon, lat] order
    QCOMPARE(polygon.upperLimit, 120.0);
    QCOMPARE(polygon.upperReference, QStringLiteral("AGL"));
    QCOMPARE(prohibited.authorities.count(), 1);
    QVERIFY(prohibited.authorities[0].contains(QStringLiteral("Test Authority")));
    QVERIFY(prohibited.authorities[0].contains(QStringLiteral("zones@example.com")));

    const GeoZone::Data &authorisation = result.zones[1];
    QCOMPARE(authorisation.restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(authorisation.volumes.count(), 1);
    const GeoZone::Volume &circle = authorisation.volumes[0];
    QVERIFY(circle.isCircle);
    QVERIFY(circle.feet);
    QCOMPARE(circle.center, QGeoCoordinate(47.5, 8.5));
    QCOMPARE(circle.radius, 1000 * 0.3048); // ED-269 radius uses uomDimensions
    QCOMPARE(circle.upperLimit, 400.0);

    const GeoZone::Data &scheduled = result.zones[2];
    QCOMPARE(scheduled.restriction, GeoZone::Conditional);
    QVERIFY(qIsNaN(scheduled.volumes[0].lowerLimit));
    QVERIFY(qIsNaN(scheduled.volumes[0].upperLimit));
    QCOMPARE(scheduled.applicability.count(), 1);
    const GeoZone::TimePeriod &period = scheduled.applicability[0];
    QVERIFY(!period.permanent);
    QCOMPARE(period.start, _utc(QDate(2026, 3, 29), QTime(1, 0)));
    QCOMPARE(period.end, _utc(QDate(2026, 10, 25), QTime(0, 59, 59)));
    QCOMPARE(period.schedule.count(), 1);
    QCOMPARE(period.schedule[0].days, QList<int>({ Qt::Monday, Qt::Tuesday }));
    QCOMPARE(period.schedule[0].startTime, QTime(5, 0));
    QCOMPARE(period.schedule[0].endTime, QTime(18, 0));
}

void GeoZoneTest::_testParseED318()
{
    const QLocale savedLocale;
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedKingdom));
    GeoZoneParser::Result result;
    QString errorString;
    const bool parsed = GeoZoneParser::parseFile(QStringLiteral(":/unittest/ed318_test.json"), result, errorString);
    QLocale::setDefault(QLocale(QLocale::Swedish, QLocale::Sweden));
    const GeoZoneParser::Result swedishResult = _parseResource(QStringLiteral("ed318_test.json"));
    QLocale::setDefault(savedLocale);

    QVERIFY2(parsed, qPrintable(errorString));
    QCOMPARE(result.format, GeoZoneParser::Format::ED318);
    QCOMPARE(result.provider, QStringLiteral("Test Authority"));
    QCOMPARE(result.issued, _utc(QDate(2026, 8, 13), QTime(0, 0)));
    QCOMPARE(result.validFrom, _utc(QDate(2026, 8, 13), QTime(0, 0)));
    QCOMPARE(result.validTo, _utc(QDate(2099, 1, 1), QTime(0, 0)));
    QCOMPARE(result.title, QStringLiteral("Test GeoZones"));
    QCOMPARE(result.zones.count(), 3);
    QCOMPARE(result.skippedFeatures, 1); // LineString

    // Point with circle extent, localized name
    const GeoZone::Data &circleZone = result.zones[0];
    QCOMPARE(circleZone.identifier, QStringLiteral("S-CIRC"));
    QCOMPARE(circleZone.name, QStringLiteral("Example power plant"));
    QCOMPARE(swedishResult.zones[0].name, QStringLiteral("Exempel kraftverk"));
    QCOMPARE(circleZone.restriction, GeoZone::Prohibited);
    QCOMPARE(circleZone.volumes.count(), 1);
    const GeoZone::Volume &circle = circleZone.volumes[0];
    QVERIFY(circle.isCircle);
    QCOMPARE(circle.center, QGeoCoordinate(58.5, 15.5));
    QCOMPARE(circle.radius, 500.0); // ED-318 radius is always meters, uom only applies to the layer
    QVERIFY(circle.feet);
    QCOMPARE(circle.upperLimit, 1500.0);
    QCOMPARE(circle.upperReference, QStringLiteral("AMSL"));
    QCOMPARE(circleZone.applicability.count(), 1);
    QCOMPARE(circleZone.applicability[0].schedule.count(), 1);
    QVERIFY(circleZone.applicability[0].schedule[0].days.isEmpty()); // ANY
    QVERIFY(!circleZone.applicability[0].schedule[0].startTime.isValid());
    QCOMPARE(circleZone.applicability[0].schedule[0].startEvent, QStringLiteral("SR"));

    // MultiPolygon with a hole, plain string text fields and nulls
    const GeoZone::Data &multiPolygon = result.zones[1];
    QCOMPARE(multiPolygon.restriction, GeoZone::ReqAuthorisation); // American spelling
    QCOMPARE(multiPolygon.name, QStringLiteral("Control zone"));
    QCOMPARE(multiPolygon.reasons, QStringList(QStringLiteral("AIR_TRAFFIC")));
    QVERIFY(multiPolygon.message.isEmpty());
    QVERIFY(multiPolygon.restrictionConditions.isEmpty());
    QCOMPARE(multiPolygon.volumes.count(), 2);
    QCOMPARE(multiPolygon.volumes[0].rings.count(), 2);
    QCOMPARE(multiPolygon.volumes[0].rings[1].count(), 4);
    QCOMPARE(multiPolygon.volumes[1].rings.count(), 1);
    QCOMPARE(multiPolygon.volumes[1].rings[0].count(), 3);
    QCOMPARE(multiPolygon.volumes[1].upperLimit, 2500.0);

    // GeometryCollection where each geometry has its own layer
    const GeoZone::Data &collection = result.zones[2];
    QCOMPARE(collection.identifier, QStringLiteral("F-GC")); // Feature id fallback
    QCOMPARE(collection.restriction, GeoZone::USpace);
    QCOMPARE(collection.volumes.count(), 2);
    QCOMPARE(collection.volumes[0].lowerLimit, 50.0);
    QCOMPARE(collection.volumes[0].upperLimit, 150.0);
    QCOMPARE(collection.volumes[1].lowerLimit, 0.0);
    QCOMPARE(collection.volumes[1].upperLimit, 50.0);
    QVERIFY(!collection.volumes[1].feet);
}

void GeoZoneTest::_testParseErrors()
{
    GeoZoneParser::Result result;
    QString errorString;

    QVERIFY(!GeoZoneParser::parseFile(QStringLiteral("/nonexistent/geozones.json"), result, errorString));
    QVERIFY(!errorString.isEmpty());

    QVERIFY(!GeoZoneParser::parse("{ \"features\": [", result, errorString));
    QVERIFY(!errorString.isEmpty());

    QVERIFY(!GeoZoneParser::parse(R"({ "type": "Polygon", "coordinates": [] })", result, errorString));
    QVERIFY(!errorString.isEmpty());

    QVERIFY(!GeoZoneParser::parse(R"({ "features": [ { "identifier": "X", "restriction": "PROHIBITED", "geometry": [] } ] })", result, errorString));
    QCOMPARE(result.skippedFeatures, 1);

    // Latitude out of range
    QVERIFY(!GeoZoneParser::parse(R"({ "type": "FeatureCollection", "features": [ { "type": "Feature", "properties": { "type": "PROHIBITED" },
        "geometry": { "type": "Polygon", "coordinates": [ [ [0, 95], [1, 95], [1, 96], [0, 95] ] ] } } ] })", result, errorString));
    QCOMPARE(result.skippedFeatures, 1);
}

void GeoZoneTest::_testIsActiveAt()
{
    const GeoZoneParser::Result ed269 = _parseResource(QStringLiteral("ed269_test.json"));
    const GeoZoneParser::Result ed318 = _parseResource(QStringLiteral("ed318_test.json"));
    QCOMPARE(ed269.zones.count(), 3);
    QCOMPARE(ed318.zones.count(), 3);

    const GeoZone permanent(ed269.zones[0]);
    QVERIFY(permanent.isActiveAt(_utc(QDate(2035, 1, 1), QTime(0, 0))));

    // Mondays and Tuesdays 05:00-18:00 UTC between 2026-03-29 and 2026-10-25
    const GeoZone scheduled(ed269.zones[2]);
    const QDate monday(2026, 6, 1);
    QVERIFY(scheduled.isActiveAt(_utc(monday, QTime(10, 0))));
    QVERIFY(scheduled.isActiveAt(QDateTime(monday, QTime(12, 0), QTimeZone::fromSecondsAheadOfUtc(2 * 3600))));
    QVERIFY(!scheduled.isActiveAt(_utc(monday, QTime(4, 59))));
    QVERIFY(!scheduled.isActiveAt(_utc(monday, QTime(18, 1))));
    QVERIFY(!scheduled.isActiveAt(_utc(monday.addDays(2), QTime(10, 0))));
    QVERIFY(!scheduled.isActiveAt(_utc(QDate(2026, 11, 2), QTime(10, 0))));

    // Daylight events cannot be evaluated, so they apply all day within the date range
    const GeoZone daylight(ed318.zones[0]);
    QVERIFY(daylight.isActiveAt(_utc(QDate(2026, 6, 1), QTime(23, 30))));
    QVERIFY(!daylight.isActiveAt(_utc(QDate(2027, 1, 2), QTime(12, 0))));

    // Schedules wrapping past midnight continue into the morning after the listed day
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(R"({ "features": [ { "identifier": "N", "restriction": "PROHIBITED",
        "applicability": [ { "permanent": "NO", "schedule": [ { "day": ["FRI"], "startTime": "22:00Z", "endTime": "02:00Z" } ] } ],
        "geometry": [ { "horizontalProjection": { "type": "Circle", "center": [8, 47], "radius": 100 } } ] } ] })", result, errorString), qPrintable(errorString));
    const GeoZone night(result.zones[0]);
    const QDate friday(2026, 6, 5);
    QVERIFY(night.isActiveAt(_utc(friday, QTime(23, 0))));
    QVERIFY(night.isActiveAt(_utc(friday.addDays(1), QTime(1, 0))));
    QVERIFY(!night.isActiveAt(_utc(friday, QTime(1, 0))));
    QVERIFY(!night.isActiveAt(_utc(friday.addDays(1), QTime(23, 0))));

    // Days and times with a UTC offset are evaluated in that offset: Monday 01:00-03:00 UTC+2 is Sunday 23:00 to Monday 01:00 UTC
    QVERIFY2(GeoZoneParser::parse(R"({ "features": [ { "identifier": "O", "restriction": "PROHIBITED",
        "applicability": [ { "permanent": "NO", "schedule": [ { "day": ["MON"], "startTime": "01:00+02:00", "endTime": "01:00Z" } ] } ],
        "geometry": [ { "horizontalProjection": { "type": "Circle", "center": [8, 47], "radius": 100 } } ] } ] })", result, errorString), qPrintable(errorString));
    const GeoZone::DailyPeriod &offsetPeriod = result.zones[0].applicability[0].schedule[0];
    QCOMPARE(offsetPeriod.utcOffsetSecs, 2 * 3600);
    QCOMPARE(offsetPeriod.startTime, QTime(1, 0));
    QCOMPARE(offsetPeriod.endTime, QTime(3, 0));
    const GeoZone offset(result.zones[0]);
    QVERIFY(offset.isActiveAt(_utc(monday.addDays(-1), QTime(23, 30))));
    QVERIFY(offset.isActiveAt(_utc(monday, QTime(0, 30))));
    QVERIFY(!offset.isActiveAt(_utc(monday, QTime(1, 30))));
    QVERIFY(!offset.isActiveAt(_utc(monday, QTime(23, 30))));
    QCOMPARE(offset.applicabilityText(), QStringLiteral("  MON 01:00-03:00 UTC+02:00"));
}

void GeoZoneTest::_testIsActiveDuring()
{
    const GeoZoneParser::Result ed269 = _parseResource(QStringLiteral("ed269_test.json"));
    QCOMPARE(ed269.zones.count(), 3);

    const GeoZone permanent(ed269.zones[0]);
    QVERIFY(permanent.isActiveDuring(_utc(QDate(2035, 1, 1), QTime(0, 0)), _utc(QDate(2035, 1, 1), QTime(1, 0))));

    // Mondays and Tuesdays 05:00-18:00 UTC between 2026-03-29 and 2026-10-25
    const GeoZone scheduled(ed269.zones[2]);
    const QDate monday(2026, 6, 1);
    QVERIFY(!scheduled.isActiveDuring(_utc(monday, QTime(1, 0)), _utc(monday, QTime(4, 59))));
    QVERIFY(scheduled.isActiveDuring(_utc(monday, QTime(4, 0)), _utc(monday, QTime(5, 30))));
    QVERIFY(scheduled.isActiveDuring(_utc(monday, QTime(17, 30)), _utc(monday, QTime(20, 0))));
    QVERIFY(!scheduled.isActiveDuring(_utc(monday.addDays(1), QTime(18, 1)), _utc(monday.addDays(5), QTime(23, 0))));
    QVERIFY(scheduled.isActiveDuring(_utc(monday.addDays(2), QTime(0, 0)), _utc(monday.addDays(30), QTime(0, 0))));
    QVERIFY(!scheduled.isActiveDuring(_utc(monday, QTime(20, 0)), _utc(monday, QTime(10, 0))));
    // A window starting before the validity begins only overlaps the scheduled days inside it
    QVERIFY(!scheduled.isActiveDuring(_utc(QDate(2026, 3, 25), QTime(0, 0)), _utc(QDate(2026, 3, 29), QTime(23, 0))));
    QVERIFY(scheduled.isActiveDuring(_utc(QDate(2026, 3, 25), QTime(0, 0)), _utc(QDate(2026, 3, 30), QTime(6, 0))));
    QVERIFY(!scheduled.isActiveDuring(_utc(QDate(2026, 10, 26), QTime(0, 0)), _utc(QDate(2026, 12, 1), QTime(0, 0))));

    // Friday 22:00 to Saturday 02:00 UTC, and Monday 01:00-03:00 UTC+2 which is Sunday 23:00 to Monday 01:00 UTC
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(R"({ "features": [ { "identifier": "N", "restriction": "PROHIBITED",
        "applicability": [ { "permanent": "NO", "schedule": [ { "day": ["FRI"], "startTime": "22:00Z", "endTime": "02:00Z" },
                                                               { "day": ["MON"], "startTime": "01:00+02:00", "endTime": "01:00Z" } ] } ],
        "geometry": [ { "horizontalProjection": { "type": "Circle", "center": [8, 47], "radius": 100 } } ] } ] })", result, errorString), qPrintable(errorString));
    const GeoZone night(result.zones[0]);
    const QDate friday(2026, 6, 5);
    QVERIFY(!night.isActiveDuring(_utc(friday, QTime(19, 0)), _utc(friday, QTime(21, 59))));
    QVERIFY(night.isActiveDuring(_utc(friday, QTime(21, 0)), _utc(friday, QTime(22, 30))));
    QVERIFY(night.isActiveDuring(_utc(friday.addDays(1), QTime(1, 0)), _utc(friday.addDays(1), QTime(3, 0))));
    QVERIFY(!night.isActiveDuring(_utc(friday.addDays(1), QTime(2, 1)), _utc(friday.addDays(2), QTime(22, 59))));
    QVERIFY(night.isActiveDuring(_utc(friday.addDays(2), QTime(22, 30)), _utc(friday.addDays(2), QTime(23, 10))));
    QVERIFY(!night.isActiveDuring(_utc(friday.addDays(3), QTime(1, 1)), _utc(friday.addDays(6), QTime(21, 0))));

    // The manager filters zones by the activity window
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QVERIFY2(manager.loadFile(QStringLiteral(":/unittest/ed269_test.json"), errorString), qPrintable(errorString));
    const QList<GeoZone::Restriction> all = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::NoRestriction, GeoZone::USpace, GeoZone::Unknown };
    QCOMPARE(manager.zonesInArea(all, QGeoRectangle()).count(), 3);
    QCOMPARE(manager.zonesInArea(all, QGeoRectangle(), _utc(monday.addDays(2), QTime(10, 0))).count(), 2);
    QCOMPARE(manager.zonesInArea(all, QGeoRectangle(), _utc(monday.addDays(2), QTime(10, 0)), _utc(monday.addDays(7), QTime(10, 0))).count(), 3);
}

void GeoZoneTest::_testOutwardSimplification()
{
    const auto verifyEncloses = [](const QList<QGeoCoordinate> &original, const QList<QGeoCoordinate> &simplified, double tolerance) {
        const GeoZoneGeometry::LocalProjection projection(original.first());
        const QList<QPointF> originalPoints = projection.toLocal(original);
        const QList<QPointF> simplifiedPoints = projection.toLocal(simplified);
        QVERIFY(GeoZoneGeometry::isSimple(simplifiedPoints));
        for (qsizetype i = 0; i < originalPoints.count(); i++) {
            QVERIFY(GeoZoneGeometry::ringContains(simplifiedPoints, originalPoints[i]));
            QVERIFY(!GeoZoneGeometry::segmentIntersectsRing(originalPoints[i], originalPoints[(i + 1) % originalPoints.count()], simplifiedPoints));
        }
        // Expanded by no more than twice the tolerance
        for (const QPointF &point : simplifiedPoints) {
            QVERIFY(GeoZoneGeometry::distanceToRing(point, originalPoints) <= (2 * tolerance) + 1);
        }
    };

    // Circle published as a polygon
    const QList<QGeoCoordinate> circle = _circleRing(QGeoCoordinate(47, 8), 500, 64);
    const QList<QGeoCoordinate> simplifiedCircle = GeoZoneGeometry::simplifyOutward(circle, 25);
    QVERIFY(simplifiedCircle.count() < 20);
    verifyEncloses(circle, simplifiedCircle, 25);

    // Concave star with a jagged boundary
    QList<QGeoCoordinate> star;
    for (int i = 0; i < 200; i++) {
        const double radius = ((i % 2) ? 400 : 395) + ((i % 40) < 20 ? 300 : 0);
        star.append(QGeoCoordinate(47, 8).atDistanceAndAzimuth(radius, i * 360.0 / 200));
    }
    const QList<QGeoCoordinate> simplifiedStar = GeoZoneGeometry::simplifyOutward(star, 10);
    QVERIFY(simplifiedStar.count() < star.count() / 2);
    verifyEncloses(star, simplifiedStar, 10);

    // U shape with a notch narrower than the tolerance and sharp corners
    const QGeoCoordinate origin(47, 8);
    QList<QGeoCoordinate> notch;
    const QList<QPointF> notchPoints = { { 0, 0 }, { 1000, 0 }, { 1000, 1000 }, { 510, 1000 }, { 510, 100 }, { 500, 100 }, { 500, 1000 }, { 0, 1000 } };
    const GeoZoneGeometry::LocalProjection projection(origin);
    for (const QPointF &point : notchPoints) {
        // Densify the edges so there is something to simplify
        notch.append(projection.toGeo(point));
    }
    QList<QGeoCoordinate> denseNotch;
    for (qsizetype i = 0; i < notch.count(); i++) {
        const QPointF a = projection.toLocal(notch[i]);
        const QPointF b = projection.toLocal(notch[(i + 1) % notch.count()]);
        for (int step = 0; step < 10; step++) {
            denseNotch.append(projection.toGeo(a + ((b - a) * (step / 10.0)) + QPointF((step % 2) * 2, 0)));
        }
    }
    const QList<QGeoCoordinate> simplifiedNotch = GeoZoneGeometry::simplifyOutward(denseNotch, 25);
    QVERIFY(simplifiedNotch.count() < denseNotch.count());
    verifyEncloses(denseNotch, simplifiedNotch, 25);

    // Nothing to do
    QCOMPARE(GeoZoneGeometry::simplifyOutward(circle, 0), circle);
    const QList<QGeoCoordinate> triangle = { QGeoCoordinate(47, 8), QGeoCoordinate(47.01, 8), QGeoCoordinate(47, 8.01) };
    QCOMPARE(GeoZoneGeometry::simplifyOutward(triangle, 25), triangle);
}

void GeoZoneTest::_testDisplayText()
{
    const GeoZoneParser::Result ed269 = _parseResource(QStringLiteral("ed269_test.json"));
    QCOMPARE(ed269.zones.count(), 3);

    Fact *const verticalUnits = SettingsManager::instance()->unitsSettings()->verticalDistanceUnits();
    const QVariant savedVerticalUnits = verticalUnits->rawValue();
    const auto restoreVerticalUnits = qScopeGuard([verticalUnits, savedVerticalUnits] { verticalUnits->setRawValue(savedVerticalUnits); });
    verticalUnits->setRawValue(UnitsSettings::VerticalDistanceUnitsMeters);

    const GeoZone prohibited(ed269.zones[0]);
    QCOMPARE(prohibited.verticalLimitsText(), QStringLiteral("GND - 120 m AGL\n120 m AGL - Unlimited"));
    QCOMPARE(prohibited.applicabilityText(), QStringLiteral("Permanent"));
    QCOMPARE(prohibited.reasonText(), QStringLiteral("Air traffic, Sensitive area"));

    // Limits are shown in the user's units with the published value when it differs
    const GeoZone circle(ed269.zones[1]);
    QCOMPARE(circle.verticalLimitsText(), QStringLiteral("GND - 122 m AGL (400 ft)"));

    verticalUnits->setRawValue(UnitsSettings::VerticalDistanceUnitsFeet);
    QCOMPARE(prohibited.verticalLimitsText(), QStringLiteral("GND - 394 ft AGL (120 m)\n394 ft AGL (120 m) - Unlimited"));
    QCOMPARE(circle.verticalLimitsText(), QStringLiteral("GND - 400 ft AGL"));

    const GeoZone scheduled(ed269.zones[2]);
    QCOMPARE(scheduled.verticalLimitsText(), QStringLiteral("Not specified"));
    QCOMPARE(scheduled.applicabilityText(), QStringLiteral("From 2026-03-29 01:00 UTC to 2026-10-25 00:59 UTC\n  MON, TUE 05:00-18:00 UTC"));
}

void GeoZoneTest::_testExclusionShapes()
{
    const GeoZoneParser::Result ed269 = _parseResource(QStringLiteral("ed269_test.json"));
    const GeoZoneParser::Result ed318 = _parseResource(QStringLiteral("ed318_test.json"));
    QCOMPARE(ed269.zones.count(), 3);
    QCOMPARE(ed318.zones.count(), 3);

    // Both volumes share the footprint, and the counter-clockwise ring is reversed to clockwise
    const GeoZone prohibited(ed269.zones[0]);
    const QList<QList<QGeoCoordinate>> polygons = prohibited.exclusionPolygons();
    QCOMPARE(polygons.count(), 1);
    QList<QGeoCoordinate> expected = ed269.zones[0].volumes[0].rings[0];
    std::reverse(expected.begin(), expected.end());
    QCOMPARE(polygons[0], expected);
    QVERIFY(prohibited.exclusionCircles().isEmpty());

    const GeoZone circle(ed269.zones[1]);
    QVERIFY(circle.exclusionPolygons().isEmpty());
    QCOMPARE(circle.exclusionCircles().count(), 1);
    QCOMPARE(circle.exclusionCircles()[0].radius(), 1000 * 0.3048);

    // Holes are dropped so the whole outer boundary is excluded
    const GeoZone multiPolygon(ed318.zones[1]);
    const QList<QList<QGeoCoordinate>> multiPolygons = multiPolygon.exclusionPolygons();
    QCOMPARE(multiPolygons.count(), 2);
    QCOMPARE(multiPolygons[0].count(), 4);
    QCOMPARE(multiPolygons[1].count(), 3);
}

void GeoZoneTest::_testManagerLoadPersistClear()
{
    const QTemporaryDir storageDir;
    const QTemporaryDir sourceDir;
    QVERIFY(storageDir.isValid() && sourceDir.isValid());
    const QString ed318File = sourceDir.filePath(QStringLiteral("zones.json"));
    const QString ed269File = sourceDir.filePath(QStringLiteral("other.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), ed318File));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed269_test.json"), ed269File));
    const auto storedFiles = [&storageDir]() {
        return QDir(storageDir.path()).entryList({ QStringLiteral("*.json") }, QDir::Files);
    };

    {
        GeoZoneManager manager(storageDir.path());
        manager.loadStoredSources(false);
        QCOMPARE(manager.zones()->count(), 0);
        QCOMPARE(manager.sources()->count(), 0);

        QString errorString;
        QVERIFY2(manager.loadFile(ed318File, errorString), qPrintable(errorString));
        QCOMPARE(manager.zones()->count(), 3);
        QCOMPARE(manager.shapes().count(), 5);
        QCOMPARE(std::count_if(manager.shapes().cbegin(), manager.shapes().cend(), [](const GeoZoneShape *shape) { return shape->isCircle(); }), 1);
        QVERIFY(manager.boundingBox().isValid());

        QCOMPARE(manager.sources()->count(), 1);
        const GeoZoneSource *const ed318Source = manager.sources()->value<GeoZoneSource*>(0);
        QCOMPARE(ed318Source->key(), QStringLiteral("zones.json"));
        QVERIFY(!ed318Source->isFeed());
        QCOMPARE(ed318Source->name(), QStringLiteral("Test GeoZones"));
        QCOMPARE(ed318Source->formatName(), QStringLiteral("ED-318"));
        QCOMPARE(ed318Source->provider(), QStringLiteral("Test Authority"));
        QCOMPARE(ed318Source->skippedCount(), 1);
        QCOMPARE(ed318Source->dataDate(), _utc(QDate(2026, 8, 13), QTime(0, 0)));
        QVERIFY(ed318Source->detailsText().contains(QStringLiteral("issued 2026-08-13")));
        QVERIFY(QFile::exists(QDir(storageDir.path()).filePath(ed318Source->storedFileName())));

        // A failed load keeps the current zones
        QVERIFY(!manager.loadFile(sourceDir.filePath(QStringLiteral("missing.json")), errorString));
        QVERIFY(!errorString.isEmpty());
        QCOMPARE(manager.zones()->count(), 3);
        QCOMPARE(manager.sources()->count(), 1);

        QCOMPARE(manager.zonesInArea({ GeoZone::Prohibited }, QGeoRectangle()).count(), 1);
        const QGeoRectangle ireland(QGeoCoordinate(52.0, -9.0), QGeoCoordinate(51.0, -8.0));
        const QList<GeoZone*> zonesInArea = manager.zonesInArea({ GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::USpace }, ireland);
        QCOMPARE(zonesInArea.count(), 1);
        QCOMPARE(zonesInArea[0]->name(), QStringLiteral("Control zone"));

        // Another file adds its zones, loading a file with the same name again replaces its zones
        QVERIFY2(manager.loadFile(ed269File, errorString), qPrintable(errorString));
        QCOMPARE(manager.sources()->count(), 2);
        QCOMPARE(manager.zones()->count(), 6);
        const QString previousStoredFile = ed318Source->storedFileName();
        QVERIFY2(manager.loadFile(ed318File, errorString), qPrintable(errorString));
        QCOMPARE(manager.sources()->count(), 2);
        QCOMPARE(manager.zones()->count(), 6);
        QVERIFY(ed318Source->storedFileName() != previousStoredFile);
        QCOMPARE(storedFiles().count(), 3);
    }

    // Unreferenced files are removed on startup
    QFile strayFile(QDir(storageDir.path()).filePath(QStringLiteral("stray.json")));
    QVERIFY(strayFile.open(QIODevice::WriteOnly));
    strayFile.close();

    {
        // Stored copies are reloaded in order on startup
        GeoZoneManager manager(storageDir.path());
        manager.loadStoredSources(false);
        QCOMPARE(manager.sources()->count(), 2);
        QCOMPARE(manager.zones()->count(), 6);
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(0)->key(), QStringLiteral("zones.json"));
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(0)->formatName(), QStringLiteral("ED-318"));
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(1)->key(), QStringLiteral("other.json"));
        QVERIFY(!strayFile.exists());

        manager.removeSource(0);
        QCOMPARE(manager.sources()->count(), 1);
        QCOMPARE(manager.zones()->count(), 3);
        QCOMPARE(storedFiles().count(), 2);

        manager.clear();
        QCOMPARE(manager.zones()->count(), 0);
        QVERIFY(manager.shapes().isEmpty());
        QVERIFY(storedFiles().isEmpty());
    }

    GeoZoneManager manager(storageDir.path());
    manager.loadStoredSources(false);
    QCOMPARE(manager.sources()->count(), 0);

    // The single file kept by earlier versions is migrated
    const QTemporaryDir legacyDir;
    QVERIFY(legacyDir.isValid());
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), legacyDir.filePath(QStringLiteral("geozones.json"))));
    QSettings settings;
    settings.setValue(QStringLiteral("GeoZoneManager/sourceName"), QStringLiteral("Legacy zones"));
    {
        GeoZoneManager legacyManager(legacyDir.path());
        legacyManager.loadStoredSources(false);
        QCOMPARE(legacyManager.sources()->count(), 1);
        QCOMPARE(legacyManager.sources()->value<GeoZoneSource*>(0)->key(), QStringLiteral("Legacy zones"));
        QCOMPARE(legacyManager.zones()->count(), 3);
        QVERIFY(!settings.contains(QStringLiteral("GeoZoneManager/sourceName")));
    }
    GeoZoneManager migratedManager(legacyDir.path());
    migratedManager.loadStoredSources(false);
    QCOMPARE(migratedManager.zones()->count(), 3);
}

void GeoZoneTest::_testManagerBackgroundLoad()
{
    const QTemporaryDir storageDir;
    const QTemporaryDir sourceDir;
    QVERIFY(storageDir.isValid() && sourceDir.isValid());
    const QString ed318File = sourceDir.filePath(QStringLiteral("zones.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), ed318File));
    QVERIFY(QDir(sourceDir.path()).mkdir(QStringLiteral("newer")));
    const QString ed269File = sourceDir.filePath(QStringLiteral("newer/zones.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed269_test.json"), ed269File));

    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    manager.addFile(ed318File);
    QVERIFY(manager.busy());
    QVERIFY(addedSpy.wait(10000));
    QVERIFY(!manager.busy());
    QCOMPARE(manager.zones()->count(), 3);

    // Only the most recent load of a file name is applied
    addedSpy.clear();
    manager.addFile(ed318File);
    manager.addFile(ed269File);
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(addedSpy.count(), 1);
    QCOMPARE(manager.sources()->count(), 1);
    QCOMPARE(manager.sources()->value<GeoZoneSource*>(0)->formatName(), QStringLiteral("ED-269"));

    // Removing all sources drops loads in progress
    manager.addFile(ed318File);
    manager.clear();
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(manager.sources()->count(), 0);

    // Feeds keep their URL as key and can be refreshed
    const QString feedUrl = QUrl::fromLocalFile(ed318File).toString();
    addedSpy.clear();
    manager.addUrl(feedUrl);
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.busy());
    GeoZoneSource *const feed = manager.source(feedUrl);
    QVERIFY(feed);
    QVERIFY(feed->isFeed());
    QCOMPARE(feed->url(), feedUrl);
    QCOMPARE(manager.zones()->count(), 3);
    // The user's file is read in place, never removed
    QVERIFY(QFile::exists(ed318File));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVERIFY(!feed->refreshDue(now, GeoZoneManager::refreshIntervalHours));
    QVERIFY(feed->refreshDue(now.addSecs(25 * 3600), GeoZoneManager::refreshIntervalHours));

    // A failed refresh keeps the previous data
    QVERIFY(QFile::remove(ed318File));
    manager.refreshFeeds();
    QVERIFY(feed->busy());
    QTRY_VERIFY(!manager.busy());
    QVERIFY(!feed->busy());
    QVERIFY(!feed->errorString().isEmpty());
    QCOMPARE(manager.zones()->count(), 3);

    // Stored sources load in the background in their original order
    GeoZoneManager restarted(storageDir.path());
    restarted.loadStoredSources(true);
    QCOMPARE(restarted.sources()->count(), 1);
    QVERIFY(restarted.busy());
    QTRY_VERIFY(!restarted.busy());
    QCOMPARE(restarted.zones()->count(), 3);
    QVERIFY(restarted.source(feedUrl)->isFeed());
}

void GeoZoneTest::_testSourceValidity()
{
    GeoZoneSource source(QStringLiteral("zones.json"), QString());
    GeoZoneParser::Result result;
    result.format = GeoZoneParser::Format::ED269;
    const QDateTime now = QDateTime::currentDateTimeUtc();

    // Without an issue date the file date counts
    source.setResult(result, now, now.addDays(-10));
    QVERIFY(source.validityText().isEmpty());
    source.setResult(result, now, now.addDays(-30));
    QCOMPARE(source.validityText(), QStringLiteral("Data is 30 days old"));

    result.issued = now.addDays(-1);
    source.setResult(result, now, now.addDays(-30));
    QVERIFY(source.validityText().isEmpty());

    result.validTo = now.addDays(-2);
    source.setResult(result, now, now);
    QVERIFY(source.validityText().startsWith(QStringLiteral("Expired on")));

    result.validTo = QDateTime();
    result.validFrom = now.addDays(2);
    source.setResult(result, now, now);
    QVERIFY(source.validityText().startsWith(QStringLiteral("Not valid until")));
    source.updateValidity(now.addDays(3));
    QVERIFY(source.validityText().isEmpty());

    // The manager summarizes outdated sources
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.path());
    QString errorString;
    QVERIFY2(manager.loadFile(QStringLiteral(":/unittest/ed318_test.json"), errorString), qPrintable(errorString));
    QVERIFY(manager.validityWarning().startsWith(QStringLiteral("Test GeoZones: Data is")));
}

void GeoZoneTest::_testGeoFenceImport()
{
    const GeoZoneParser::Result ed269 = _parseResource(QStringLiteral("ed269_test.json"));
    const GeoZoneParser::Result ed318 = _parseResource(QStringLiteral("ed318_test.json"));
    QCOMPARE(ed269.zones.count(), 3);
    QCOMPARE(ed318.zones.count(), 3);

    QObject zoneParent;
    const QList<GeoZone*> zones = {
        new GeoZone(ed269.zones[0], &zoneParent),   // One polygon after merging identical footprints
        new GeoZone(ed269.zones[1], &zoneParent),   // Circle
        new GeoZone(ed318.zones[1], &zoneParent),   // Two polygons
    };

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    geoFenceController->setDirty(false);

    QCOMPARE(geoFenceController->addGeoZoneExclusions(zones), 4);
    QCOMPARE(geoFenceController->polygons()->count(), 3);
    QCOMPARE(geoFenceController->circles()->count(), 1);
    QVERIFY(geoFenceController->dirty());

    const QGCFencePolygon *const polygon = geoFenceController->polygons()->value<QGCFencePolygon*>(0);
    QVERIFY(!polygon->inclusion());
    QCOMPARE(polygon->count(), 4);

    QGCFenceCircle *const circle = geoFenceController->circles()->value<QGCFenceCircle*>(0);
    QVERIFY(!circle->inclusion());
    QCOMPARE(circle->center(), QGeoCoordinate(47.5, 8.5));
    QCOMPARE(circle->radius()->rawValue().toDouble(), 1000 * 0.3048);

    // Importing the same zones again does not duplicate fences
    QCOMPARE(geoFenceController->addGeoZoneExclusions(zones), 0);
    QCOMPARE(geoFenceController->polygons()->count(), 3);
    QCOMPARE(geoFenceController->circles()->count(), 1);
}

void GeoZoneTest::_testGeoFenceImportPreview()
{
    // Two circular zones published as 40 vertex polygons
    QObject zoneParent;
    QList<GeoZone*> zones;
    for (int i = 0; i < 2; i++) {
        GeoZone::Volume volume;
        volume.rings.append(_circleRing(QGeoCoordinate(47, 8 + (i * 0.1)), 500, 40));
        GeoZone::Data data;
        data.identifier = QStringLiteral("Z%1").arg(i);
        data.restriction = GeoZone::Prohibited;
        data.volumes.append(volume);
        zones.append(new GeoZone(data, &zoneParent));
    }

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    QVERIFY(masterController.managerVehicle()->px4Firmware());
    GeoFenceController *const geoFenceController = masterController.geoFenceController();

    // Exceeds the PX4 fence storage
    QVariantMap preview = geoFenceController->geoZoneExclusionsPreview(zones);
    QCOMPARE(preview[QStringLiteral("fenceCount")].toInt(), 2);
    QCOMPARE(preview[QStringLiteral("pointCount")].toInt(), 80);
    QCOMPARE(preview[QStringLiteral("totalPointCount")].toInt(), 80);
    QVERIFY(!preview[QStringLiteral("limitWarning")].toString().isEmpty());

    // Simplified polygons fit
    preview = geoFenceController->geoZoneExclusionsPreview(zones, 25);
    const int simplifiedPointCount = preview[QStringLiteral("pointCount")].toInt();
    QCOMPARE(preview[QStringLiteral("fenceCount")].toInt(), 2);
    QVERIFY(simplifiedPointCount < GeoFenceController::px4MaxFencePoints);
    QVERIFY(preview[QStringLiteral("limitWarning")].toString().isEmpty());

    QCOMPARE(geoFenceController->addGeoZoneExclusions(zones, 25), 2);
    QCOMPARE(geoFenceController->polygons()->value<QGCFencePolygon*>(0)->count() + geoFenceController->polygons()->value<QGCFencePolygon*>(1)->count(), simplifiedPointCount);

    // Existing fences count towards the total but are not added again
    preview = geoFenceController->geoZoneExclusionsPreview(zones, 25);
    QCOMPARE(preview[QStringLiteral("fenceCount")].toInt(), 0);
    QCOMPARE(preview[QStringLiteral("totalPointCount")].toInt(), simplifiedPointCount);
    preview = geoFenceController->geoZoneExclusionsPreview(zones);
    QCOMPARE(preview[QStringLiteral("totalPointCount")].toInt(), simplifiedPointCount + 80);
}

void GeoZoneTest::_testContainsAndPaths()
{
    GeoZone::Volume polygonVolume;
    // 1 km square with a 200 m hole in the middle
    const QGeoCoordinate center(47, 8);
    polygonVolume.rings.append({ center.atDistanceAndAzimuth(707.1, 45), center.atDistanceAndAzimuth(707.1, 135),
                                 center.atDistanceAndAzimuth(707.1, 225), center.atDistanceAndAzimuth(707.1, 315) });
    polygonVolume.rings.append({ center.atDistanceAndAzimuth(141.4, 45), center.atDistanceAndAzimuth(141.4, 135),
                                 center.atDistanceAndAzimuth(141.4, 225), center.atDistanceAndAzimuth(141.4, 315) });
    GeoZone::Data polygonData;
    polygonData.volumes.append(polygonVolume);
    const GeoZone polygon(polygonData);

    QVERIFY(polygon.contains(center.atDistanceAndAzimuth(300, 0)));
    QVERIFY(!polygon.contains(center));
    QVERIFY(!polygon.contains(center.atDistanceAndAzimuth(600, 0)));
    QCOMPARE(polygon.distanceTo(center.atDistanceAndAzimuth(300, 0)), 0.0);
    QVERIFY(qAbs(polygon.distanceTo(center.atDistanceAndAzimuth(600, 0)) - 100) < 1);
    QVERIFY(qAbs(polygon.distanceTo(center) - 100) < 1);

    // Paths through the zone, inside the hole only, and passing by
    QVERIFY(polygon.intersectsPath({ center.atDistanceAndAzimuth(2000, 270), center.atDistanceAndAzimuth(2000, 90) }));
    QVERIFY(polygon.intersectsPath({ center.atDistanceAndAzimuth(300, 0) }));
    QVERIFY(!polygon.intersectsPath({ center.atDistanceAndAzimuth(50, 270), center.atDistanceAndAzimuth(50, 90) }));
    QVERIFY(!polygon.intersectsPath({ center.atDistanceAndAzimuth(1000, 0).atDistanceAndAzimuth(2000, 270), center.atDistanceAndAzimuth(1000, 0).atDistanceAndAzimuth(2000, 90) }));

    GeoZone::Volume circleVolume;
    circleVolume.isCircle = true;
    circleVolume.center = center;
    circleVolume.radius = 500;
    GeoZone::Data circleData;
    circleData.volumes.append(circleVolume);
    const GeoZone circle(circleData);
    QVERIFY(qAbs(circle.distanceTo(center.atDistanceAndAzimuth(600, 30)) - 100) < 1);
    QVERIFY(circle.intersectsPath({ center.atDistanceAndAzimuth(1000, 270).atDistanceAndAzimuth(400, 0), center.atDistanceAndAzimuth(1000, 90).atDistanceAndAzimuth(400, 0) }));
    QVERIFY(!circle.intersectsPath({ center.atDistanceAndAzimuth(1000, 270).atDistanceAndAzimuth(600, 0), center.atDistanceAndAzimuth(1000, 90).atDistanceAndAzimuth(600, 0) }));

    // Connected segments form one path, long legs get points along the great circle
    const QGeoCoordinate a(47, 8);
    const QGeoCoordinate b = a.atDistanceAndAzimuth(500, 0);
    const QGeoCoordinate c = b.atDistanceAndAzimuth(500, 90);
    const QGeoCoordinate d = a.atDistanceAndAzimuth(5000, 180);
    const QList<QList<QGeoCoordinate>> paths = GeoZoneMissionCheck::pathsFromSegments({ { a, b }, { b, c }, { QGeoCoordinate(), d }, { d, a } });
    QCOMPARE(paths.count(), 2);
    QCOMPARE(paths[0], QList<QGeoCoordinate>({ a, b, c }));
    QCOMPARE(paths[1].count(), 6);
    QCOMPARE(paths[1].first(), d);
    QCOMPARE(paths[1].last(), a);
    QVERIFY(qAbs(paths[1][1].distanceTo(d) - 1000) < 1);
}

void GeoZoneTest::_testMissionCheck()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("C"), QStringLiteral("CONDITIONAL"), center.atDistanceAndAzimuth(2000, 0), 300),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(2000, 180), 300),
    });
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();

    GeoZoneMissionCheck check(nullptr, &manager);
    check.setMissionController(missionController);

    // Passes the no restriction zone only
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 180).atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 180).atDistanceAndAzimuth(1000, 90), 2);
    QTest::qWait(1000);
    QVERIFY(check.crossedZones().isEmpty());
    QVERIFY(check.warningText().isEmpty());

    // Through the prohibited zone and then the conditional one, listed most restrictive first
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 3);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 4);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 0), 5);
    QTRY_COMPARE(check.crossedZones().count(), 2);
    QCOMPARE(qobject_cast<GeoZone*>(check.crossedZones()[0])->identifier(), QStringLiteral("P"));
    QCOMPARE(qobject_cast<GeoZone*>(check.crossedZones()[1])->identifier(), QStringLiteral("C"));
    QVERIFY(check.warningText().contains(QStringLiteral("P (Prohibited)")));

    // Zones are released before the manager deletes them
    manager.clear();
    QVERIFY(check.crossedZones().isEmpty());
}

void GeoZoneTest::_testMonitorAlerts()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QJsonObject expired = _circleFeature(QStringLiteral("E"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(3000, 90), 300);
    expired[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2020-01-01T00:00:00Z" }, { "endDateTime", "2020-01-02T00:00:00Z" } } };
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(3000, 270), 300),
        expired,
    });
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);

    // Far away, then approaching, then inside, each alerting once
    monitor.evaluate(center.atDistanceAndAzimuth(1000, 0));
    QCOMPARE(alertSpy.count(), 0);
    monitor.evaluate(center.atDistanceAndAzimuth(380, 0));
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Approaching geozone P")));
    monitor.evaluate(center.atDistanceAndAzimuth(350, 0));
    QCOMPARE(alertSpy.count(), 1);
    monitor.evaluate(center.atDistanceAndAzimuth(250, 0));
    QCOMPARE(alertSpy.count(), 2);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Entered geozone P")));

    // Jitter across the boundary and the approach distance does not alert again
    monitor.evaluate(center.atDistanceAndAzimuth(310, 0));
    monitor.evaluate(center.atDistanceAndAzimuth(290, 0));
    monitor.evaluate(center.atDistanceAndAzimuth(410, 0));
    monitor.evaluate(center.atDistanceAndAzimuth(390, 0));
    QCOMPARE(alertSpy.count(), 2);

    // Leaving the area resets the zone
    monitor.evaluate(center.atDistanceAndAzimuth(1000, 0));
    monitor.evaluate(center.atDistanceAndAzimuth(250, 0));
    QCOMPARE(alertSpy.count(), 3);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Entered geozone P")));

    // Unrestricted and inactive zones don't alert
    monitor.evaluate(center.atDistanceAndAzimuth(3000, 270));
    monitor.evaluate(center.atDistanceAndAzimuth(3000, 90));
    QCOMPARE(alertSpy.count(), 3);

    // Zones are forgotten before the manager deletes them
    manager.clear();
    monitor.evaluate(center);
    QCOMPARE(alertSpy.count(), 3);
}

void GeoZoneTest::_testLabelZonesLimit()
{
    // Many zones centered in the same small area, alternating restrictions
    QJsonArray features;
    const int zoneCount = GeoZoneManager::maxLabelCount * 2;
    for (int i = 0; i < zoneCount; i++) {
        const QGeoCoordinate center(47.0 + (i / 40) * 0.01, 8.0 + (i % 40) * 0.01);
        features.append(_circleFeature(QStringLiteral("Z%1").arg(i), (i % 2) ? QStringLiteral("NO_RESTRICTION") : QStringLiteral("PROHIBITED"), center, 100));
    }
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fileName = _writeFeatures(dir, QStringLiteral("many.json"), features);

    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));
    QCOMPARE(manager.zones()->count(), zoneCount);

    QVERIFY(manager.labelZones(QGeoRectangle()).isEmpty());

    // Only zones centered in the region are labelled
    const QGeoRectangle smallRegion(QGeoCoordinate(47.005, 7.995), QGeoCoordinate(46.995, 8.035));
    QCOMPARE(manager.labelZones(smallRegion).count(), 4);

    // The limit keeps the most restrictive zones
    const QList<QObject*> labels = manager.labelZones(manager.boundingBox());
    QCOMPARE(labels.count(), GeoZoneManager::maxLabelCount);
    for (const QObject *object : labels) {
        QCOMPARE(qobject_cast<const GeoZone*>(object)->restriction(), GeoZone::Prohibited);
    }
}

void GeoZoneTest::_testShapeSimplification()
{
    // Dense 5 km circle with a 40 m hole
    const QGeoCoordinate center(47.0, 8.0);
    const QList<QGeoCoordinate> ring = _circleRing(center, 5000, 10000);
    const GeoZoneShape shape(nullptr, { ring, _circleRing(center, 20, 8) });

    const QGeoPolygon full = shape.polygon(GeoZoneShape::fullDetailZoomLevel);
    QCOMPARE(full.perimeter().count(), ring.count());
    QCOMPARE(full.holesCount(), 1);

    const int zoomLevel = 10;
    const QGeoPolygon simplified = shape.polygon(zoomLevel);
    QVERIFY(simplified.perimeter().count() >= 3);
    QVERIFY(simplified.perimeter().count() < 100);
    QCOMPARE(simplified.holesCount(), 0); // The hole is under a pixel at this zoom level
    QCOMPARE(shape.polygon(16).holesCount(), 1);

    // Shapes under a pixel become a triangle spanning them instead of keeping every vertex
    const QList<QGeoCoordinate> smallRing = _circleRing(center, 50, 200);
    const GeoZoneShape smallShape(nullptr, { smallRing });
    const QList<QGeoCoordinate> triangle = smallShape.polygon(5).perimeter();
    QCOMPARE(triangle.count(), 3);
    const QList<double> sides = { triangle[0].distanceTo(triangle[1]), triangle[1].distanceTo(triangle[2]), triangle[2].distanceTo(triangle[0]) };
    QVERIFY(*std::max_element(sides.cbegin(), sides.cend()) > 95); // Spans the 100 m diameter
    QVERIFY(*std::min_element(sides.cbegin(), sides.cend()) > 65);
    QCOMPARE(smallShape.polygon(GeoZoneShape::fullDetailZoomLevel).perimeter().count(), smallRing.count());

    // Simplified vertices are original vertices and every original vertex stays within half a pixel of the outline
    const double tolerance = 0.5 / GeoZoneShape::mercatorPixels(zoomLevel);
    for (const QGeoCoordinate &vertex : simplified.perimeter()) {
        QVERIFY(ring.contains(vertex));
    }
    for (const QGeoCoordinate &vertex : ring) {
        QVERIFY(_distanceToRing(GeoZoneShape::toMercator(vertex), simplified.perimeter()) <= tolerance * 1.0001);
    }

    // A 1 km circle is 2000 m / 104.26 m per pixel wide at zoom level 10 and 47 degrees latitude
    const GeoZoneShape circle(nullptr, center, 1000);
    const double widthPixels = circle.mercatorBounds().width() * GeoZoneShape::mercatorPixels(zoomLevel);
    QVERIFY2(qAbs(widthPixels - 19.18) < 0.2, qPrintable(QString::number(widthPixels)));
    QCOMPARE(circle.mercatorBounds().height(), circle.mercatorBounds().width());
}

void GeoZoneTest::_testMapModel()
{
    const QGeoRectangle view(QGeoCoordinate(47.1, 7.85), QGeoCoordinate(46.9, 8.15));
    const QJsonArray features = {
        _polygonFeature(QStringLiteral("BIG"), QStringLiteral("PROHIBITED"), _circleRing(QGeoCoordinate(47.0, 8.0), 4000, 2000)),
        _circleFeature(QStringLiteral("TINY"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.01, 8.01), 20),
        _circleFeature(QStringLiteral("NEAR"), QStringLiteral("CONDITIONAL"), QGeoCoordinate(47.0, 8.2), 500),     // Within the view margin
        _circleFeature(QStringLiteral("FAR"), QStringLiteral("PROHIBITED"), QGeoCoordinate(50.0, 15.0), 1000),
    };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString), qPrintable(errorString));

    GeoZoneMapModel model(&manager);
    const auto identifiers = [](const QmlObjectListModel *list) {
        QStringList result;
        for (int i = 0; i < list->count(); i++) {
            result.append(list->value<GeoZoneMapItem*>(i)->zone()->identifier());
        }
        result.sort();
        return result;
    };

    // Only shapes in or near the view
    model.setView(view, 10, false);
    QCOMPARE(identifiers(model.polygons()), QStringList({ QStringLiteral("BIG") }));
    QCOMPARE(identifiers(model.circles()), QStringList({ QStringLiteral("NEAR"), QStringLiteral("TINY") }));
    QCOMPARE(model.hiddenZoneCount(), 0);
    QCOMPARE(model.labelZones()->count(), 0);

    GeoZoneMapItem *const polygonItem = model.polygons()->value<GeoZoneMapItem*>(0);
    const qsizetype lowDetailCount = QGeoPolygon(polygonItem->geoShape()).perimeter().count();
    QVERIFY(lowDetailCount < 2000);

    // Zooming in keeps the items and adds detail. Only zones centered in the view are labelled.
    model.setView(view, 16, true);
    QCOMPARE(model.polygons()->value<GeoZoneMapItem*>(0), polygonItem);
    QCOMPARE(model.labelZones()->count(), 2);
    QVERIFY(QGeoPolygon(polygonItem->geoShape()).perimeter().count() > lowDetailCount);

    // Another file adds items, removing a source removes its items
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("other.json"),
        { _circleFeature(QStringLiteral("OTHER"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.0, 8.0), 1000) }), errorString), qPrintable(errorString));
    QCOMPARE(model.polygons()->count(), 1);
    QVERIFY(identifiers(model.circles()).contains(QStringLiteral("OTHER")));
    manager.removeSource(0);
    QCOMPARE(model.polygons()->count(), 0);
    QCOMPARE(identifiers(model.circles()), QStringList({ QStringLiteral("OTHER") }));

    model.setView(QGeoRectangle(), 16, true);
    QCOMPARE(model.circles()->count(), 0);
    QCOMPARE(model.labelZones()->count(), 0);

    model.setView(view, 16, true);
    manager.clear();
    QCOMPARE(model.circles()->count(), 0);
}

void GeoZoneTest::_testMapModelShapeLimit()
{
    // More zones in view than can be shown, with growing radius
    QJsonArray features;
    const int extraZones = 10;
    for (int i = 0; i < GeoZoneMapModel::maxShapeCount + extraZones; i++) {
        const QGeoCoordinate center(47.0 + (i / 50) * 0.005, 8.0 + (i % 50) * 0.005);
        features.append(_circleFeature(QStringLiteral("Z%1").arg(i), QStringLiteral("PROHIBITED"), center, 100 + i));
    }
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString), qPrintable(errorString));

    // The smallest zones are dropped and not labelled
    GeoZoneMapModel model(&manager);
    model.setView(manager.boundingBox(), 12, true);
    QCOMPARE(model.circles()->count(), GeoZoneMapModel::maxShapeCount);
    QCOMPARE(model.hiddenZoneCount(), extraZones);
    for (int i = 0; i < model.circles()->count(); i++) {
        QVERIFY(model.circles()->value<GeoZoneMapItem*>(i)->radius() >= 100 + extraZones);
    }
    QVERIFY(model.labelZones()->count() > 0);
    for (int i = 0; i < model.labelZones()->count(); i++) {
        QVERIFY(model.labelZones()->value<GeoZone*>(i)->identifier().mid(1).toInt() >= extraZones);
    }
}
