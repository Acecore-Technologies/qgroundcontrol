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
#include "GeoZoneDownloader.h"
#include "GeoZoneAlertLog.h"
#include "GeoZoneFenceImporter.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneIndex.h"
#include "GeoZoneManager.h"
#include "GeoZoneMapModel.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMonitor.h"
#include "GeoZoneParser.h"
#include "GeoZoneProfile.h"
#include "GeoZoneSettings.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "QGCMapPolygon.h"
#include "SettingsManager.h"
#include "SurveyComplexItem.h"
#include "UnitsSettings.h"
#include "Vehicle.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QRandomGenerator>
#include <QtCore/QScopeGuard>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimeZone>
#include <QtCore/QUrl>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
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
    QCOMPARE(polygon.upperReference, GeoZone::VerticalReference::Ground);
    QCOMPARE(polygon.upperReferenceCode, QStringLiteral("AGL"));
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
    QCOMPARE(circle.upperReference, GeoZone::VerticalReference::MeanSeaLevel);
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

void GeoZoneTest::_testParseFaaTfr()
{
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parseFile(QStringLiteral(":/unittest/faa_tfr_test.json"), result, errorString), qPrintable(errorString));
    QCOMPARE(result.format, GeoZoneParser::Format::FaaTfr);
    QCOMPARE(result.zones.count(), 2);
    QCOMPARE(result.issued, _utc(QDate(2026, 10, 31), QTime(12, 20)));

    const GeoZone::Data &tfr = result.zones[0];
    QCOMPARE(tfr.identifier, QStringLiteral("6/4240-1-FDC-F"));
    QCOMPARE(tfr.restriction, GeoZone::Prohibited);
    QVERIFY(tfr.name.startsWith(QStringLiteral("SAN ANGELO, TX")));
    QCOMPARE(tfr.otherReasonInfo, QStringLiteral("SECURITY"));
    QVERIFY(tfr.message.contains(QStringLiteral("6/4240")));
    QCOMPARE(tfr.volumes.count(), 1);
    QVERIFY(qIsNaN(tfr.volumes[0].lowerLimit) && qIsNaN(tfr.volumes[0].upperLimit));
    QVERIFY(tfr.applicability.isEmpty());

    // An empty list from the FAA is valid, all restrictions have ended. From other GeoServer feeds it's an error,
    // which keeps their previous zones.
    const QByteArray emptyList = R"({ "type": "FeatureCollection", "features": [], "totalFeatures": 0, "timeStamp": "2026-10-31T12:20:00Z" })";
    QVERIFY2(GeoZoneParser::parse(emptyList, result, errorString, GeoZoneParser::Format::FaaTfr), qPrintable(errorString));
    QCOMPARE(result.format, GeoZoneParser::Format::FaaTfr);
    QVERIFY(result.zones.isEmpty());
    QVERIFY(!GeoZoneParser::parse(emptyList, result, errorString));

    // Live restrictions are refreshed more often than other feeds
    GeoZoneSource source(QStringLiteral("https://tfr.faa.gov/x"), QStringLiteral("https://tfr.faa.gov/x"));
    QVERIFY2(GeoZoneParser::parseFile(QStringLiteral(":/unittest/faa_tfr_test.json"), result, errorString), qPrintable(errorString));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    source.setResult(result, now, now);
    QCOMPARE(source.refreshIntervalSecs(), 15 * 60);
    QVERIFY(!source.refreshDue(now.addSecs(10 * 60)));
    QVERIFY(source.refreshDue(now.addSecs(16 * 60)));

    // Restrictions change during the day, so the list is outdated after hours rather than weeks
    source.updateValidity(result.issued.addSecs(3600));
    QVERIFY(source.validityText().isEmpty());
    source.updateValidity(result.issued.addSecs(3 * 3600));
    QCOMPARE(source.validityText(), QStringLiteral("Data is 3 hours old"));
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

    // Sunrise to sunset at the zone in southern Sweden, within the date range
    const GeoZone daylight(ed318.zones[0]);
    QVERIFY(daylight.isActiveAt(_utc(QDate(2026, 6, 1), QTime(12, 0))));
    QVERIFY(!daylight.isActiveAt(_utc(QDate(2026, 6, 1), QTime(23, 30))));
    QVERIFY(!daylight.isActiveAt(_utc(QDate(2026, 12, 1), QTime(7, 0))));
    QVERIFY(!daylight.isActiveAt(_utc(QDate(2027, 1, 2), QTime(12, 0))));
    QVERIFY(daylight.isActiveDuring(_utc(QDate(2026, 6, 1), QTime(22, 0)), _utc(QDate(2026, 6, 2), QTime(3, 0))));
    QVERIFY(!daylight.isActiveDuring(_utc(QDate(2026, 12, 1), QTime(16, 0)), _utc(QDate(2026, 12, 2), QTime(6, 0))));
    QCOMPARE(daylight.applicabilityText(), QStringLiteral("From 2026-01-01 00:00 UTC to 2026-12-31 23:59 UTC\n  Every day sunrise-sunset"));

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

