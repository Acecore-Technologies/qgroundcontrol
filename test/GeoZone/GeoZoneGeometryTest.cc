/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneGeometryTest.h"
#include "GeoZoneTestHelpers.h"
#include "GeoZoneFences.h"

using namespace GeoZoneTestHelpers;

void GeoZoneGeometryTest::_testIsActiveAt()
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

void GeoZoneGeometryTest::_testDaylightEvents()
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

void GeoZoneGeometryTest::_testIsActiveDuring()
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
    QVERIFY(!scheduled.isActiveAt(_utc(QDate(2026, 3, 23), QTime(10, 0))));
    QVERIFY(!scheduled.isActiveDuring(_utc(QDate(2026, 3, 23), QTime(0, 0)), _utc(QDate(2026, 3, 29), QTime(23, 0))));
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

    // The catalog filters zones by the activity window
    GeoZoneSource source(QStringLiteral("ed269_test.json"), QString());
    source.setResult(ed269, QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc());
    GeoZoneCatalog catalog;
    catalog.rebuild({ &source });
    const GeoZonePolicy all({ GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::NoRestriction, GeoZone::USpace, GeoZone::Unknown });
    QCOMPARE(catalog.zonesInArea(QGeoRectangle(), all).count(), 3);
    QCOMPARE(catalog.zonesInArea(QGeoRectangle(), all.activeDuring(_utc(monday.addDays(2), QTime(10, 0)))).count(), 2);
    QCOMPARE(catalog.zonesInArea(QGeoRectangle(), all.activeDuring(_utc(monday.addDays(2), QTime(10, 0)), _utc(monday.addDays(7), QTime(10, 0)))).count(), 3);
}

void GeoZoneGeometryTest::_testOutwardSimplification()
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

