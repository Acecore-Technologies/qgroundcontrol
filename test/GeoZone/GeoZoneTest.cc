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
#include "GeoZoneManager.h"
#include "GeoZoneParser.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimeZone>
#include <QtTest/QTest>

#include <algorithm>

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

void GeoZoneTest::_testDisplayText()
{
    const GeoZoneParser::Result ed269 = _parseResource(QStringLiteral("ed269_test.json"));
    QCOMPARE(ed269.zones.count(), 3);

    const GeoZone prohibited(ed269.zones[0]);
    QCOMPARE(prohibited.verticalLimitsText(), QStringLiteral("GND - 120 m AGL\n120 m AGL - Unlimited"));
    QCOMPARE(prohibited.applicabilityText(), QStringLiteral("Permanent"));
    QCOMPARE(prohibited.reasonText(), QStringLiteral("Air traffic, Sensitive area"));

    const GeoZone circle(ed269.zones[1]);
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
    const QString sourceFile = sourceDir.filePath(QStringLiteral("zones.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), sourceFile));

    {
        GeoZoneManager manager(storageDir.path());
        QCOMPARE(manager.zones()->count(), 0);

        QString errorString;
        QVERIFY2(manager.loadFile(sourceFile, errorString), qPrintable(errorString));
        QCOMPARE(manager.zones()->count(), 3);
        QCOMPARE(manager.polygonShapes()->count(), 4);
        QCOMPARE(manager.circleShapes()->count(), 1);
        QCOMPARE(manager.formatName(), QStringLiteral("ED-318"));
        QCOMPARE(manager.sourceName(), QStringLiteral("Test GeoZones"));
        QCOMPARE(manager.skippedCount(), 1);
        QVERIFY(manager.boundingBox().isValid());
        QVERIFY(QFile::exists(manager.storedFilePath()));

        // A failed load keeps the current zones
        QVERIFY(!manager.loadFile(sourceDir.filePath(QStringLiteral("missing.json")), errorString));
        QVERIFY(!errorString.isEmpty());
        QCOMPARE(manager.zones()->count(), 3);

        QCOMPARE(manager.zonesInArea({ GeoZone::Prohibited }, QGeoRectangle()).count(), 1);
        const QGeoRectangle ireland(QGeoCoordinate(52.0, -9.0), QGeoCoordinate(51.0, -8.0));
        const QList<GeoZone*> zonesInArea = manager.zonesInArea({ GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::USpace }, ireland);
        QCOMPARE(zonesInArea.count(), 1);
        QCOMPARE(zonesInArea[0]->name(), QStringLiteral("Control zone"));
    }

    {
        // Stored copy is reloaded on startup
        GeoZoneManager manager(storageDir.path());
        QCOMPARE(manager.zones()->count(), 3);
        QCOMPARE(manager.sourceName(), QStringLiteral("Test GeoZones"));

        manager.clear();
        QCOMPARE(manager.zones()->count(), 0);
        QCOMPARE(manager.polygonShapes()->count(), 0);
        QCOMPARE(manager.circleShapes()->count(), 0);
        QVERIFY(!QFile::exists(manager.storedFilePath()));
    }

    GeoZoneManager manager(storageDir.path());
    QCOMPARE(manager.zones()->count(), 0);
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

void GeoZoneTest::_testLabelZonesLimit()
{
    // Many zones centered in the same small area, alternating restrictions
    QJsonArray features;
    const int zoneCount = GeoZoneManager::maxLabelCount * 2;
    for (int i = 0; i < zoneCount; i++) {
        const double lon = 8.0 + (i % 40) * 0.01;
        const double lat = 47.0 + (i / 40) * 0.01;
        features.append(QJsonObject{
            { "identifier", QStringLiteral("Z%1").arg(i) },
            { "restriction", (i % 2) ? QStringLiteral("NO_RESTRICTION") : QStringLiteral("PROHIBITED") },
            { "geometry", QJsonArray{ QJsonObject{ { "horizontalProjection", QJsonObject{
                { "type", "Circle" }, { "center", QJsonArray{ lon, lat } }, { "radius", 100 } } } } } },
        });
    }
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fileName = dir.filePath(QStringLiteral("many.json"));
    QFile file(fileName);
    QVERIFY(file.open(QIODevice::WriteOnly));
    (void) file.write(QJsonDocument(QJsonObject{ { "features", features } }).toJson(QJsonDocument::Compact));
    file.close();

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