void GeoZoneTest::_testDaylightEvents()
{
    const auto minutesFrom = [](const QDateTime &time, const QDateTime &expected) {
        return qAbs(time.secsTo(expected)) / 60.0;
    };

    // Published times for Zurich at midsummer and London at midwinter
    const QGeoCoordinate zurich(47.37, 8.54);
    const QDate midsummer(2026, 6, 21);
    QVERIFY(minutesFrom(GeoZoneSchedule::daylightEventTime(QStringLiteral("SR"), midsummer, zurich), _utc(midsummer, QTime(3, 30))) < 3);
    QVERIFY(minutesFrom(GeoZoneSchedule::daylightEventTime(QStringLiteral("SS"), midsummer, zurich), _utc(midsummer, QTime(19, 26))) < 3);
    QVERIFY(GeoZoneSchedule::daylightEventTime(QStringLiteral("BMCT"), midsummer, zurich) < GeoZoneSchedule::daylightEventTime(QStringLiteral("SR"), midsummer, zurich));
    QVERIFY(GeoZoneSchedule::daylightEventTime(QStringLiteral("EECT"), midsummer, zurich) > GeoZoneSchedule::daylightEventTime(QStringLiteral("SS"), midsummer, zurich));
    const QGeoCoordinate london(51.5, -0.13);
    const QDate midwinter(2026, 12, 21);
    QVERIFY(minutesFrom(GeoZoneSchedule::daylightEventTime(QStringLiteral("SR"), midwinter, london), _utc(midwinter, QTime(8, 4))) < 3);
    QVERIFY(minutesFrom(GeoZoneSchedule::daylightEventTime(QStringLiteral("SS"), midwinter, london), _utc(midwinter, QTime(15, 54))) < 3);
    QVERIFY(!GeoZoneSchedule::daylightEventTime(QStringLiteral("XX"), midwinter, london).isValid());

    // North of the arctic circle the sun doesn't set at midsummer and doesn't rise at midwinter
    const QGeoCoordinate tromso(69.65, 18.96);
    QVERIFY(!GeoZoneSchedule::daylightEventTime(QStringLiteral("SR"), midsummer, tromso).isValid());
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(R"({ "features": [ { "identifier": "D", "restriction": "PROHIBITED",
        "applicability": [ { "permanent": "NO", "schedule": [ { "day": ["ANY"], "startEvent": "SR", "endEvent": "SS" } ] } ],
        "geometry": [ { "horizontalProjection": { "type": "Circle", "center": [18.96, 69.65], "radius": 100 } } ] } ] })", result, errorString), qPrintable(errorString));
    const GeoZone arctic(result.zones[0]);
    QVERIFY(arctic.isActiveAt(_utc(midsummer, QTime(23, 0))));
    QVERIFY(!arctic.isActiveAt(_utc(midwinter, QTime(11, 0))));
    QVERIFY(!arctic.isActiveDuring(_utc(midwinter, QTime(0, 0)), _utc(midwinter, QTime(23, 0))));

    // Night periods cover the whole polar night and none of the polar day
    QVERIFY2(GeoZoneParser::parse(R"({ "features": [ { "identifier": "N", "restriction": "PROHIBITED",
        "applicability": [ { "permanent": "NO", "schedule": [ { "day": ["ANY"], "startEvent": "SS", "endEvent": "SR" } ] } ],
        "geometry": [ { "horizontalProjection": { "type": "Circle", "center": [18.96, 69.65], "radius": 100 } } ] } ] })", result, errorString), qPrintable(errorString));
    const GeoZone arcticNight(result.zones[0]);
    QVERIFY(arcticNight.isActiveAt(_utc(midwinter, QTime(11, 0))));
    QVERIFY(!arcticNight.isActiveAt(_utc(midsummer, QTime(23, 0))));

    // In Finland the midsummer sunrise is before midnight UTC
    QVERIFY2(GeoZoneParser::parse(R"({ "features": [ { "identifier": "F", "restriction": "PROHIBITED",
        "applicability": [ { "permanent": "NO", "schedule": [ { "day": ["ANY"], "startEvent": "SR", "endEvent": "SS" } ] } ],
        "geometry": [ { "horizontalProjection": { "type": "Circle", "center": [25, 65], "radius": 100 } } ] } ] })", result, errorString), qPrintable(errorString));
    const GeoZone finland(result.zones[0]);
    QVERIFY(GeoZoneSchedule::daylightEventTime(QStringLiteral("SR"), midsummer, QGeoCoordinate(65, 25)) < _utc(midsummer, QTime(0, 0)));
    QVERIFY(finland.isActiveAt(_utc(midsummer.addDays(-1), QTime(23, 45))));
    QVERIFY(finland.isActiveDuring(_utc(midsummer.addDays(-1), QTime(23, 40)), _utc(midsummer.addDays(-1), QTime(23, 50))));
    QVERIFY(!finland.isActiveAt(_utc(midsummer.addDays(-1), QTime(22, 30))));

    // Mixed time and event: 08:00 local time to sunset
    QVERIFY2(GeoZoneParser::parse(R"({ "features": [ { "identifier": "M", "restriction": "PROHIBITED",
        "applicability": [ { "permanent": "NO", "schedule": [ { "day": ["ANY"], "startTime": "08:00+02:00", "endEvent": "SS" } ] } ],
        "geometry": [ { "horizontalProjection": { "type": "Circle", "center": [8.54, 47.37], "radius": 100 } } ] } ] })", result, errorString), qPrintable(errorString));
    const GeoZone mixed(result.zones[0]);
    QVERIFY(!mixed.isActiveAt(_utc(midsummer, QTime(5, 30))));
    QVERIFY(mixed.isActiveAt(_utc(midsummer, QTime(6, 30))));
    QVERIFY(mixed.isActiveAt(_utc(midsummer, QTime(19, 0))));
    QVERIFY(!mixed.isActiveAt(_utc(midsummer, QTime(20, 0))));
    QCOMPARE(mixed.applicabilityText(), QStringLiteral("  Every day 08:00-sunset UTC+02:00"));
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