void GeoZoneGeometryTest::_testSpatialIndex()
{
    // Random circles from a few meters to a hundred kilometers, and a box crossing many cells
    ZoneOwner zoneParent;
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
        zones.append(zoneParent.add(data));
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

void GeoZoneGeometryTest::_testExclusionShapes()
{
    const GeoZoneParser::Result ed269 = _parseResource(QStringLiteral("ed269_test.json"));
    const GeoZoneParser::Result ed318 = _parseResource(QStringLiteral("ed318_test.json"));
    QCOMPARE(ed269.zones.count(), 3);
    QCOMPARE(ed318.zones.count(), 3);

    // Both volumes share the footprint, and the counter-clockwise ring is reversed to clockwise
    const GeoZone prohibited(ed269.zones[0]);
    const QList<QList<QGeoCoordinate>> polygons = prohibited.exclusionPolygons();
    QCOMPARE(polygons.count(), 1);
    QList<QGeoCoordinate> expected = GeoZone::toCoordinates(ed269.zones[0].volumes[0].rings[0]);
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

void GeoZoneGeometryTest::_testFenceCapacity()
{
    using namespace GeoZoneFences;

    // Items and ArduPilot storage of polygons, circles and the return point
    Usage usage;
    usage.addPolygon(4);
    usage.addCircle();
    usage.addReturnPoint();
    QCOMPARE(usage, (Usage{ 6, 34 + 13 + 9, 4 }));

    // PX4 stores 64 items
    Usage polygon64;
    polygon64.addPolygon(64);
    QVERIFY(Capacity::px4().fits(polygon64));
    QVERIFY(!Capacity::px4().fits(polygon64 + usage));

    // ArduPilot copters store 70 items in 560 bytes, so the polygon bytes fill the storage before the items
    const Capacity copter = Capacity::arduPilot(true);
    QCOMPARE(copter.maxItems, 70);
    QVERIFY(copter.estimated);
    Usage polygon69;
    polygon69.addPolygon(69);
    Usage polygon70;
    polygon70.addPolygon(70);
    QVERIFY(copter.fits(polygon69));
    QVERIFY(!copter.fits(polygon70));
    QCOMPARE(Capacity::arduPilot(false).maxItems, 84);
    // Fences stored on the SD card hold more, but no polygon of more than 255 vertices
    const Capacity sdCard = Capacity::arduPilot(true, 4);
    QVERIFY(!sdCard.estimated);
    QCOMPARE(sdCard.maxItems, 512);
    Usage polygon255;
    polygon255.addPolygon(255);
    Usage polygon256;
    polygon256.addPolygon(256);
    QVERIFY(sdCard.fits(polygon255));
    QVERIFY(!sdCard.fits(polygon256));
    // Without a vehicle the fence fits both, without circles
    QVERIFY(!Capacity::conservative().circles);
    QVERIFY(!Capacity::conservative().fits(polygon70));
    QVERIFY(Capacity::unsupported().fits(Usage()));
    QVERIFY(!Capacity::unsupported().fits(usage));

    // Circles become polygons enclosing them for vehicles without fence circles, fewer vertices when simplified
    ZoneOwner owner;
    GeoZone::Volume circleVolume;
    circleVolume.isCircle = true;
    circleVolume.center = QGeoCoordinate(47, 8);
    circleVolume.radius = 500;
    GeoZone::Data circleData;
    circleData.restriction = GeoZone::Prohibited;
    circleData.volumes = { circleVolume };
    const GeoZone *const circle = owner.add(circleData);
    const ZoneFences asCircle = zoneFences(circle);
    QCOMPARE(asCircle.circles.count(), 1);
    QVERIFY(asCircle.polygons.isEmpty());
    const ZoneFences asPolygon = zoneFences(circle, 0, true);
    QVERIFY(asPolygon.circles.isEmpty());
    QCOMPARE(asPolygon.polygons.count(), 1);
    const ZoneFences simplifiedPolygon = zoneFences(circle, 50, true);
    QVERIFY(simplifiedPolygon.polygons.first().count() < asPolygon.polygons.first().count());
    QVERIFY(simplifiedPolygon.polygons.first().count() >= minCircleVertices);
    for (const ZoneFences &fences : { asPolygon, simplifiedPolygon }) {
        QVERIFY(fencesCover(fences, circle));
        for (const QGeoCoordinate &vertex : fences.polygons.first()) {
            QVERIFY(vertex.distanceTo(circleVolume.center) > circleVolume.radius);
        }
        // Edges stay outside the circle
        const GeoZoneGeometry::LocalProjection projection(circleVolume.center);
        QVERIFY(GeoZoneGeometry::distanceToRing(QPointF(0, 0), projection.toLocal(fences.polygons.first())) >= circleVolume.radius);
    }
    QVERIFY(GeoZoneGeometry::signedArea(asPolygon.polygons.first()) < 0);

    // Simplified fences of a jagged polygon never cut into it, whatever the tolerance
    QList<QGeoCoordinate> star;
    for (int i = 0; i < 120; i++) {
        const double radius = ((i % 2) ? 400 : 380) + ((i % 30) < 15 ? 250 : 0);
        star.append(QGeoCoordinate(47, 8).atDistanceAndAzimuth(radius, i * 360.0 / 120));
    }
    GeoZone::Volume starVolume;
    starVolume.rings.append(GeoZone::toRing(star));
    GeoZone::Data starData;
    starData.restriction = GeoZone::Prohibited;
    starData.volumes = { starVolume };
    const GeoZone *const starZone = owner.add(starData);
    qsizetype previousCount = star.count() + 1;
    for (const double tolerance : { 0.0, 10.0, 25.0, 50.0, 100.0 }) {
        const ZoneFences fences = zoneFences(starZone, tolerance);
        QCOMPARE(fences.polygons.count(), 1);
        QVERIFY(fences.polygons.first().count() <= previousCount);
        previousCount = fences.polygons.first().count();
        QVERIFY(fencesCover(fences, starZone));
        const GeoZoneGeometry::LocalProjection projection(star.first());
        const QList<QPointF> ring = projection.toLocal(fences.polygons.first());
        for (const QGeoCoordinate &vertex : star) {
            const QPointF point = projection.toLocal(vertex);
            QVERIFY(GeoZoneGeometry::ringContains(ring, point) || (GeoZoneGeometry::distanceToRing(point, ring) <= boundaryTolerance));
        }
    }
    QVERIFY(previousCount < star.count() / 4);
}

void GeoZoneGeometryTest::_testFenceSelection()
{
    using namespace GeoZoneFences;

    ZoneOwner owner;
    const QGeoCoordinate origin(47, 8);
    const auto polygonZone = [&owner, &origin](const QString &name, GeoZone::Restriction restriction, double east, double north, int vertexCount, double radius = 200) {
        GeoZone::Volume volume;
        volume.rings.append(GeoZone::toRing(_circleRing(origin.atDistanceAndAzimuth(north, 0).atDistanceAndAzimuth(east, 90), radius, vertexCount)));
        GeoZone::Data data;
        data.name = name;
        data.restriction = restriction;
        data.volumes = { volume };
        return owner.add(data);
    };

    // A flight path heading east from the origin
    const QList<QList<QGeoCoordinate>> flight = { { origin, origin.atDistanceAndAzimuth(10000, 90) } };
    const GeoZone *const enteredRound = polygonZone(QStringLiteral("Prohibited entered"), GeoZone::Prohibited, 3000, 0, 40);
    const GeoZone *const nearProhibited = polygonZone(QStringLiteral("Prohibited near"), GeoZone::Prohibited, 5000, 600, 8);
    const GeoZone *const farProhibited = polygonZone(QStringLiteral("Prohibited far"), GeoZone::Prohibited, 5000, 5000, 4);
    const GeoZone *const fartherProhibited = polygonZone(QStringLiteral("Prohibited farther"), GeoZone::Prohibited, 5000, 8000, 4);
    const GeoZone *const enteredRestricted = polygonZone(QStringLiteral("Authorisation entered"), GeoZone::ReqAuthorisation, 7000, 0, 4);
    const GeoZone *const nearConditional = polygonZone(QStringLiteral("Conditional near"), GeoZone::Conditional, 1000, -500, 12);
    const GeoZone *const smallConditional = polygonZone(QStringLiteral("Conditional small"), GeoZone::Conditional, 1000, -5000, 4);
    const GeoZone *const uSpace = polygonZone(QStringLiteral("U-space"), GeoZone::USpace, 0, 0, 4, 2000);
    const GeoZone *const overflown = polygonZone(QStringLiteral("Overflown"), GeoZone::Prohibited, 9000, 0, 4);
    const GeoZone *const duplicate = polygonZone(QStringLiteral("Prohibited far copy"), GeoZone::Prohibited, 5000, 5000, 4);

    // The evaluated paths enter some zones and only pass above one
    GeoZonePathEvaluator::Result evaluation;
    for (const GeoZone *zone : { enteredRound, enteredRestricted, uSpace }) {
        GeoZonePathEvaluator::ZoneResult result;
        result.zone = zone;
        result.crossings.append(GeoZonePathEvaluator::Crossing());
        result.stretches.append(GeoZonePathEvaluator::Stretch());
        evaluation.zones.append(result);
    }
    GeoZonePathEvaluator::ZoneResult overflownResult;
    overflownResult.zone = overflown;
    overflownResult.stretches.append(GeoZonePathEvaluator::Stretch());
    evaluation.zones.append(overflownResult);

    const QList<const GeoZone*> zones = { uSpace, smallConditional, farProhibited, overflown, nearConditional, enteredRestricted, fartherProhibited, nearProhibited, enteredRound, duplicate };
    const QList<Candidate> candidateList = candidates(zones, evaluation, flight);
    QCOMPARE(candidateList.count(), zones.count());
    const auto candidateOf = [&candidateList](const GeoZone *zone) {
        return *std::find_if(candidateList.cbegin(), candidateList.cend(), [zone](const Candidate &candidate) { return candidate.zone == zone; });
    };
    QCOMPARE(candidateOf(enteredRound).proximity, Proximity::Entered);
    QCOMPARE(candidateOf(overflown).proximity, Proximity::Overflown);
    QVERIFY(candidateOf(overflown).excluded);
    QCOMPARE(candidateOf(nearProhibited).proximity, Proximity::Near);
    QVERIFY(qAbs(candidateOf(nearProhibited).distance - 400) < 5);
    QCOMPARE(candidateOf(farProhibited).proximity, Proximity::Far);
    QVERIFY(candidateOf(fartherProhibited).distance > candidateOf(farProhibited).distance);
    // Without a flight all zones are far
    QCOMPARE(candidates({ nearProhibited }, GeoZonePathEvaluator::Result(), {}).first().proximity, Proximity::Far);

    // Prohibited zones first, entered, near, then by distance, then other restricted zones, then the others
    const auto names = [](const auto &list, auto zoneOf) {
        QStringList result;
        for (const auto &entry : list) {
            result.append(zoneOf(entry)->name());
        }
        return result;
    };
    const auto candidateZone = [](const Candidate &candidate) { return candidate.zone; };
    const QStringList priority = { QStringLiteral("Prohibited entered"), QStringLiteral("Overflown"), QStringLiteral("Prohibited near"),
                                   QStringLiteral("Prohibited far"), QStringLiteral("Prohibited far copy"), QStringLiteral("Prohibited farther"),
                                   QStringLiteral("Authorisation entered"), QStringLiteral("Conditional near"), QStringLiteral("Conditional small"),
                                   QStringLiteral("U-space") };
    QCOMPARE(names(sortedByPriority(candidateList), candidateZone), priority);

    // PX4 stores 64 points: the zones fit in priority order, a smaller one after a zone without room still fits
    const auto choiceZone = [](const Choice &choice) { return choice.candidate.zone; };
    Selection selection = select(candidateList, Capacity::px4());
    QCOMPARE(names(selection.choices, choiceZone), QStringList({ QStringLiteral("Prohibited entered"), QStringLiteral("Prohibited near"), QStringLiteral("Prohibited far"),
                                                                 QStringLiteral("Prohibited farther"), QStringLiteral("Authorisation entered"), QStringLiteral("Conditional small"),
                                                                 QStringLiteral("Overflown"), QStringLiteral("Prohibited far copy"), QStringLiteral("Conditional near"),
                                                                 QStringLiteral("U-space") }));
    QCOMPARE(selection.usage.items, 64);
    QCOMPARE(selection.selectedZones().count(), 6);
    const auto choiceOf = [&selection](const GeoZone *zone) {
        return *std::find_if(selection.choices.cbegin(), selection.choices.cend(), [zone](const Choice &choice) { return choice.candidate.zone == zone; });
    };
    QCOMPARE(choiceOf(overflown).status, Status::Excluded);
    QCOMPARE(choiceOf(duplicate).status, Status::Duplicate);
    QCOMPARE(choiceOf(duplicate).duplicateOf, farProhibited);
    QCOMPARE(choiceOf(nearConditional).status, Status::NoRoom);
    QCOMPARE(choiceOf(nearConditional).usage.items, 12);
    for (const Choice &choice : selection.choices) {
        QCOMPARE(statusText(choice).isEmpty(), choice.status == Status::Selected);
        QCOMPARE(choice.fences.isEmpty(), choice.status != Status::Selected);
    }
    QVERIFY(statusText(choiceOf(duplicate)).contains(QStringLiteral("Prohibited far")));

    // Kept fences take room, and fences which don't fit even alone are too large
    Usage kept;
    kept.addPolygon(30);
    selection = select(candidateList, Capacity::px4(), kept);
    QCOMPARE(choiceOf(enteredRound).status, Status::TooLarge);
    QCOMPARE(choiceOf(nearProhibited).status, Status::Selected);
    QCOMPARE(selection.reserved, kept);

    // Simplifying makes room: the round zone first in priority gets the least simplified fence which fits
    Candidate leftOut = candidateOf(uSpace);
    leftOut.excluded = true;
    QList<Candidate> userChoice = candidateList;
    userChoice.erase(std::remove_if(userChoice.begin(), userChoice.end(), [uSpace](const Candidate &candidate) { return candidate.zone == uSpace; }), userChoice.end());
    userChoice.append(leftOut);
    selection = select(userChoice, Capacity::px4(), kept, { 0, 10, 25, 50 });
    QCOMPARE(choiceOf(uSpace).status, Status::Excluded);
    QCOMPARE(statusText(choiceOf(uSpace)), QStringLiteral("Left out"));
    QCOMPARE(choiceOf(enteredRound).status, Status::Selected);
    QVERIFY(choiceOf(enteredRound).simplifyTolerance > 0);
    QVERIFY(choiceOf(enteredRound).usage.items < 40);
    QCOMPARE(choiceOf(enteredRound).fences, zoneFences(enteredRound, choiceOf(enteredRound).simplifyTolerance));
    QVERIFY(fencesCover(choiceOf(enteredRound).fences, enteredRound));
    QVERIFY(Capacity::px4().fits(selection.usage));
    Usage total = kept;
    for (const Choice &choice : selection.choices) {
        if (choice.status == Status::Selected) {
            total += choice.usage;
        }
    }
    QCOMPARE(total, selection.usage);
    // With room to spare nothing is simplified
    selection = select(userChoice, Capacity::arduPilot(true, 8), Usage(), { 0, 10, 25, 50 });
    QCOMPARE(choiceOf(enteredRound).simplifyTolerance, 0.0);
    QCOMPARE(choiceOf(nearConditional).status, Status::Selected);

    // Vehicles without exclusion fences store none
    selection = select(candidateList, Capacity::unsupported());
    QCOMPARE(choiceOf(enteredRound).status, Status::Unsupported);
    QCOMPARE(selection.usage, Usage());
}

void GeoZoneGeometryTest::_testContainsAndPaths()
{
    GeoZone::Volume polygonVolume;
    // 1 km square with a 200 m hole in the middle
    const QGeoCoordinate center(47, 8);
    polygonVolume.rings.append(GeoZone::toRing({ center.atDistanceAndAzimuth(707.1, 45), center.atDistanceAndAzimuth(707.1, 135),
                                                 center.atDistanceAndAzimuth(707.1, 225), center.atDistanceAndAzimuth(707.1, 315) }));
    polygonVolume.rings.append(GeoZone::toRing({ center.atDistanceAndAzimuth(141.4, 45), center.atDistanceAndAzimuth(141.4, 135),
                                                 center.atDistanceAndAzimuth(141.4, 225), center.atDistanceAndAzimuth(141.4, 315) }));
    GeoZone::Data polygonData;
    polygonData.volumes.append(polygonVolume);
    const GeoZone polygon(polygonData);

    QVERIFY(polygon.contains(center.atDistanceAndAzimuth(300, 0)));
    QVERIFY(!polygon.contains(center));
    QVERIFY(!polygon.contains(center.atDistanceAndAzimuth(600, 0)));
    QCOMPARE(polygon.distanceTo(center.atDistanceAndAzimuth(300, 0)), 0.0);
    QVERIFY(qAbs(polygon.distanceTo(center.atDistanceAndAzimuth(600, 0)) - 100) < 1);
    QVERIFY(qAbs(polygon.distanceTo(center) - 100) < 1);

    // Distances near the edge of a zone spanning two degrees of latitude are measured at the position's latitude
    GeoZone::Data tallData;
    tallData.volumes.append(GeoZone::Volume{ false, { GeoZone::toRing({ QGeoCoordinate(59, 10), QGeoCoordinate(61, 10), QGeoCoordinate(61, 11), QGeoCoordinate(59, 11) }) } });
    const GeoZone tall(tallData);
    const QGeoCoordinate eastOfEdge = QGeoCoordinate(60.9, 11).atDistanceAndAzimuth(500, 90);
    QVERIFY2(qAbs(tall.distanceTo(eastOfEdge) - 500) < 2, qPrintable(QString::number(tall.distanceTo(eastOfEdge))));
    QVERIFY(tall.contains(QGeoCoordinate(60.9, 11).atDistanceAndAzimuth(10, 270)));

    // Paths through the zone, inside the hole only, and passing by
    QVERIFY(polygon.intersectsPath({ center.atDistanceAndAzimuth(2000, 270), center.atDistanceAndAzimuth(2000, 90) }));
    // Invalid positions are skipped
    QVERIFY(polygon.intersectsPath({ QGeoCoordinate(), center.atDistanceAndAzimuth(2000, 270), center.atDistanceAndAzimuth(2000, 90) }));
    QVERIFY(polygon.intersectsPath({ center.atDistanceAndAzimuth(300, 0) }));
    QVERIFY(!polygon.intersectsPath({ center.atDistanceAndAzimuth(50, 270), center.atDistanceAndAzimuth(50, 90) }));
    QVERIFY(!polygon.intersectsPath({ center.atDistanceAndAzimuth(1000, 0).atDistanceAndAzimuth(2000, 270), center.atDistanceAndAzimuth(1000, 0).atDistanceAndAzimuth(2000, 90) }));

    // Entries at the outer boundary, at the hole's boundary when starting in the hole, and at the start when inside
    const auto entryDistance = [](const GeoZone &zone, const QList<QGeoCoordinate> &path) {
        QList<GeoZone::Position> positions;
        for (const QGeoCoordinate &coordinate : path) {
            positions.append(GeoZone::Position{ coordinate });
        }
        const std::optional<GeoZone::PathEntry> entry = zone.pathEntry(positions);
        return entry ? path[entry->segment].distanceTo(entry->position.coordinate) : qQNaN();
    };
    QVERIFY(qAbs(entryDistance(polygon, { center.atDistanceAndAzimuth(2000, 270), center.atDistanceAndAzimuth(2000, 90) }) - 1500) < 1);
    QVERIFY(qAbs(entryDistance(polygon, { center.atDistanceAndAzimuth(3000, 270), center.atDistanceAndAzimuth(2000, 270), center.atDistanceAndAzimuth(2000, 90) }) - 1500) < 1);
    QVERIFY(qAbs(entryDistance(polygon, { center, center.atDistanceAndAzimuth(2000, 90) }) - 100) < 1);
    QCOMPARE(entryDistance(polygon, { center.atDistanceAndAzimuth(300, 0), center.atDistanceAndAzimuth(2000, 90) }), 0.0);

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
    QVERIFY(qAbs(entryDistance(circle, { center.atDistanceAndAzimuth(1000, 270), center.atDistanceAndAzimuth(1000, 90) }) - 500) < 1);

#ifndef GEOZONE_CORE_TEST
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
    QList<QList<GeoZone::Position>> paths = GeoZoneMissionPath::fromSegments({ { a, b }, { b, c }, { QGeoCoordinate(), d }, { d, a } }).positionLists();
    QCOMPARE(paths.count(), 2);
    QCOMPARE(coordinates(paths[0]), QList<QGeoCoordinate>({ a, b, c }));
    QCOMPARE(paths[1].count(), 6);
    QCOMPARE(paths[1].first().coordinate, d);
    QCOMPARE(paths[1].last().coordinate, a);
    QVERIFY(qAbs(paths[1][1].coordinate.distanceTo(d) - 1000) < 1);

    // Positions at the terrain samples with interpolated altitudes, and a climb at a shared waypoint is kept.
    // The climb starts at the launch position, whose coordinate has an altitude.
    const QGeoCoordinate launch(a.latitude(), a.longitude(), 300);
    GeoZoneMissionPath::Segment climb{ launch, a, 400, 500 };
    GeoZoneMissionPath::Segment leg{ a, b, 500, 600, { 300, 350, 320 }, 300 };
    paths = GeoZoneMissionPath::fromSegments({ climb, leg }).positionLists();
    QCOMPARE(paths.count(), 1);
    QCOMPARE(paths[0].count(), 4);
    QCOMPARE(paths[0][0].altitudeAMSL, 400.0);
    // The climb has no terrain samples, it is at the terrain of the leg start
    QCOMPARE(paths[0][0].terrainAMSL, 300.0);
    QCOMPARE(paths[0][1].altitudeAMSL, 500.0);
    QCOMPARE(paths[0][1].terrainAMSL, 300.0);
    QCOMPARE(paths[0][2].altitudeAMSL, 560.0);
    QCOMPARE(paths[0][2].terrainAMSL, 350.0);
    QVERIFY(qAbs(paths[0][2].coordinate.distanceTo(a) - 300) < 1);
    QCOMPARE(paths[0][3].coordinate, b);
    QCOMPARE(paths[0][3].terrainAMSL, 320.0);
    // The profile samples between terrain samples
    QVERIFY(qAbs(leg.terrainAt(150) - 325) < 1e-9);
    const GeoZone::Position halfway = leg.positionAt(a.distanceTo(b) / 2);
    QVERIFY(qAbs(halfway.altitudeAMSL - 550) < 1e-9);
    QVERIFY(qAbs(halfway.coordinate.distanceTo(a) - (a.distanceTo(b) / 2)) < 0.01);

    // Paths remember the mission item of their first segment
    GeoZoneMissionPath::Segment first{ a, b };
    first.itemIndex = 1;
    GeoZoneMissionPath::Segment second{ b, c };
    second.itemIndex = 2;
    GeoZoneMissionPath::Segment third{ d, a };
    third.itemIndex = 4;
    const QList<GeoZoneMissionPath::Path> itemPaths = GeoZoneMissionPath::pathsFromSegments({ first, second, third });
    QCOMPARE(itemPaths.count(), 2);
    QCOMPARE(itemPaths[0].itemIndex, 1);
    QCOMPARE(itemPaths[1].itemIndex, 4);

    // A PX4 takeoff at the launch position: a vertical segment whose terrain samples have no spacing
    paths = GeoZoneMissionPath::fromSegments({ { launch, a, 500, 500, { 310, 310 }, 0 }, leg }).positionLists();
    QCOMPARE(paths.count(), 1);
    QCOMPARE(paths[0].first().terrainAMSL, 310.0);
    QVERIFY(std::none_of(paths[0].cbegin(), paths[0].cend(), [](const GeoZone::Position &position) { return qIsNaN(position.terrainAMSL); }));
#endif
}

void GeoZoneGeometryTest::_testAltitudeLimits()
{
    const QGeoCoordinate center(47, 8);
    const auto position = [](const QGeoCoordinate &coordinate, double altitudeAMSL, double terrainAMSL) {
        return GeoZone::Position{ coordinate, altitudeAMSL, terrainAMSL };
    };

    // Ground to 120 m above ground, and 1000 ft above mean sea level and higher, see _testVerticalOverlap
    GeoZone::Volume low;
    low.isCircle = true;
    low.center = center;
    low.radius = 500;
    low.lowerLimit = 0;
    low.upperLimit = 120;
    low.setReferences(QStringLiteral("AGL"), QStringLiteral("AGL"));
    GeoZone::Volume high = low;
    high.feet = true;
    high.lowerLimit = 1000;
    high.upperLimit = 99999;
    high.setReferences(QStringLiteral("AMSL"), QStringLiteral("AMSL"));

    // Heights above the WGS84 ellipsoid are about 48 m higher than above mean sea level in Zurich
    QVERIFY(qAbs(GeoZoneGeometry::geoidHeight(QGeoCoordinate(47.37, 8.54)) - 47.3) < 2);
    QVERIFY(qAbs(GeoZoneGeometry::geoidHeight(QGeoCoordinate(42.39, -71.15)) + 28.6) < 2);
    QVERIFY(qAbs(GeoZoneGeometry::geoidHeight(QGeoCoordinate(5, 78)) + 104.7) < 2);
    QVERIFY(qIsNaN(GeoZoneGeometry::geoidHeight(QGeoCoordinate())));

    // Published codes are case insensitive and have aliases, the ground is shown as GND either way
    GeoZone::Volume surface = low;
    surface.setReferences(QStringLiteral(" sfc"), QStringLiteral("msl "));
    QCOMPARE(surface.lowerReference, GeoZone::VerticalReference::Ground);
    QCOMPARE(surface.upperReference, GeoZone::VerticalReference::MeanSeaLevel);
    surface.setReferences(QStringLiteral("STD"), QStringLiteral("WGS84"));
    QCOMPARE(surface.lowerReference, GeoZone::VerticalReference::FlightLevel);
    QCOMPARE(surface.upperReference, GeoZone::VerticalReference::Ellipsoid);
    surface.setReferences(QStringLiteral(" sfc"), QStringLiteral("msl "));
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
    // Altitudes are interpolated: descending through the ceiling over the zone enters it there, descending below the
    // ceiling only past the zone doesn't
    const std::optional<GeoZone::PathEntry> entry = zone.pathEntry({ position(west, 600, 400), position(east, 450, 400) });
    QVERIFY(entry.has_value());
    QVERIFY(qAbs(entry->position.altitudeAMSL - 520) < 0.01);
    QCOMPARE(entry->position.terrainAMSL, 400.0);
    QVERIFY(qAbs(west.distanceTo(entry->position.coordinate) - (2000 * 80.0 / 150)) < 1);
    QVERIFY(!zone.intersectsPath(QList<GeoZone::Position>{ position(west, 600, 400), position(center.atDistanceAndAzimuth(3000, 90), 400, 400) }));
    // Climbing in place enters at the floor
    GeoZone::Data highData;
    highData.volumes.append(high);
    const std::optional<GeoZone::PathEntry> climb = GeoZone(highData).pathEntry({ position(center, 200, 150), position(center, 400, 150) });
    QVERIFY(climb.has_value());
    QVERIFY(qAbs(climb->position.altitudeAMSL - 304.8) < 0.01);
}

void GeoZoneGeometryTest::_testVerticalOverlap_data()
{
    QTest::addColumn<double>("lowerLimit");
    QTest::addColumn<double>("upperLimit");
    QTest::addColumn<QString>("lowerReference");
    QTest::addColumn<QString>("upperReference");
    QTest::addColumn<bool>("feet");
    QTest::addColumn<QGeoCoordinate>("coordinate");
    QTest::addColumn<double>("fromAltitude");
    QTest::addColumn<double>("toAltitude");
    QTest::addColumn<double>("terrain");
    QTest::addColumn<double>("margin");
    QTest::addColumn<bool>("overlap");

    const QGeoCoordinate center(47, 8);
    const double nan = qQNaN();
    // Ground to 120 m above ground. Slightly below the ground is still inside a zone from the ground, and unknown
    // terrain or altitudes can't be excluded.
    QTest::newRow("AGL inside") << 0.0 << 120.0 << "AGL" << "AGL" << false << center << 450.0 << 450.0 << 400.0 << 0.0 << true;
    QTest::newRow("AGL above") << 0.0 << 120.0 << "AGL" << "AGL" << false << center << 600.0 << 600.0 << 400.0 << 0.0 << false;
    QTest::newRow("AGL below ground") << 0.0 << 120.0 << "AGL" << "AGL" << false << center << 399.9 << 399.9 << 400.0 << 0.0 << true;
    QTest::newRow("AGL within margin") << 0.0 << 120.0 << "AGL" << "AGL" << false << center << 600.0 << 600.0 << 400.0 << 100.0 << true;
    QTest::newRow("AGL climb through") << 0.0 << 120.0 << "AGL" << "AGL" << false << center << 450.0 << 700.0 << 400.0 << 0.0 << true;
    QTest::newRow("AGL unknown terrain") << 0.0 << 120.0 << "AGL" << "AGL" << false << center << 600.0 << 600.0 << nan << 0.0 << true;
    QTest::newRow("AGL unknown altitude") << 0.0 << 120.0 << "AGL" << "AGL" << false << center << nan << nan << nan << 0.0 << true;
    // 1000 ft above mean sea level and higher, published in feet: 304.8 m
    QTest::newRow("ft AMSL below") << 1000.0 << 99999.0 << "AMSL" << "AMSL" << true << center << 300.0 << 300.0 << 250.0 << 0.0 << false;
    QTest::newRow("ft AMSL above floor") << 1000.0 << 99999.0 << "AMSL" << "AMSL" << true << center << 310.0 << 310.0 << 250.0 << 0.0 << true;
    QTest::newRow("ft AMSL high") << 1000.0 << 99999.0 << "AMSL" << "AMSL" << true << center << 9000.0 << 9000.0 << 250.0 << 0.0 << true;
    // Heights above the WGS84 ellipsoid are about 48 m higher than above mean sea level in Zurich
    const QGeoCoordinate zurich(47.37, 8.54);
    QTest::newRow("WGS84 above floor") << 500.0 << 1000.0 << "WGS84" << "WGS84" << false << zurich << 460.0 << 460.0 << 400.0 << 0.0 << true;
    QTest::newRow("WGS84 below floor") << 500.0 << 1000.0 << "WGS84" << "WGS84" << false << zurich << 440.0 << 440.0 << 400.0 << 0.0 << false;
    // Flight levels depend on the air pressure and unknown codes like FL can't be compared either
    QTest::newRow("STD floor") << 1000.0 << 99999.0 << "STD" << "AMSL" << true << center << 0.0 << 0.0 << 0.0 << 0.0 << true;
    QTest::newRow("FL unknown") << 95.0 << 195.0 << "FL" << "FL" << false << center << 0.0 << 0.0 << 0.0 << 0.0 << true;
    // Aliases of the codes, ground to 120 m above mean sea level
    QTest::newRow("SFC MSL inside") << 0.0 << 120.0 << " sfc" << "msl " << false << center << 110.0 << 110.0 << 400.0 << 0.0 << true;
    QTest::newRow("SFC MSL above") << 0.0 << 120.0 << " sfc" << "msl " << false << center << 130.0 << 130.0 << 0.0 << 0.0 << false;
}

void GeoZoneGeometryTest::_testVerticalOverlap()
{
    QFETCH(double, lowerLimit);
    QFETCH(double, upperLimit);
    QFETCH(QString, lowerReference);
    QFETCH(QString, upperReference);
    QFETCH(bool, feet);
    QFETCH(QGeoCoordinate, coordinate);
    QFETCH(double, fromAltitude);
    QFETCH(double, toAltitude);
    QFETCH(double, terrain);
    QFETCH(double, margin);
    QFETCH(bool, overlap);

    GeoZone::Volume volume;
    volume.isCircle = true;
    volume.center = coordinate;
    volume.radius = 500;
    volume.feet = feet;
    volume.lowerLimit = lowerLimit;
    volume.upperLimit = upperLimit;
    volume.setReferences(lowerReference, upperReference);
    QCOMPARE(GeoZone::verticalOverlap(volume, GeoZone::Position{ coordinate, fromAltitude, terrain }, GeoZone::Position{ coordinate, toAltitude, terrain }, margin),
             overlap);
}

void GeoZoneGeometryTest::_testAreaIntersection()
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
    small.volumes.append(GeoZone::Volume{ false, { GeoZone::toRing(square(center.atDistanceAndAzimuth(200, 0), 10)) } });
    QVERIFY(GeoZone(small).intersectsArea(area(survey, 100, 200)));
    GeoZone::Data large;
    large.volumes.append(GeoZone::Volume{ false, { GeoZone::toRing(square(center, 2000)) } });
    QVERIFY(GeoZone(large).intersectsArea(area(survey, 100, 200)));
    large.volumes[0].rings.append(GeoZone::toRing(square(center, 1000)));
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

void GeoZoneGeometryTest::_testShapeSimplification()
{
    // Dense 5 km circle with a 40 m hole
    const QGeoCoordinate center(47.0, 8.0);
    const QList<QGeoCoordinate> ring = _circleRing(center, 5000, 10000);
    const GeoZoneShape shape(nullptr, { GeoZone::toRing(ring), GeoZone::toRing(_circleRing(center, 20, 8)) });

    const QList<QList<QPointF>> full = shape.mercatorRings(GeoZoneShape::fullDetailZoomLevel);
    QCOMPARE(full.count(), 2);
    QCOMPARE(full.first().count(), ring.count());

    const int zoomLevel = 10;
    const QList<QList<QPointF>> simplifiedRings = shape.mercatorRings(zoomLevel);
    QCOMPARE(simplifiedRings.count(), 1); // The hole is under a pixel at this zoom level
    const QList<QPointF> simplified = simplifiedRings.first();
    QVERIFY(simplified.count() >= 3);
    QVERIFY(simplified.count() < 100);
    QCOMPARE(shape.mercatorRings(16).count(), 2);

    // Shapes under a pixel become a triangle spanning them instead of keeping every vertex
    const QList<QGeoCoordinate> smallRing = _circleRing(center, 50, 200);
    const GeoZoneShape smallShape(nullptr, { GeoZone::toRing(smallRing) });
    QList<QGeoCoordinate> triangle;
    for (const QPointF &point : smallShape.mercatorRings(5).first()) {
        triangle.append(GeoZoneShape::fromMercator(point));
    }
    QCOMPARE(triangle.count(), 3);
    const QList<double> sides = { triangle[0].distanceTo(triangle[1]), triangle[1].distanceTo(triangle[2]), triangle[2].distanceTo(triangle[0]) };
    QVERIFY(*std::max_element(sides.cbegin(), sides.cend()) > 95); // Spans the 100 m diameter
    QVERIFY(*std::min_element(sides.cbegin(), sides.cend()) > 65);
    QCOMPARE(smallShape.mercatorRings(GeoZoneShape::fullDetailZoomLevel).first().count(), smallRing.count());

    // Simplified vertices are original vertices and every original vertex stays within half a pixel of the outline
    const double tolerance = 0.5 / GeoZoneShape::mercatorPixels(zoomLevel);
    for (const QPointF &vertex : simplified) {
        QVERIFY(full.first().contains(vertex));
    }
    for (const QPointF &vertex : full.first()) {
        QVERIFY(_distanceToRing(vertex, simplified) <= tolerance * 1.0001);
    }

    // A 1 km circle is 2000 m / 104.26 m per pixel wide at zoom level 10 and 47 degrees latitude
    const GeoZoneShape circle(nullptr, center, 1000);
    const double widthPixels = circle.mercatorBounds().width() * GeoZoneShape::mercatorPixels(zoomLevel);
    QVERIFY2(qAbs(widthPixels - 19.18) < 0.2, qPrintable(QString::number(widthPixels)));
    QCOMPARE(circle.mercatorBounds().height(), circle.mercatorBounds().width());
}

#ifndef GEOZONE_CORE_TEST
void GeoZoneGeometryTest::_testMapLayerTriangulation()
{
    const auto area = [](const QList<QPointF> &triangles) {
        double total = 0;
        for (qsizetype i = 0; (i + 2) < triangles.count(); i += 3) {
            const QPointF a = triangles[i + 1] - triangles[i];
            const QPointF b = triangles[i + 2] - triangles[i];
            total += std::abs((a.x() * b.y()) - (a.y() * b.x())) / 2;
        }
        return total;
    };

    // A square with a square hole fills the area between them
    const QList<QPointF> outer = { { 0.5, 0.5 }, { 0.5001, 0.5 }, { 0.5001, 0.5001 }, { 0.5, 0.5001 } };
    const QList<QPointF> hole = { { 0.50004, 0.50004 }, { 0.50006, 0.50004 }, { 0.50006, 0.50006 }, { 0.50004, 0.50006 } };
    const GeoZoneMapLayer::Triangulation square = GeoZoneMapLayer::triangulatePolygon({ outer, hole });
    QCOMPARE(square.rings.count(), 2);
    QVERIFY(qAbs(area(square.triangles) - (1e-8 - 4e-10)) < 1e-11);
    QVERIFY(qAbs(square.center.x() - 0.50005) < 1e-9);

    // A shape crossing the antimeridian stays in one piece
    const QList<QPointF> crossing = { { 0.9999, 0.5 }, { 0.0001, 0.5 }, { 0.0001, 0.5001 }, { 0.9999, 0.5001 } };
    const GeoZoneMapLayer::Triangulation wrapped = GeoZoneMapLayer::triangulatePolygon({ crossing });
    QVERIFY(qAbs(area(wrapped.triangles) - 2e-8) < 1e-11);
    QVERIFY(qAbs(wrapped.center.x() - 1.0) < 1e-9);

    // Circles follow the radius in meters
    const QGeoCoordinate center(47, 8);
    const GeoZoneMapLayer::Triangulation circle = GeoZoneMapLayer::triangulateCircle(center, 1000);
    const QPointF mercatorCenter = GeoZoneShape::toMercator(center);
    const double mercatorRadius = (GeoZoneShape::toMercator(center.atDistanceAndAzimuth(1000, 90)) - mercatorCenter).x();
    QVERIFY(qAbs((area(circle.triangles) / (M_PI * mercatorRadius * mercatorRadius)) - 1) < 0.01);
    QVERIFY(GeoZoneMapLayer::triangulateCircle(center, 0).triangles.isEmpty());
    QVERIFY(GeoZoneMapLayer::triangulatePolygon({ { { 0.5, 0.5 }, { 0.6, 0.6 } } }).triangles.isEmpty());

    // Simplified rings can cross themselves, filled even-odd
    const QList<QPointF> bowtie = { { 0.5, 0.5 }, { 0.5001, 0.5001 }, { 0.5001, 0.5 }, { 0.5, 0.5001 } };
    const GeoZoneMapLayer::Triangulation crossed = GeoZoneMapLayer::triangulatePolygon({ bowtie });
    QVERIFY(qAbs(area(crossed.triangles) - 0.5e-8) < 1e-11);
    for (const QPointF &vertex : crossed.triangles) {
        QVERIFY((vertex.x() >= 0.5 - 1e-9) && (vertex.x() <= 0.5001 + 1e-9) && (vertex.y() >= 0.5 - 1e-9) && (vertex.y() <= 0.5001 + 1e-9));
    }

    // The map transform is found next to the antimeridian, where coordinates east of it can't be projected
    const QPointF itemCenter(800, 450);
    for (const QGeoCoordinate &mapCenter : { QGeoCoordinate(47, 179.999), QGeoCoordinate(47, 8), QGeoCoordinate(-85, 8) }) {
        const double pixels = GeoZoneShape::mercatorPixels(13);
        const auto toItem = [&mapCenter, &itemCenter, pixels](const QGeoCoordinate &coordinate) {
            if (!coordinate.isValid()) {
                return QPointF(qQNaN(), qQNaN());
            }
            QPointF offset = GeoZoneShape::toMercator(coordinate) - GeoZoneShape::toMercator(mapCenter);
            offset.rx() -= std::round(offset.x());
            return itemCenter + (offset * pixels);
        };
        QTransform transform;
        QVERIFY(GeoZoneMapLayer::mercatorTransform(mapCenter, 13, toItem, transform));
        const QPointF mapped = transform.map(GeoZoneShape::toMercator(mapCenter.atDistanceAndAzimuth(500, 225)));
        const QPointF expected = toItem(mapCenter.atDistanceAndAzimuth(500, 225));
        QVERIFY2((mapped - expected).manhattanLength() < 0.01, qPrintable(QStringLiteral("%1 %2").arg(mapped.x()).arg(expected.x())));
    }
    QTransform transform;
    QVERIFY(!GeoZoneMapLayer::mercatorTransform(QGeoCoordinate(), 13, [](const QGeoCoordinate &) { return QPointF(); }, transform));
}
#endif

void GeoZoneGeometryTest::_testAntimeridian()
{
    // A zone from 179.5 E to 179.5 W published with its longitudes jumping across the antimeridian
    ZoneOwner owner;
    GeoZone::Data crossingData;
    crossingData.identifier = QStringLiteral("CROSSING");
    crossingData.volumes.append(GeoZone::Volume{ false, { GeoZone::toRing({ QGeoCoordinate(-17, 179.5), QGeoCoordinate(-16, 179.5),
                                                                            QGeoCoordinate(-16, -179.5), QGeoCoordinate(-17, -179.5) }) } });
    GeoZone *const crossing = owner.add(crossingData);
    QVERIFY(crossing->boundingBox().width() < 1.01);
    QVERIFY(crossing->contains(QGeoCoordinate(-16.5, 179.9)));
    QVERIFY(crossing->contains(QGeoCoordinate(-16.5, -179.9)));
    QVERIFY(crossing->contains(QGeoCoordinate(-16.5, 180)));
    QVERIFY(!crossing->contains(QGeoCoordinate(-16.5, 0)));
    QVERIFY(!crossing->contains(QGeoCoordinate(-16.5, 179)));
    const double expectedDistance = QGeoCoordinate(-16.5, -179).distanceTo(QGeoCoordinate(-16.5, -179.5));
    QVERIFY(qAbs(crossing->distanceTo(QGeoCoordinate(-16.5, -179)) - expectedDistance) < (expectedDistance * 0.005));
    const QList<QList<QGeoCoordinate>> exclusionPolygons = crossing->exclusionPolygons();
    QCOMPARE(exclusionPolygons.count(), 1);
    for (const QGeoCoordinate &coordinate : exclusionPolygons.first()) {
        QVERIFY(coordinate.isValid());
    }

    // A path crossing the antimeridian enters at the west edge of the zone
    const std::optional<GeoZone::PathEntry> entry = crossing->pathEntry({ { QGeoCoordinate(-16.5, 179) }, { QGeoCoordinate(-16.5, -179) } });
    QVERIFY(entry);
    QVERIFY(qAbs(entry->position.coordinate.longitude() - 179.5) < 1e-6);
    QVERIFY(!crossing->pathEntry({ { QGeoCoordinate(-16.5, 179) }, { QGeoCoordinate(-16.5, 0) } }));

    // The index finds zones on both sides of the antimeridian without matching the rest of the world
    GeoZone::Data circleData;
    circleData.identifier = QStringLiteral("CIRCLE");
    GeoZone::Volume circleVolume;
    circleVolume.isCircle = true;
    circleVolume.center = QGeoCoordinate(-16.5, 179.99);
    circleVolume.radius = 5000;
    circleData.volumes.append(circleVolume);
    GeoZone::Data farData;
    farData.identifier = QStringLiteral("FAR");
    farData.volumes.append(GeoZone::Volume{ false, { GeoZone::toRing({ QGeoCoordinate(-17, -0.5), QGeoCoordinate(-16, -0.5), QGeoCoordinate(-16, 0.5) }) } });
    GeoZoneIndex index;
    index.build({ crossing, owner.add(circleData), owner.add(farData) });
    QCOMPARE(index.query(-16.5, -179.99, -16.5, -179.99), QList<int>({ 0, 1 }));
    QCOMPARE(index.query(-16.5, 179.99, -16.5, 179.99), QList<int>({ 0, 1 }));
    QCOMPARE(index.query(-16.5, 179.2, -16.5, 179.2), QList<int>());
    QCOMPARE(index.query(QGeoRectangle(QGeoCoordinate(-16, 179), QGeoCoordinate(-17, -179))), QList<int>({ 0, 1 }));
    QCOMPARE(index.query(-16.5, -0.1, -16.4, 0.1), QList<int>({ 2 }));
    const auto [west, east] = GeoZoneBox::longitudeSpan(179, -179);
    QCOMPARE(index.query(-16.5, west, -16.5, east), QList<int>({ 0, 1 }));

    // The map shape stays narrow and is found by views on either side of the antimeridian
    const GeoZoneShape shape(crossing, crossing->data().volumes[0].rings);
    QVERIFY(shape.mercatorBounds().width() < 0.01);
    const double y = GeoZoneShape::toMercator(QGeoCoordinate(-16.5, 0)).y();
    QVERIFY(shape.mercatorBounds().intersects({ 0.0, y - 0.001, 0.0005, y + 0.001 }));
    QVERIFY(shape.mercatorBounds().intersects({ 0.9995, y - 0.001, 1.0, y + 0.001 }));
    QVERIFY(!shape.mercatorBounds().intersects({ 0.5, y - 0.001, 0.6, y + 0.001 }));
}

void GeoZoneGeometryTest::_testBoxAcrossAntimeridian()
{
    // A west longitude larger than the east one crosses the antimeridian, the east one continues past 180 degrees
    const GeoZoneBox crossing(-17, 179.5, -16, -179.5);
    QVERIFY(crossing.isValid());
    QVERIFY(crossing.crossesAntimeridian());
    QCOMPARE(crossing.west(), 179.5);
    QCOMPARE(crossing.east(), 180.5);
    QCOMPARE(crossing.width(), 1.0);
    QCOMPARE(crossing.center().latitude(), -16.5);
    QCOMPARE(qAbs(crossing.center().longitude()), 180.0);
    QVERIFY(crossing.contains(QGeoCoordinate(-16.5, 179.9)));
    QVERIFY(crossing.contains(QGeoCoordinate(-16.5, -179.9)));
    QVERIFY(crossing.contains(QGeoCoordinate(-16.5, 180)));
    QVERIFY(crossing.contains(QGeoCoordinate(-16.5, -180)));
    QVERIFY(!crossing.contains(QGeoCoordinate(-16.5, 179)));
    QVERIFY(!crossing.contains(QGeoCoordinate(-16.5, 0)));
    QVERIFY(!crossing.contains(QGeoCoordinate(-15.5, 179.9)));
    QVERIFY(!GeoZoneBox().contains(QGeoCoordinate(0, 0)));

    // Boxes on either side, single points and meridian segments intersect it, the rest of the world doesn't
    QVERIFY(crossing.intersects(GeoZoneBox(-16.6, 179, -16.4, 179.6)));
    QVERIFY(crossing.intersects(GeoZoneBox(-16.6, -179.6, -16.4, -179)));
    QVERIFY(crossing.intersects(GeoZoneBox(-16.5, -179.9, -16.5, -179.9)));
    QVERIFY(GeoZoneBox(-16.6, 179.9, -16.4, 179.9).intersects(crossing));
    QVERIFY(!crossing.intersects(GeoZoneBox(-16.6, -179, -16.4, 179)));
    QVERIFY(!crossing.intersects(GeoZoneBox(-15.9, 179, -15, -179)));
    QVERIFY(!crossing.intersects(GeoZoneBox()));
    QVERIFY(GeoZoneBox(-17, -100, -16, 300).intersects(GeoZoneBox(-16.5, 0, -16.5, 0)));

    // Converts both ways with QGeoRectangle, which crosses the antimeridian the same way
    const QGeoRectangle rectangle(QGeoCoordinate(-16, 179.5), QGeoCoordinate(-17, -179.5));
    QVERIFY(GeoZoneBox(rectangle) == crossing);
    QVERIFY(crossing.toRectangle() == rectangle);
    QVERIFY(!GeoZoneBox(QGeoRectangle()).isValid());
    QVERIFY(!crossing.toRectangle().contains(QGeoCoordinate(-16.5, 0)));

    // Paths and polygons follow their edges, which go the shorter way around
    const QList<QGeoCoordinate> polygon = { QGeoCoordinate(-17, 179.5), QGeoCoordinate(-16, 179.5), QGeoCoordinate(-16, -179.5), QGeoCoordinate(-17, -179.5) };
    QVERIFY(GeoZoneBox::fromPath(polygon) == crossing);
    const GeoZoneBox longEdges = GeoZoneBox::fromPath({ QGeoCoordinate(0, 0), QGeoCoordinate(0, 100), QGeoCoordinate(1, -100) });
    QCOMPARE(longEdges.west(), 0.0);
    QCOMPARE(longEdges.east(), 260.0);
    QVERIFY(longEdges.contains(QGeoCoordinate(0.5, 180)));
    QVERIFY(!longEdges.contains(QGeoCoordinate(0.5, -50)));
    const GeoZoneBox segment = GeoZoneBox::fromPath({ QGeoCoordinate(-16.5, -179.9), QGeoCoordinate(-16.5, 179.9) });
    QVERIFY(segment.crossesAntimeridian());
    QVERIFY(qAbs(segment.width() - 0.2) < 1e-9);
    QCOMPARE(segment.height(), 0.0);
    QVERIFY(segment.intersects(crossing));
    QVERIFY(!segment.intersects(GeoZoneBox(-17, -1, -16, 1)));
    QVERIFY(!GeoZoneBox::fromPath({ QGeoCoordinate() }).isValid());
    const auto [west, east] = GeoZoneBox::longitudeSpan(179.9, -179.9);
    QCOMPARE(west, 179.9);
    QCOMPARE(east, -179.9);
    QVERIFY(qAbs(segment.west() - 179.9) < 1e-9);
    QVERIFY(qAbs(segment.east() - 180.1) < 1e-9);

    // Uniting boxes on both sides of the antimeridian goes the shorter way too
    const GeoZoneBox united = GeoZoneBox(-17, 179, -16, 179.5).united(GeoZoneBox(-16.5, -179.5, -15, -179));
    QVERIFY(united.crossesAntimeridian());
    QVERIFY(qAbs(united.width() - 2) < 1e-9);
    QCOMPARE(united.south(), -17.0);
    QCOMPARE(united.north(), -15.0);
    QVERIFY(GeoZoneBox().united(crossing) == crossing);

    // Distances are measured the shorter way around, across the antimeridian
    const double degree = QGeoCoordinate(-16.5, 179).distanceTo(QGeoCoordinate(-16.5, 180));
    QCOMPARE(crossing.distanceTo(QGeoCoordinate(-16.5, 180)), 0.0);
    QVERIFY(qAbs(crossing.distanceTo(QGeoCoordinate(-16.5, -178.5)) - degree) < (degree * 0.01));
    QVERIFY(qAbs(crossing.distanceTo(QGeoCoordinate(-16.5, 178.5)) - degree) < (degree * 0.01));
    QVERIFY(qAbs(GeoZoneBox(-17, 179, -16, 179.5).distanceTo(QGeoCoordinate(-16.5, -179.5)) - degree) < (degree * 0.01));
    QVERIFY(qAbs(GeoZoneBox(-17, -179.5, -16, -179).distanceTo(QGeoCoordinate(-16.5, 179.5)) - degree) < (degree * 0.01));
    QVERIFY(qIsInf(GeoZoneBox().distanceTo(QGeoCoordinate(0, 0))));

    // A zone spanning more than 180 degrees of longitude is near positions past the antimeridian, more than 180
    // degrees from its west end, so that distances limited to its surroundings find it
    ZoneOwner owner;
    GeoZone::Data wideData;
    wideData.volumes.append(GeoZone::Volume{ false, { GeoZone::toRing({ QGeoCoordinate(-1, 0), QGeoCoordinate(-1, 50), QGeoCoordinate(-1, 100),
                                                                        QGeoCoordinate(-1, 150), QGeoCoordinate(-1, -160), QGeoCoordinate(1, -160),
                                                                        QGeoCoordinate(1, 150), QGeoCoordinate(1, 100), QGeoCoordinate(1, 50),
                                                                        QGeoCoordinate(1, 0) }) } });
    const GeoZone *const wide = owner.add(wideData);
    QVERIFY(wide->box().crossesAntimeridian());
    QVERIFY(qAbs(wide->box().width() - 200) < 1e-9);
    QVERIFY(wide->contains(GeoZone::Position{ QGeoCoordinate(0, -170) }));
    QVERIFY(!wide->contains(GeoZone::Position{ QGeoCoordinate(0, -150) }));
    QCOMPARE(wide->distanceTo(GeoZone::Position{ QGeoCoordinate(0, -170) }, 0, 100), 0.0);
    const QGeoCoordinate nearEastEnd(0, -159.99);
    const double eastDistance = nearEastEnd.distanceTo(QGeoCoordinate(0, -160));
    QVERIFY(qAbs(wide->distanceTo(GeoZone::Position{ nearEastEnd }, 0, 5000) - eastDistance) < (eastDistance * 0.01));
}

void GeoZoneGeometryTest::_testProjectionContinuity()
{
    // Paths continue from point to point across the antimeridian, even past 180 degrees from the origin
    const GeoZoneGeometry::LocalProjection projection(QGeoCoordinate(0, 0));
    const double metersPerDegree = projection.toLocal(QGeoCoordinate(0, 1)).x();
    const QList<QPointF> points = projection.toLocal(QList<QGeoCoordinate>{ QGeoCoordinate(0, 90), QGeoCoordinate(0, 170),
                                                                            QGeoCoordinate(0, -170), QGeoCoordinate(0, -100) });
    QCOMPARE(points.count(), 4);
    QVERIFY(qAbs(points[0].x() - (90 * metersPerDegree)) < 1e-3);
    QVERIFY(qAbs(points[2].x() - (190 * metersPerDegree)) < 1e-3);
    QVERIFY(qAbs(points[3].x() - (260 * metersPerDegree)) < 1e-3);
    QCOMPARE(projection.lonLatToLocal(QList<QPointF>{ QPointF(90, 0), QPointF(170, 0), QPointF(-170, 0), QPointF(-100, 0) }), points);
    QVERIFY(qAbs(projection.toGeo(points[3]).longitude() - (-100)) < 1e-9);

    // Single points stay within 180 degrees of the origin, or of the previous point
    QVERIFY(qAbs(projection.toLocal(QGeoCoordinate(0, -100)).x() - (-100 * metersPerDegree)) < 1e-3);
    QCOMPARE(projection.toLocal(QGeoCoordinate(0, -100), points[2]), points[3]);
    QCOMPARE(projection.toLocal(QGeoCoordinate(0, 10), points[3]), projection.toLocal(QGeoCoordinate(0, 10)) + QPointF(360 * metersPerDegree, 0));

    // A ring across the antimeridian projected around its first vertex, as simplification does, keeps its shape
    // although it extends more than 180 degrees east of the vertex
    const QList<QGeoCoordinate> ring = { QGeoCoordinate(-1, 0), QGeoCoordinate(-1, 90), QGeoCoordinate(-1, 170), QGeoCoordinate(-1, -130),
                                         QGeoCoordinate(1, -130), QGeoCoordinate(1, 170), QGeoCoordinate(1, 90), QGeoCoordinate(1, 0) };
    const GeoZoneGeometry::LocalProjection ringProjection(ring.first());
    const QList<QPointF> localRing = ringProjection.toLocal(ring);
    const double ringMetersPerDegree = ringProjection.toLocal(QGeoCoordinate(-1, 1)).x();
    QVERIFY(qAbs(GeoZoneGeometry::signedArea(localRing)) > (2 * 229 * ringMetersPerDegree * ringMetersPerDegree));
    QVERIFY(GeoZoneGeometry::ringContains(localRing, ringProjection.toLocal(QGeoCoordinate(0, 120))));
    QVERIFY(GeoZoneGeometry::ringContains(localRing, ringProjection.toLocal(QGeoCoordinate(0, -150), localRing[3])));
    QVERIFY(!GeoZoneGeometry::ringContains(localRing, ringProjection.toLocal(QGeoCoordinate(0, -100), localRing[3])));
}

void GeoZoneGeometryTest::_testConcurrentQueries()
{
    // Mission checks query zones on worker threads while the GUI thread uses the same zones and shapes
    ZoneOwner owner;
    GeoZone::Data data;
    data.volumes.append(GeoZone::Volume{ false, { GeoZone::toRing(_circleRing(QGeoCoordinate(47, 8), 2000, 400)) } });
    GeoZone *const zone = owner.add(data);
    const GeoZoneShape shape(zone, zone->data().volumes[0].rings);
    const QList<double> tolerances = { 5, 10, 20, 40, 80 };
    const QList<int> zoomLevels = { 8, 10, 12, 14, 16 };

    QList<QFuture<bool>> futures;
    for (int thread = 0; thread < 8; thread++) {
        futures.append(QtConcurrent::run([zone, &shape, &tolerances, &zoomLevels, thread]() {
            bool ok = true;
            for (int i = 0; i < 200; i++) {
                const double tolerance = tolerances[(thread + i) % tolerances.count()];
                ok = ok && !zone->exclusionPolygons(tolerance).isEmpty();
                ok = ok && !shape.mercatorRings(zoomLevels[(thread + i) % zoomLevels.count()]).isEmpty();
                ok = ok && zone->contains(QGeoCoordinate(47, 8)) && zone->active();
            }
            return ok;
        }));
    }
    for (int i = 0; i < 200; i++) {
        (void) zone->updateActive(QDateTime::currentDateTimeUtc());
    }
    for (QFuture<bool> &future : futures) {
        QVERIFY(future.result());
    }
    // Each tolerance is simplified once and returned the same way afterwards
    QCOMPARE(zone->exclusionPolygons(10), zone->exclusionPolygons(10));
}