void GeoZoneTest::_testSpatialIndex()
{
    // Random circles from a few meters to a hundred kilometers, and a box crossing many cells
    QObject zoneParent;
    QList<GeoZone*> zones;
    quint32 seed = 12345;
    const auto random = [&seed]() {
        seed = (seed * 1103515245u) + 12345u;
        return static_cast<double>((seed >> 8) & 0xFFFF) / 0xFFFF;
    };
    for (int i = 0; i < 3000; i++) {
        GeoZone::Volume volume;
        volume.isCircle = true;
        volume.center = QGeoCoordinate(45 + (random() * 5), 5 + (random() * 8));
        volume.radius = (i % 500 == 0) ? 100000 : (10 + (random() * 3000));
        GeoZone::Data data;
        data.identifier = QString::number(i);
        data.restriction = (i % 2) ? GeoZone::Prohibited : GeoZone::Conditional;
        data.volumes.append(volume);
        zones.append(new GeoZone(data, &zoneParent));
    }

    GeoZoneIndex index;
    index.build(zones);
    for (int i = 0; i < 200; i++) {
        const double south = 44 + (random() * 7);
        const double west = 4 + (random() * 10);
        const double size = (i % 20 == 0) ? 10 : (random() * 0.5);
        const QGeoRectangle area(QGeoCoordinate(south + size, west), QGeoCoordinate(south, west + size));

        QList<int> expected;
        for (int j = 0; j < zones.count(); j++) {
            const QGeoRectangle box = zones[j]->boundingBox();
            if ((box.topLeft().longitude() <= area.bottomRight().longitude()) && (area.topLeft().longitude() <= box.bottomRight().longitude()) &&
                (box.bottomRight().latitude() <= area.topLeft().latitude()) && (area.bottomRight().latitude() <= box.topLeft().latitude())) {
                expected.append(j);
            }
        }
        QCOMPARE(index.query(area), expected);
    }

    // A point query matches the zones containing it in their bounding box
    const QGeoCoordinate point = zones[1]->data().volumes[0].center;
    QVERIFY(index.query(point.latitude(), point.longitude(), point.latitude(), point.longitude()).contains(1));
    QVERIFY(index.query(QGeoRectangle()).isEmpty());
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
        QCOMPARE(manager.zones()->value<GeoZone*>(0)->sourceText(), QStringLiteral("Test GeoZones (issued 2026-08-13)"));
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
    QVERIFY(!feed->refreshDue(now));
    QVERIFY(feed->refreshDue(now.addSecs(25 * 3600)));

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

void GeoZoneTest::_testFeedNotModified()
{
    QFile fixture(QStringLiteral(":/unittest/ed318_test.json"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    QByteArray body = fixture.readAll();

    // Serves the feed with an ETag and answers matching conditional requests with 304 Not Modified
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int requestCount = 0;
    int notModifiedCount = 0;
    (void) connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (QTcpSocket *const socket = server.nextPendingConnection()) {
            (void) connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) {
                    return;
                }
                requestCount++;
                // Header names are case insensitive, Qt sends them in lower case
                if (request.toLower().contains("if-none-match: \"v1\"")) {
                    notModifiedCount++;
                    (void) socket->write("HTTP/1.1 304 Not Modified\r\nETag: \"v1\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else {
                    (void) socket->write(QByteArray("HTTP/1.1 200 OK\r\nETag: \"v1\"\r\nLast-Modified: Sat, 31 Oct 2026 10:00:00 GMT\r\n"
                                                    "Content-Type: application/json\r\nConnection: close\r\nContent-Length: ") +
                                         QByteArray::number(body.size()) + "\r\n\r\n" + body);
                }
                socket->disconnectFromHost();
            });
            (void) connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    const QString url = QStringLiteral("http://127.0.0.1:%1/zones.json").arg(server.serverPort());

    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        manager.addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.busy());
        GeoZoneSource *const feed = manager.source(url);
        QVERIFY(feed);
        QCOMPARE(feed->etag(), QStringLiteral("\"v1\""));
        QCOMPARE(feed->lastModified(), QStringLiteral("Sat, 31 Oct 2026 10:00:00 GMT"));
        QCOMPARE(requestCount, 1);

        // An unchanged feed is not downloaded again but counts as current
        QSignalSpy aboutToChangeSpy(&manager, &GeoZoneManager::zonesAboutToChange);
        const QDateTime loadedAt = feed->loadedAt();
        QTest::qWait(10);
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(notModifiedCount, 1);
        QCOMPARE(manager.zones()->count(), 3);
        QVERIFY(feed->loadedAt() > loadedAt);
        QVERIFY(feed->errorString().isEmpty());

        // A server without validators sends the same zones again, which are kept instead of reloaded
        QCOMPARE(aboutToChangeSpy.count(), 0);
        feed->setValidators(QString(), QString());
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(requestCount, 3);
        QCOMPARE(aboutToChangeSpy.count(), 0);
        QCOMPARE(manager.zones()->count(), 3);

        // Changed zones are reloaded, without signaling an added source which would move the maps
        QFile changed(QStringLiteral(":/unittest/ed269_test.json"));
        QVERIFY(changed.open(QIODevice::ReadOnly));
        body = changed.readAll();
        feed->setValidators(QString(), QString());
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(aboutToChangeSpy.count(), 1);
        QCOMPARE(manager.source(url)->formatName(), QStringLiteral("ED-269"));
        QCOMPARE(addedSpy.count(), 1);
    }

    // The validators are kept across restarts
    GeoZoneManager restarted(storageDir.path());
    restarted.loadStoredSources(false);
    QCOMPARE(restarted.source(url)->etag(), QStringLiteral("\"v1\""));
    restarted.refreshFeeds();
    QTRY_VERIFY(!restarted.busy());
    QCOMPARE(notModifiedCount, 2);
    QCOMPARE(restarted.zones()->count(), 3);
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
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);

    QCOMPARE(importer.addExclusions(zones), 4);
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

    // Fences are named after their zones and marked as added from geozones
    const auto zoneName = [](const GeoZone *zone) { return zone->name().isEmpty() ? zone->identifier() : zone->name(); };
    QCOMPARE(polygon->name(), zoneName(zones[0]));
    QVERIFY(polygon->geoZone());
    QCOMPARE(circle->name(), zoneName(zones[1]));
    QVERIFY(circle->geoZone());

    // Which the plan file keeps, while fences of other plans load without them
    QString errorString;
    QJsonObject polygonJson;
    const_cast<QGCFencePolygon*>(polygon)->saveToJson(polygonJson);
    QGCFencePolygon loadedPolygon(true);
    QVERIFY2(loadedPolygon.loadFromJson(polygonJson, true, errorString), qPrintable(errorString));
    QCOMPARE(loadedPolygon.name(), polygon->name());
    QVERIFY(loadedPolygon.geoZone());
    // Loading an unchanged exclusion polygon leaves it clean
    QGCFencePolygon reloadedPolygon(false);
    QVERIFY2(reloadedPolygon.loadFromJson(polygonJson, true, errorString), qPrintable(errorString));
    QVERIFY(!reloadedPolygon.dirty());
    QJsonObject circleJson;
    circle->saveToJson(circleJson);
    QGCFenceCircle loadedCircle;
    QVERIFY2(loadedCircle.loadFromJson(circleJson, errorString), qPrintable(errorString));
    QCOMPARE(loadedCircle.name(), circle->name());
    QVERIFY(loadedCircle.geoZone());
    polygonJson.remove(QStringLiteral("name"));
    polygonJson.remove(QStringLiteral("geoZone"));
    QVERIFY2(loadedPolygon.loadFromJson(polygonJson, true, errorString), qPrintable(errorString));
    QVERIFY(loadedPolygon.name().isEmpty());
    QVERIFY(!loadedPolygon.geoZone());
    QJsonObject drawnJson;
    loadedPolygon.saveToJson(drawnJson);
    QVERIFY(!drawnJson.contains(QStringLiteral("name")) && !drawnJson.contains(QStringLiteral("geoZone")));

    // Importing the same zones again does not duplicate fences
    QCOMPARE(importer.addExclusions(zones), 0);
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
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);

    // Exceeds the PX4 fence storage
    QVariantMap preview = importer.exclusionsPreview(zones);
    QCOMPARE(preview[QStringLiteral("fenceCount")].toInt(), 2);
    QCOMPARE(preview[QStringLiteral("pointCount")].toInt(), 80);
    QCOMPARE(preview[QStringLiteral("totalPointCount")].toInt(), 80);
    QVERIFY(!preview[QStringLiteral("limitWarning")].toString().isEmpty());

    // Simplified polygons fit
    preview = importer.exclusionsPreview(zones, 25);
    const int simplifiedPointCount = preview[QStringLiteral("pointCount")].toInt();
    QCOMPARE(preview[QStringLiteral("fenceCount")].toInt(), 2);
    QVERIFY(simplifiedPointCount < GeoZoneFenceImporter::px4MaxFencePoints);
    QVERIFY(preview[QStringLiteral("limitWarning")].toString().isEmpty());

    QCOMPARE(importer.addExclusions(zones, 25), 2);
    QCOMPARE(geoFenceController->polygons()->value<QGCFencePolygon*>(0)->count() + geoFenceController->polygons()->value<QGCFencePolygon*>(1)->count(), simplifiedPointCount);

    // Existing fences count towards the total but are not added again
    preview = importer.exclusionsPreview(zones, 25);
    QCOMPARE(preview[QStringLiteral("fenceCount")].toInt(), 0);
    QCOMPARE(preview[QStringLiteral("totalPointCount")].toInt(), simplifiedPointCount);
    preview = importer.exclusionsPreview(zones);
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
    const auto coordinates = [](const QList<GeoZone::Position> &path) {
        QList<QGeoCoordinate> result;
        for (const GeoZone::Position &position : path) {
            result.append(position.coordinate);
        }
        return result;
    };
    QList<QList<GeoZone::Position>> paths = GeoZoneMissionCheck::pathsFromSegments({ { a, b }, { b, c }, { QGeoCoordinate(), d }, { d, a } });
    QCOMPARE(paths.count(), 2);
    QCOMPARE(coordinates(paths[0]), QList<QGeoCoordinate>({ a, b, c }));
    QCOMPARE(paths[1].count(), 6);
    QCOMPARE(paths[1].first().coordinate, d);
    QCOMPARE(paths[1].last().coordinate, a);
    QVERIFY(qAbs(paths[1][1].coordinate.distanceTo(d) - 1000) < 1);

    // Positions at the terrain samples with interpolated altitudes, and a climb at a shared waypoint is kept
    GeoZoneMissionCheck::Segment climb{ a, a, 400, 500 };
    GeoZoneMissionCheck::Segment leg{ a, b, 500, 600, { 300, 350, 320 }, 300 };
    paths = GeoZoneMissionCheck::pathsFromSegments({ climb, leg });
    QCOMPARE(paths.count(), 1);
    QCOMPARE(paths[0].count(), 4);
    QCOMPARE(paths[0][0].altitudeAMSL, 400.0);
    QCOMPARE(paths[0][1].altitudeAMSL, 500.0);
    QCOMPARE(paths[0][1].terrainAMSL, 300.0);
    QCOMPARE(paths[0][2].altitudeAMSL, 560.0);
    QCOMPARE(paths[0][2].terrainAMSL, 350.0);
    QVERIFY(qAbs(paths[0][2].coordinate.distanceTo(a) - 300) < 1);
    QCOMPARE(paths[0][3].coordinate, b);
    QCOMPARE(paths[0][3].terrainAMSL, 320.0);
}

void GeoZoneTest::_testAltitudeLimits()
{
    const QGeoCoordinate center(47, 8);
    const auto position = [](const QGeoCoordinate &coordinate, double altitudeAMSL, double terrainAMSL) {
        return GeoZone::Position{ coordinate, altitudeAMSL, terrainAMSL };
    };

    // Ground to 120 m above ground
    GeoZone::Volume low;
    low.isCircle = true;
    low.center = center;
    low.radius = 500;
    low.lowerLimit = 0;
    low.upperLimit = 120;
    low.setReferences(QStringLiteral("AGL"), QStringLiteral("AGL"));
    QVERIFY(GeoZone::verticalOverlap(low, position(center, 450, 400), position(center, 450, 400)));
    QVERIFY(!GeoZone::verticalOverlap(low, position(center, 600, 400), position(center, 600, 400)));
    // Slightly below the ground is still inside a zone from the ground
    QVERIFY(GeoZone::verticalOverlap(low, position(center, 399.9, 400), position(center, 399.9, 400)));
    QVERIFY(GeoZone::verticalOverlap(low, position(center, 600, 400), position(center, 600, 400), 100));
    QVERIFY(GeoZone::verticalOverlap(low, position(center, 450, 400), position(center, 700, 400)));
    // Unknown terrain or altitude can't be excluded
    QVERIFY(GeoZone::verticalOverlap(low, position(center, 600, qQNaN()), position(center, 600, qQNaN())));
    QVERIFY(GeoZone::verticalOverlap(low, GeoZone::Position{ center }, GeoZone::Position{ center }));

    // 1000 ft above mean sea level and higher, published in feet
    GeoZone::Volume high = low;
    high.feet = true;
    high.lowerLimit = 1000;
    high.upperLimit = 99999;
    high.setReferences(QStringLiteral("AMSL"), QStringLiteral("AMSL"));
    QVERIFY(!GeoZone::verticalOverlap(high, position(center, 300, 250), position(center, 300, 250)));
    QVERIFY(GeoZone::verticalOverlap(high, position(center, 310, 250), position(center, 310, 250)));
    QVERIFY(GeoZone::verticalOverlap(high, position(center, 9000, 250), position(center, 9000, 250)));

    // Heights above the WGS84 ellipsoid are about 48 m higher than above mean sea level in Zurich
    const QGeoCoordinate zurich(47.37, 8.54);
    QVERIFY(qAbs(GeoZoneGeometry::geoidHeight(zurich) - 47.3) < 2);
    QVERIFY(qAbs(GeoZoneGeometry::geoidHeight(QGeoCoordinate(42.39, -71.15)) + 28.6) < 2);
    QVERIFY(qAbs(GeoZoneGeometry::geoidHeight(QGeoCoordinate(5, 78)) + 104.7) < 2);
    QVERIFY(qIsNaN(GeoZoneGeometry::geoidHeight(QGeoCoordinate())));
    GeoZone::Volume ellipsoid = low;
    ellipsoid.lowerLimit = 500;
    ellipsoid.upperLimit = 1000;
    ellipsoid.setReferences(QStringLiteral("WGS84"), QStringLiteral("WGS84"));
    QVERIFY(GeoZone::verticalOverlap(ellipsoid, position(zurich, 460, 400), position(zurich, 460, 400)));
    QVERIFY(!GeoZone::verticalOverlap(ellipsoid, position(zurich, 440, 400), position(zurich, 440, 400)));

    // Flight levels can't be compared
    GeoZone::Volume flightLevel = high;
    flightLevel.setReferences(QStringLiteral("STD"), QStringLiteral("AMSL"));
    QCOMPARE(flightLevel.lowerReference, GeoZone::VerticalReference::FlightLevel);
    QVERIFY(GeoZone::verticalOverlap(flightLevel, position(center, 0, 0), position(center, 0, 0)));

    // Published codes are case insensitive and have aliases, the ground is shown as GND either way
    GeoZone::Volume surface = low;
    surface.setReferences(QStringLiteral(" sfc"), QStringLiteral("msl "));
    QCOMPARE(surface.lowerReference, GeoZone::VerticalReference::Ground);
    QCOMPARE(surface.upperReference, GeoZone::VerticalReference::MeanSeaLevel);
    QVERIFY(GeoZone::verticalOverlap(surface, position(center, 110, 400), position(center, 110, 400)));
    QVERIFY(!GeoZone::verticalOverlap(surface, position(center, 130, 0), position(center, 130, 0)));
    GeoZone::Data surfaceData;
    surfaceData.volumes.append(surface);
    QVERIFY(GeoZone(surfaceData).verticalLimitsText().startsWith(QStringLiteral("GND - ")));
    QCOMPARE(GeoZone::verticalReferenceFromString(QStringLiteral("FL")), GeoZone::VerticalReference::Unknown);

    GeoZone::Data data;
    data.volumes.append(low);
    const GeoZone zone(data);
    QVERIFY(zone.contains(position(center, 450, 400)));
    QVERIFY(!zone.contains(position(center, 600, 400)));
    QCOMPARE(zone.distanceTo(position(center.atDistanceAndAzimuth(600, 0), 450, 400)), zone.distanceTo(center.atDistanceAndAzimuth(600, 0)));
    QVERIFY(qIsInf(zone.distanceTo(position(center, 600, 400))));
    QCOMPARE(zone.distanceTo(position(center, 600, 400), 100), 0.0);

    // Paths above the ceiling pass, paths descending into the zone enter it
    const QGeoCoordinate west = center.atDistanceAndAzimuth(1000, 270);
    const QGeoCoordinate east = center.atDistanceAndAzimuth(1000, 90);
    QVERIFY(!zone.intersectsPath(QList<GeoZone::Position>{ position(west, 600, 400), position(east, 600, 400) }));
    QVERIFY(zone.intersectsPath(QList<GeoZone::Position>{ position(west, 600, 400), position(east, 450, 400) }));
    QVERIFY(zone.intersectsPath(QList<GeoZone::Position>{ position(west, 600, qQNaN()), position(east, 600, qQNaN()) }));
}

void GeoZoneTest::_testAreaIntersection()
{
    const QGeoCoordinate center(47, 8);
    const auto square = [](const QGeoCoordinate &squareCenter, double halfSide) {
        return QList<QGeoCoordinate>{ squareCenter.atDistanceAndAzimuth(halfSide * M_SQRT2, 45), squareCenter.atDistanceAndAzimuth(halfSide * M_SQRT2, 135),
                                      squareCenter.atDistanceAndAzimuth(halfSide * M_SQRT2, 225), squareCenter.atDistanceAndAzimuth(halfSide * M_SQRT2, 315) };
    };
    const auto area = [](const QList<QGeoCoordinate> &polygon, double lowest, double highest, double maxTerrain = qQNaN(), double minTerrain = qQNaN()) {
        return GeoZone::Area{ polygon, GeoZone::Position{ polygon.first(), lowest, maxTerrain }, GeoZone::Position{ polygon.first(), highest, minTerrain } };
    };
    const auto circleZone = [](const QGeoCoordinate &zoneCenter, double radius) {
        GeoZone::Volume volume;
        volume.isCircle = true;
        volume.center = zoneCenter;
        volume.radius = radius;
        GeoZone::Data data;
        data.volumes.append(volume);
        return data;
    };
    const QList<QGeoCoordinate> survey = square(center, 500);

    // A small zone between the flight lines of a survey
    QVERIFY(GeoZone(circleZone(center, 20)).intersectsArea(area(survey, 100, 200)));
    QVERIFY(GeoZone(circleZone(center.atDistanceAndAzimuth(540, 90), 50)).intersectsArea(area(survey, 100, 200)));
    QVERIFY(!GeoZone(circleZone(center.atDistanceAndAzimuth(600, 90), 50)).intersectsArea(area(survey, 100, 200)));

    // Polygon zones inside the area, around it, and with the area in a hole
    GeoZone::Data small;
    small.volumes.append(GeoZone::Volume{ false, { square(center.atDistanceAndAzimuth(200, 0), 10) } });
    QVERIFY(GeoZone(small).intersectsArea(area(survey, 100, 200)));
    GeoZone::Data large;
    large.volumes.append(GeoZone::Volume{ false, { square(center, 2000) } });
    QVERIFY(GeoZone(large).intersectsArea(area(survey, 100, 200)));
    large.volumes[0].rings.append(square(center, 1000));
    QVERIFY(!GeoZone(large).intersectsArea(area(survey, 100, 200)));
    QVERIFY(!GeoZone(small).intersectsArea(area(square(center.atDistanceAndAzimuth(3000, 0), 500), 100, 200)));

    // Vertical limits: 150 m above ground and higher, the area flown 80 to 150 m above the ground
    GeoZone::Data elevated = circleZone(center, 100);
    elevated.volumes[0].lowerLimit = 150;
    elevated.volumes[0].lowerReference = GeoZone::VerticalReference::Ground;
    QVERIFY(GeoZone(elevated).intersectsArea(area(survey, 400, 450, 320, 300)));
    elevated.volumes[0].lowerLimit = 160;
    QVERIFY(!GeoZone(elevated).intersectsArea(area(survey, 400, 450, 320, 300)));
    // Unknown terrain can't exclude the zone
    QVERIFY(GeoZone(elevated).intersectsArea(area(survey, 400, 450)));
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
        // On the leg from 1000 m east to 2000 m north
        _circleFeature(QStringLiteral("U"), QStringLiteral("USPACE"), center.atDistanceAndAzimuth(1118, 26.57), 200),
    });
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));

    // Far above the planned altitudes
    QJsonObject high = _circleFeature(QStringLiteral("HIGH"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(500, 90), 300);
    QJsonArray geometry = high[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("lowerLimit")] = 5000;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
    layer[QStringLiteral("upperLimit")] = 6000;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AMSL");
    geometry[0] = layer;
    high[QStringLiteral("geometry")] = geometry;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("high.json"), QJsonArray{ high }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();

    GeoZoneMissionCheck check(nullptr, &manager);
    check.setMissionController(missionController);

    // Passes the no restriction zone only, which is reported without a warning
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 180).atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 180).atDistanceAndAzimuth(1000, 90), 2);
    QTRY_COMPARE(check.permittedZones().count(), 1);
    QVERIFY(check.crossedZones().isEmpty());
    QVERIFY(check.warningText().isEmpty());
    QVERIFY(check.uSpaceZones().isEmpty());
    QVERIFY(check.infoText().startsWith(QStringLiteral("Areas without restriction: N.")));

    // Through the prohibited zone and then the conditional one, listed most restrictive first
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 3);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 4);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 0), 5);
    QTRY_COMPARE(check.crossedZones().count(), 2);
    QCOMPARE(qobject_cast<GeoZone*>(check.crossedZones()[0])->identifier(), QStringLiteral("P"));
    QCOMPARE(qobject_cast<GeoZone*>(check.crossedZones()[1])->identifier(), QStringLiteral("C"));
    QVERIFY(check.warningText().contains(QStringLiteral("P (Prohibited)")));
    // The path north also passes through the U-space airspace
    QCOMPARE(check.uSpaceZones().count(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("U-space airspace: U.")));
    QVERIFY(check.crossedZonesBoundingBox().contains(center));
    QVERIFY(check.crossedZonesBoundingBox().contains(center.atDistanceAndAzimuth(2000, 0)));

    // A survey whose area covers a zone its flight lines miss
    const QGeoCoordinate surveyCenter = center.atDistanceAndAzimuth(5000, 180);
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("survey.json"),
        QJsonArray{ _circleFeature(QStringLiteral("S"), QStringLiteral("PROHIBITED"), surveyCenter.atDistanceAndAzimuth(395, 45), 2) }), errorString), qPrintable(errorString));
    const int surveyIndex = missionController->visualItems()->count();
    SurveyComplexItem *const survey = qobject_cast<SurveyComplexItem*>(missionController->insertComplexMissionItem(SurveyComplexItem::name, surveyCenter, surveyIndex));
    QVERIFY(survey);
    survey->surveyAreaPolygon()->clear();
    survey->surveyAreaPolygon()->appendVertices({ surveyCenter.atDistanceAndAzimuth(424, 315), surveyCenter.atDistanceAndAzimuth(424, 45),
                                                  surveyCenter.atDistanceAndAzimuth(424, 135), surveyCenter.atDistanceAndAzimuth(424, 225) });
    const auto surveyZoneCrossed = [&check]() {
        const QList<QObject*> crossed = check.crossedZones();
        return std::any_of(crossed.cbegin(), crossed.cend(), [](const QObject *zone) {
            return qobject_cast<const GeoZone*>(zone)->identifier() == QStringLiteral("S");
        });
    };
    QTRY_VERIFY(surveyZoneCrossed());

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
    monitor.evaluate({ center.atDistanceAndAzimuth(1000, 0) });
    QCOMPARE(alertSpy.count(), 0);
    monitor.evaluate({ center.atDistanceAndAzimuth(380, 0) });
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Approaching geozone P")));
    QCOMPARE(monitor.nearbyZones().count(), 1);
    QVERIFY(!monitor.inside());
    QCOMPARE(monitor.nearbyZones()[0].toMap()[QStringLiteral("distance")].toInt(), 80);
    QCOMPARE(monitor.alertRestriction(), static_cast<int>(GeoZone::Prohibited));
    monitor.evaluate({ center.atDistanceAndAzimuth(350, 0) });
    QCOMPARE(alertSpy.count(), 1);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) });
    QCOMPARE(alertSpy.count(), 2);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Entered geozone P")));
    QVERIFY(monitor.inside());

    // Jitter across the boundary and the approach distance does not alert again
    monitor.evaluate({ center.atDistanceAndAzimuth(310, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(290, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(410, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(390, 0) });
    QCOMPARE(alertSpy.count(), 2);

    // Reloading changed zones doesn't repeat the approach alert of an unchanged zone
    (void) _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(3000, 270), 300),
        expired,
        _circleFeature(QStringLiteral("NEW"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(9000, 0), 300),
    });
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));
    QVERIFY(monitor.nearbyZones().isEmpty());
    monitor.evaluate({ center.atDistanceAndAzimuth(390, 0) });
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(monitor.nearbyZones().count(), 1);

    // Leaving the area resets the zone
    monitor.evaluate({ center.atDistanceAndAzimuth(1000, 0) });
    QVERIFY(monitor.nearbyZones().isEmpty());
    QCOMPARE(monitor.alertRestriction(), -1);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) });
    QCOMPARE(alertSpy.count(), 3);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Entered geozone P")));

    // Unrestricted and inactive zones don't alert
    monitor.evaluate({ center.atDistanceAndAzimuth(3000, 270) });
    monitor.evaluate({ center.atDistanceAndAzimuth(3000, 90) });
    QCOMPARE(alertSpy.count(), 3);

    // Zones are forgotten before the manager deletes them
    manager.clear();
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 3);

    // U-space airspace alerts too, but a prohibited zone ahead decides the indicator color
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("uspace.json"),
        QJsonArray{ _circleFeature(QStringLiteral("U"), QStringLiteral("USPACE"), center.atDistanceAndAzimuth(6000, 0), 3000),
                    _circleFeature(QStringLiteral("P2"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(6380, 0), 300) }), errorString), qPrintable(errorString));
    alertSpy.clear();
    monitor.evaluate({ center.atDistanceAndAzimuth(5000, 0) });
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Entered geozone U (U-space airspace)")));
    QCOMPARE(monitor.alertRestriction(), static_cast<int>(GeoZone::USpace));
    monitor.evaluate({ center.atDistanceAndAzimuth(6000, 0) });
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(monitor.alertRestriction(), static_cast<int>(GeoZone::Prohibited));
    QVERIFY(monitor.inside());
    manager.clear();

    // A zone from 120 m above ground: flying below it does not enter it, climbing towards it approaches it
    QJsonObject elevated = _circleFeature(QStringLiteral("H"), QStringLiteral("PROHIBITED"), center, 300);
    QJsonArray geometry = elevated[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("lowerLimit")] = 120;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AGL");
    layer[QStringLiteral("upperLimit")] = 500;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AGL");
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    geometry[0] = layer;
    elevated[QStringLiteral("geometry")] = geometry;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("elevated.json"), QJsonArray{ elevated }), errorString), qPrintable(errorString));
    alertSpy.clear();
    monitor.evaluate({ center, 450, 400 });
    QCOMPARE(alertSpy.count(), 0);
    monitor.evaluate({ center, 500, 400 });
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Approaching geozone H")));
    monitor.evaluate({ center, 530, 400 });
    QCOMPARE(alertSpy.count(), 2);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Entered geozone H")));
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

void GeoZoneTest::_testSettings()
{
    GeoZoneSettings *const settings = SettingsManager::instance()->geoZoneSettings();
    const QList<Fact*> facts = { settings->showZones(), settings->approachDistance(), settings->staleDays(),
                                 settings->feedRefreshHours(), settings->liveFeedRefreshMinutes() };
    const auto restoreDefaults = qScopeGuard([&facts]() {
        for (Fact *fact : facts) {
            fact->setRawValue(fact->rawDefaultValue());
        }
    });

    // The map visibility of earlier versions is kept
    QSettings qSettings;
    qSettings.setValue(QStringLiteral("GeoZoneManager/visible"), false);
    qSettings.remove(QStringLiteral("GeoZoneManager/showZones"));
    {
        const GeoZoneSettings migrated;
    }
    QVERIFY(!qSettings.contains(QStringLiteral("GeoZoneManager/visible")));
    QCOMPARE(qSettings.value(QStringLiteral("GeoZoneManager/showZones")), QVariant(false));
    qSettings.remove(QStringLiteral("GeoZoneManager/showZones"));

    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QGeoCoordinate center(47, 8);
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300) });
    QFile file(fileName);
    QVERIFY(file.open(QIODevice::ReadWrite) && file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-10), QFileDevice::FileModificationTime));
    file.close();
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));

    // The manager visibility is the setting
    QSignalSpy visibleSpy(&manager, &GeoZoneManager::visibleChanged);
    settings->showZones()->setRawValue(false);
    QVERIFY(!manager.visible());
    QCOMPARE(visibleSpy.count(), 1);
    manager.setVisible(true);
    QVERIFY(settings->showZones()->rawValue().toBool());

    // Outdated data is reported at the configured age
    QVERIFY(manager.validityWarning().isEmpty());
    settings->staleDays()->setRawValue(7);
    QCOMPARE(manager.validityWarning(), QStringLiteral("zones.json: Data is 10 days old"));

    // Feeds are refreshed at the configured intervals
    GeoZoneSource feed(QStringLiteral("https://example.com/zones.json"), QStringLiteral("https://example.com/zones.json"));
    GeoZoneParser::Result result;
    result.format = GeoZoneParser::Format::ED318;
    feed.setResult(result, QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc());
    settings->feedRefreshHours()->setRawValue(6);
    QCOMPARE(feed.refreshIntervalSecs(), 6 * 3600);
    result.format = GeoZoneParser::Format::FaaTfr;
    feed.setResult(result, QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc());
    settings->liveFeedRefreshMinutes()->setRawValue(30);
    QCOMPARE(feed.refreshIntervalSecs(), 30 * 60);

    // Approach alerts at the configured distance
    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.evaluate({ center.atDistanceAndAzimuth(500, 0) });
    QCOMPARE(alertSpy.count(), 0);
    settings->approachDistance()->setRawValue(250);
    monitor.evaluate({ center.atDistanceAndAzimuth(500, 0) });
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Approaching geozone P")));
}

void GeoZoneTest::_testMonitorMultipleVehicles()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300) }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.setActiveVehicleId(1);

    // A single vehicle isn't named
    monitor.evaluate({ center.atDistanceAndAzimuth(380, 0) }, 1);
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Approaching geozone P")));
    QCOMPARE(monitor.nearbyZones().count(), 1);

    // Each vehicle alerts on its own and is named while several are monitored
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) }, 2);
    QCOMPARE(alertSpy.count(), 2);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Vehicle 2: Entered geozone P")));
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) }, 1);
    QCOMPARE(alertSpy.count(), 3);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Vehicle 1: Entered geozone P")));
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) }, 2);
    QCOMPARE(alertSpy.count(), 3);

    // The nearby zones are those of the active vehicle
    monitor.evaluate({ center.atDistanceAndAzimuth(5000, 0) }, 2);
    QVERIFY(monitor.inside());
    QSignalSpy nearbySpy(&monitor, &GeoZoneMonitor::nearbyZonesChanged);
    monitor.setActiveVehicleId(2);
    QCOMPARE(nearbySpy.count(), 1);
    QVERIFY(monitor.nearbyZones().isEmpty());
    QVERIFY(!monitor.inside());

    // A removed vehicle alerts again when it reconnects, the others keep their state
    monitor.removeVehicle(1);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) }, 1);
    QCOMPARE(alertSpy.count(), 4);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Vehicle 1: Entered geozone P")));
}

void GeoZoneTest::_testAlertLog()
{
    GeoZoneSettings *const settings = SettingsManager::instance()->geoZoneSettings();
    const auto restoreDefault = qScopeGuard([settings]() { settings->logAlerts()->setRawValue(settings->logAlerts()->rawDefaultValue()); });

    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QJsonObject feature = _circleFeature(QStringLiteral("P1"), QStringLiteral("PROHIBITED"), center, 300);
    feature[QStringLiteral("name")] = QStringLiteral("Prison \"West\", Zurich");
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ feature }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    GeoZoneAlertLog log;
    log.setDirectory(dir.filePath(QStringLiteral("logs")));
    (void) connect(&monitor, &GeoZoneMonitor::alertEvent, &log, &GeoZoneAlertLog::append);

    monitor.evaluate({ center.atDistanceAndAzimuth(380, 0), 520, 400 }, 3);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) }, 3);

    QFile file(log.filePath(QDateTime::currentDateTimeUtc().date()));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(file.fileName()));
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    file.close();
    QCOMPARE(lines.count(), 3);
    QCOMPARE(lines[0], GeoZoneAlertLog::header());
    QStringList fields = lines[1].split(QLatin1Char(','));
    QCOMPARE(fields[1], QStringLiteral("3"));
    QCOMPARE(fields[4], QStringLiteral("520.0"));
    QCOMPARE(fields[5], QStringLiteral("400.0"));
    QCOMPARE(fields[6], QStringLiteral("approaching"));
    QCOMPARE(fields[7], QStringLiteral("80"));
    QCOMPARE(fields[8], QStringLiteral("P1"));
    // Quoted name with a comma and quotes, then the restriction and source
    QVERIFY(lines[1].contains(QStringLiteral(",\"Prison \"\"West\"\", Zurich\",Prohibited,")));
    QVERIFY(lines[1].contains(QStringLiteral("zones.json")));
    fields = lines[2].split(QLatin1Char(','));
    QCOMPARE(fields[4], QString());
    QCOMPARE(fields[6], QStringLiteral("entered"));
    QCOMPARE(fields[7], QStringLiteral("0"));

    // Turned off
    settings->logAlerts()->setRawValue(false);
    monitor.evaluate({ center.atDistanceAndAzimuth(5000, 0) }, 3);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) }, 3);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts).count(), 3);
}

void GeoZoneTest::_testProfileBands()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const auto withLimits = [](QJsonObject feature, double lower, const QString &lowerReference, double upper, const QString &upperReference, const QString &units) {
        QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
        QJsonObject layer = geometry[0].toObject();
        layer[QStringLiteral("lowerLimit")] = lower;
        layer[QStringLiteral("lowerVerticalReference")] = lowerReference;
        layer[QStringLiteral("upperLimit")] = upper;
        layer[QStringLiteral("upperVerticalReference")] = upperReference;
        layer[QStringLiteral("uomDimensions")] = units;
        geometry[0] = layer;
        feature[QStringLiteral("geometry")] = geometry;
        return feature;
    };
    // From the ground to 120 m above it in the middle, 1000 to 2000 ft AMSL over the east end, a zone without
    // restriction and one away from the path
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        withLimits(_circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300), 0, QStringLiteral("AGL"), 120, QStringLiteral("AGL"), QStringLiteral("M")),
        // ED-269 circle radii are in the layer units too, 200 m
        withLimits(_circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1000, 90), 200 / 0.3048), 1000, QStringLiteral("AMSL"), 2000, QStringLiteral("AMSL"), QStringLiteral("FT")),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center, 500),
        _circleFeature(QStringLiteral("F"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 0), 300),
        _circleFeature(QStringLiteral("END"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(179000, 90), 300),
    });
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));

    // West to east over flat ground at 400 m, then a pattern without a flight path, then east again without terrain
    GeoZoneProfile::Segment west;
    west.from = center.atDistanceAndAzimuth(1000, 270);
    west.to = center;
    west.distance = 1000;
    west.terrainSpacing = 100;
    west.terrainAMSL = QList<double>(11, 400);
    GeoZoneProfile::Segment east = west;
    east.from = center;
    east.to = center.atDistanceAndAzimuth(1000, 90);
    GeoZoneProfile::Segment gap;
    gap.distance = 250;
    GeoZoneProfile::Segment beyond;
    beyond.from = center.atDistanceAndAzimuth(1000, 90);
    beyond.to = center.atDistanceAndAzimuth(1300, 90);
    beyond.distance = 300;

    const QVariantList bands = GeoZoneProfile::bandsForSegments({ west, east, gap, beyond }, &manager);
    QStringList names;
    for (const QVariant &band : bands) {
        names.append(band.toMap()[QStringLiteral("name")].toString());
    }
    // Most restrictive last, then along the path. The gap splits the stretch through A.
    QCOMPARE(names, QStringList({ QStringLiteral("A"), QStringLiteral("A"), QStringLiteral("P") }));

    const auto points = [&bands](int index) { return bands[index].toMap()[QStringLiteral("points")].toList(); };
    const auto value = [](const QVariant &point, const char *key) { return point.toMap()[QString::fromLatin1(key)].toDouble(); };

    // Continuous across the segments through the middle, from the ground to 120 m above it
    const QVariantList prohibited = points(2);
    QVERIFY(qAbs(value(prohibited.first(), "distance") - 700) <= GeoZoneProfile::minSampleSpacing);
    QVERIFY(qAbs(value(prohibited.last(), "distance") - 1300) <= GeoZoneProfile::minSampleSpacing);
    QVERIFY(qIsNaN(value(prohibited.first(), "floor")));
    QCOMPARE(value(prohibited.first(), "ceiling"), 520.0);

    const QVariantList authorisation = points(0);
    QVERIFY(qAbs(value(authorisation.first(), "distance") - 1800) <= GeoZoneProfile::minSampleSpacing);
    QCOMPARE(value(authorisation.last(), "distance"), 2000.0);
    QCOMPARE(value(authorisation.first(), "floor"), 1000 * 0.3048);
    QCOMPARE(value(authorisation.first(), "ceiling"), 2000 * 0.3048);
    // After the gap, beyond the end of the segment through its center
    const QVariantList afterGap = points(1);
    QCOMPARE(value(afterGap.first(), "distance"), 2250.0);
    QVERIFY(qAbs(value(afterGap.last(), "distance") - 2450) <= GeoZoneProfile::minSampleSpacing);

    // Limits above unknown terrain are unknown
    const QVariantList unknownTerrain = GeoZoneProfile::bandsForSegments({ beyond }, &manager);
    QCOMPARE(unknownTerrain.count(), 1);
    beyond.from = center.atDistanceAndAzimuth(100, 270);
    beyond.to = center.atDistanceAndAzimuth(100, 90);
    const QVariantList noTerrain = GeoZoneProfile::bandsForSegments({ beyond }, &manager);
    QCOMPARE(noTerrain.count(), 1);
    QVERIFY(qIsNaN(value(noTerrain[0].toMap()[QStringLiteral("points")].toList().first(), "ceiling")));

    // Long paths are sampled more sparsely up to their end
    QList<GeoZoneProfile::Segment> longPath;
    for (int i = 0; i < 60; i++) {
        GeoZoneProfile::Segment leg;
        leg.from = center.atDistanceAndAzimuth(i * 3000, 90);
        leg.to = center.atDistanceAndAzimuth((i + 1) * 3000, 90);
        leg.distance = 3000;
        longPath.append(leg);
    }
    const QVariantList longBands = GeoZoneProfile::bandsForSegments(longPath, &manager);
    const auto end = std::find_if(longBands.cbegin(), longBands.cend(), [](const QVariant &band) {
        return band.toMap()[QStringLiteral("name")].toString() == QStringLiteral("END");
    });
    QVERIFY(end != longBands.cend());
    const QVariantList endPoints = end->toMap()[QStringLiteral("points")].toList();
    const double longSpacing = 180000.0 / GeoZoneProfile::maxSamples;
    QVERIFY(qAbs(value(endPoints.first(), "distance") - 178700) <= longSpacing + 1);
    QVERIFY(qAbs(value(endPoints.last(), "distance") - 179300) <= longSpacing + 1);
}

void GeoZoneTest::_testDownloadSizeLimit()
{
    // Serves 4000 bytes, announcing the size or not
    const QByteArray body(4000, ' ');
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    (void) connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (QTcpSocket *const socket = server.nextPendingConnection()) {
            (void) connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) {
                    return;
                }
                const QByteArray length = request.startsWith("GET /sized") ? (QByteArray("Content-Length: ") + QByteArray::number(body.size()) + "\r\n") : QByteArray();
                (void) socket->write(QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n") + length + "\r\n" + body);
                socket->disconnectFromHost();
            });
            (void) connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    GeoZoneDownloader downloader;
    const auto download = [&downloader, &server](const QString &path) {
        GeoZoneDownloader::Result result;
        bool done = false;
        downloader.get(QStringLiteral("http://127.0.0.1:%1/%2").arg(server.serverPort()).arg(path), QString(), QString(),
                       [&result, &done](const GeoZoneDownloader::Result &downloadResult) { result = downloadResult; done = true; });
        if (!QTest::qWaitFor([&done]() { return done; }, 10000)) {
            result.errorString = QStringLiteral("timeout");
        }
        return result;
    };

    GeoZoneDownloader::Result result = download(QStringLiteral("sized"));
    QCOMPARE(result.status, GeoZoneDownloader::Result::Status::Ok);
    QCOMPARE(result.data.size(), body.size());

    downloader.setMaxBytes(1000);
    for (const QString &path : { QStringLiteral("sized"), QStringLiteral("unsized") }) {
        result = download(path);
        QCOMPARE(result.status, GeoZoneDownloader::Result::Status::Failed);
        QVERIFY2(result.errorString.startsWith(QStringLiteral("The download is larger than")), qPrintable(result.errorString));
        QVERIFY(result.data.isEmpty());
    }
    QCOMPARE(downloader.count(), 0);
}

namespace {

/// Replaces a random value in the JSON tree with one of another type or an extreme value
QJsonValue _mutateJson(const QJsonValue &value, QRandomGenerator &random, int depth = 0)
{
    const auto randomValue = [&random]() -> QJsonValue {
        switch (random.bounded(14)) {
        case 0:  return QJsonValue();
        case 1:  return QJsonValue(QJsonValue::Undefined);
        case 2:  return QString();
        case 3:  return QStringLiteral("NaN");
        case 4:  return -1;
        case 5:  return 0;
        case 6:  return 1e308;
        case 7:  return -1e308;
        case 8:  return QJsonArray();
        case 9:  return QJsonObject();
        case 10: return QJsonArray{ QJsonArray{ 1e300, -1e300 }, QJsonArray{ 0, 0 } };
        case 11: return QJsonArray{ QJsonArray{ QJsonArray{ 8, 47 }, QJsonArray{ 8, 47 }, QJsonArray{ 8, 47 } } };
        case 12: return QStringLiteral("2026-13-45T99:99:99Z");
        default: return QString(1000, QLatin1Char('x'));
        }
    };

    if (value.isObject() && (depth < 12)) {
        QJsonObject object = value.toObject();
        if (object.isEmpty() || (random.bounded(8) == 0)) {
            return randomValue();
        }
        const QStringList keys = object.keys();
        const QString key = keys[random.bounded(keys.count())];
        if (random.bounded(6) == 0) {
            object.remove(key);
        } else {
            object[key] = _mutateJson(object[key], random, depth + 1);
        }
        return object;
    }
    if (value.isArray() && (depth < 12)) {
        QJsonArray array = value.toArray();
        if (array.isEmpty() || (random.bounded(8) == 0)) {
            return randomValue();
        }
        const int index = random.bounded(static_cast<int>(array.count()));
        switch (random.bounded(6)) {
        case 0:
            array.removeAt(index);
            break;
        case 1:
            array.append(array[index]);
            break;
        default:
            array[index] = _mutateJson(array[index], random, depth + 1);
            break;
        }
        return array;
    }
    return randomValue();
}

} // namespace

void GeoZoneTest::_testParserFuzz()
{
    // Mutated documents must load or fail cleanly, and loaded zones must be usable
    QRandomGenerator random(20261031);
    const QGeoCoordinate probe(47, 8);
    int loadedCount = 0;
    for (const QString &name : { QStringLiteral("ed269_test.json"), QStringLiteral("ed318_test.json"), QStringLiteral("faa_tfr_test.json"), QStringLiteral("ch_zurich_ed269.json") }) {
        QFile file(QStringLiteral(":/unittest/") + name);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray original = file.readAll();
        const QJsonValue document = QJsonDocument::fromJson(original).isObject() ?
                                        QJsonValue(QJsonDocument::fromJson(original).object()) : QJsonValue(QJsonDocument::fromJson(original).array());
        const GeoZoneParser::Format hint = name.startsWith(QStringLiteral("faa")) ? GeoZoneParser::Format::FaaTfr : GeoZoneParser::Format::Unknown;

        for (int i = 0; i < 300; i++) {
            QByteArray bytes;
            if (i % 10 == 0) {
                // Raw corruption: truncation and flipped bytes
                bytes = original.left(random.bounded(static_cast<int>(original.size())));
                for (int flips = random.bounded(5); flips > 0 && !bytes.isEmpty(); flips--) {
                    bytes[random.bounded(static_cast<int>(bytes.size()))] = static_cast<char>(random.bounded(256));
                }
            } else {
                QJsonValue mutated = document;
                for (int mutations = 1 + random.bounded(4); mutations > 0; mutations--) {
                    mutated = _mutateJson(mutated, random);
                }
                bytes = mutated.isObject() ? QJsonDocument(mutated.toObject()).toJson(QJsonDocument::Compact) :
                        (mutated.isArray() ? QJsonDocument(mutated.toArray()).toJson(QJsonDocument::Compact) : QByteArray("1"));
            }

            GeoZoneParser::Result result;
            QString errorString;
            if (!GeoZoneParser::parse(bytes, result, errorString, hint)) {
                QVERIFY(!errorString.isEmpty());
                continue;
            }
            loadedCount++;
            for (const GeoZone::Data &data : std::as_const(result.zones)) {
                const GeoZone zone(data);
                (void) zone.contains(GeoZone::Position{ probe, 500, 400 });
                (void) zone.distanceTo(GeoZone::Position{ probe, 500, 400 }, 30);
                (void) zone.intersectsPath(QList<QGeoCoordinate>{ probe, probe.atDistanceAndAzimuth(5000, 45) });
                (void) zone.exclusionPolygons(25);
                (void) zone.exclusionCircles();
                (void) zone.isActiveDuring(QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc().addDays(2));
                (void) zone.verticalLimitsText();
                (void) zone.applicabilityText();
                QVERIFY(!zone.boundingBox().isValid() || (qAbs(zone.boundingBox().center().latitude()) <= 90));
            }
        }
    }
    // Many mutations still yield documents with zones, so the zone functions are exercised too
    QVERIFY2(loadedCount > 400, qPrintable(QString::number(loadedCount)));
}
