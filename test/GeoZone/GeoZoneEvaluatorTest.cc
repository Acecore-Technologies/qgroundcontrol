/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneEvaluatorTest.h"
#include "GeoZoneActivationTracker.h"
#include "GeoZoneAlertThresholds.h"
#include "GeoZoneAlertTracker.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneBriefing.h"
#include "GeoZoneInfo.h"
#include "GeoZoneTestHelpers.h"
#include "GeoZoneVertical.h"

using namespace GeoZoneTestHelpers;

void GeoZoneEvaluatorTest::_testPathHits()
{
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate square = center.atDistanceAndAzimuth(3000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("B"), QStringLiteral("CONDITIONAL"), { square.atDistanceAndAzimuth(282.8, 45), square.atDistanceAndAzimuth(282.8, 135),
                                                                              square.atDistanceAndAzimuth(282.8, 225), square.atDistanceAndAzimuth(282.8, 315) }),
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1000, 90), 200),
        // Unpublished restriction types can't be assumed harmless
        _circleFeature(QStringLiteral("X"), QStringLiteral("MYSTERY"), center.atDistanceAndAzimuth(2000, 90), 100),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(500, 90), 100),
    }), errorString), qPrintable(errorString));

    // Each zone once, in the order the path enters them, with the distance to the entry
    const QList<GeoZone::Position> east = { GeoZone::Position{ center }, GeoZone::Position{ center.atDistanceAndAzimuth(2000, 90) },
                                            GeoZone::Position{ center.atDistanceAndAzimuth(4000, 90) }, GeoZone::Position{ center } };
    const QList<GeoZonePathEvaluator::ZoneResult> hits = GeoZonePathEvaluator(zones.catalog(), GeoZonePolicy::alerts())
                                                             .evaluate({ GeoZonePathEvaluator::Path::fromPositions(east) }).zones;
    QCOMPARE(hits.count(), 3);
    QCOMPARE(hits[0].zone->identifier(), QStringLiteral("A"));
    QCOMPARE(hits[1].zone->identifier(), QStringLiteral("X"));
    QCOMPARE(hits[2].zone->identifier(), QStringLiteral("B"));
    QVERIFY(qAbs(hits[0].crossings.constFirst().entry.distance - 800) < 1);
    QVERIFY(qAbs(hits[1].crossings.constFirst().entry.distance - 1900) < 1);
    QCOMPARE(hits[1].crossings.constFirst().entry.segment, 0);
    QVERIFY(qAbs(hits[2].crossings.constFirst().entry.distance - 2800) < 1);
    QCOMPARE(hits[2].crossings.constFirst().entry.segment, 1);
    QVERIFY(qAbs(center.distanceTo(hits[2].crossings.constFirst().entry.position.coordinate) - 2800) < 1);

    // The first path entering a zone reports it
    const QList<GeoZonePathEvaluator::ZoneResult> pathHits = GeoZonePathEvaluator(zones.catalog(), GeoZonePolicy({ GeoZone::Prohibited })).evaluate({
        GeoZonePathEvaluator::Path::fromPositions({ GeoZone::Position{ center.atDistanceAndAzimuth(1000, 0) },
                                                    GeoZone::Position{ center.atDistanceAndAzimuth(1000, 0).atDistanceAndAzimuth(4000, 90) } }),
        GeoZonePathEvaluator::Path::fromPositions(east) }).zones;
    QCOMPARE(pathHits.count(), 1);
    QCOMPARE(pathHits[0].crossings.constFirst().entry.path, 1);

    // Unknown restrictions are restricted, see GeoZonePlanningTest::_testGuidedPathWarning for the warning
    QVERIFY(GeoZonePolicy::isRestricted(GeoZone::Unknown));
    QVERIFY(!GeoZonePolicy::isRestricted(GeoZone::USpace));
}

void GeoZoneEvaluatorTest::_testCrossings()
{
    // A square 400 m wide 1000 m east of the start and a circle of 100 m radius 2000 m east of it
    const QGeoCoordinate start(47, 8);
    const QGeoCoordinate squareCenter = start.atDistanceAndAzimuth(1000, 90);
    const QGeoCoordinate circleCenter = start.atDistanceAndAzimuth(2000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("SQ"), QStringLiteral("PROHIBITED"), { squareCenter.atDistanceAndAzimuth(282.84, 315), squareCenter.atDistanceAndAzimuth(282.84, 45),
                                                                              squareCenter.atDistanceAndAzimuth(282.84, 135), squareCenter.atDistanceAndAzimuth(282.84, 225) }),
        _circleFeature(QStringLiteral("CI"), QStringLiteral("CONDITIONAL"), circleCenter, 100),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), circleCenter, 500),
    }), errorString), qPrintable(errorString));
    const GeoZonePathEvaluator evaluator(zones.catalog(), GeoZonePolicy({ GeoZone::Prohibited, GeoZone::Conditional }));

    // East through both, with a vertex inside the square, a labelled path across the circle, and one through the
    // square and back
    const GeoZonePathEvaluator::Path east = _evaluatorPath({ start, squareCenter, start.atDistanceAndAzimuth(3000, 90) });
    const GeoZonePathEvaluator::Path loiter = _evaluatorPath({ circleCenter.atDistanceAndAzimuth(500, 0), circleCenter.atDistanceAndAzimuth(500, 180) }, {},
                                                             QStringLiteral("the loiter"));
    const GeoZonePathEvaluator::Path back = _evaluatorPath({ squareCenter.atDistanceAndAzimuth(400, 270), squareCenter.atDistanceAndAzimuth(400, 90),
                                                             squareCenter.atDistanceAndAzimuth(400, 270) });
    const GeoZonePathEvaluator::Result result = evaluator.evaluate({ east, loiter, back });
    QCOMPARE(result.zones.count(), 2);
    QCOMPARE(result.activeEnteredZones(), QList<const GeoZone*>({ result.zones[0].zone, result.zones[1].zone }));

    // Continuous across the vertex inside the square, entering and leaving it twice on the way back
    const GeoZonePathEvaluator::ZoneResult &square = result.zones[0];
    QCOMPARE(square.zone->identifier(), QStringLiteral("SQ"));
    QCOMPARE(square.severity, 0);
    QCOMPARE(square.crossings.count(), 3);
    const GeoZonePathEvaluator::Crossing &squareCrossing = square.crossings[0];
    QCOMPARE(squareCrossing.entry.path, 0);
    QVERIFY(_near(squareCrossing.entry.distance, 800));
    QVERIFY(_near(squareCrossing.exit.distance, 1200));
    QCOMPARE(squareCrossing.entry.segment, 0);
    QVERIFY(_near(squareCrossing.entry.fraction, 0.8, 0.001));
    QCOMPARE(squareCrossing.exit.segment, 1);
    QVERIFY(_near(squareCrossing.exit.fraction, 0.1, 0.001));
    QVERIFY(_near(start.distanceTo(squareCrossing.entry.position.coordinate), 800));
    QVERIFY(_near(start.distanceTo(squareCrossing.exit.position.coordinate), 1200));
    QCOMPARE(square.crossings[1].entry.path, 2);
    QVERIFY(_near(square.crossings[1].entry.distance, 200));
    QVERIFY(_near(square.crossings[1].exit.distance, 600));
    QVERIFY(_near(square.crossings[2].entry.distance, 1000));
    QVERIFY(_near(square.crossings[2].exit.distance, 1400));
    // Without altitudes the path is inside the vertical limits wherever it is inside horizontally
    QCOMPARE(square.stretches.count(), 3);
    QCOMPARE(square.stretches[0].parts.count(), 1);
    QCOMPARE(square.stretches[0].parts[0].position, GeoZone::VerticalPosition::Inside);
    QVERIFY(_near(square.stretches[0].exit.distance, 1200));
    QVERIFY(square.labels.isEmpty());

    // Entered by the main path and by the labelled one
    const GeoZonePathEvaluator::ZoneResult &circle = result.zones[1];
    QCOMPARE(circle.zone->identifier(), QStringLiteral("CI"));
    QCOMPARE(circle.crossings.count(), 2);
    QVERIFY(_near(circle.crossings[0].entry.distance, 1900));
    QVERIFY(_near(circle.crossings[0].exit.distance, 2100));
    QCOMPARE(circle.crossings[1].entry.path, 1);
    QVERIFY(_near(circle.crossings[1].entry.distance, 400));
    QVERIFY(circle.labels.isEmpty());
    QCOMPARE(result.find(circle.zone), &circle);

    // Only entered by the labelled path, whose label tells why
    const GeoZonePathEvaluator::Result loiterResult = evaluator.evaluate({ _evaluatorPath({ start, start.atDistanceAndAzimuth(1000, 0) }), loiter });
    QCOMPARE(loiterResult.zones.count(), 1);
    QCOMPARE(loiterResult.zones[0].labels, QStringList({ QStringLiteral("the loiter") }));

    // A single position inside, and paths away from the zones
    const GeoZonePathEvaluator::Result single = evaluator.evaluate({ _evaluatorPath({ squareCenter }) });
    QCOMPARE(single.zones.count(), 1);
    QCOMPARE(single.zones[0].crossings.count(), 1);
    QCOMPARE(single.zones[0].crossings[0].entry.distance, 0.0);
    QCOMPARE(single.zones[0].crossings[0].exit.distance, 0.0);
    QVERIFY(evaluator.evaluate({ _evaluatorPath({ start, start.atDistanceAndAzimuth(3000, 0) }), GeoZonePathEvaluator::Path() }).zones.isEmpty());

    // One zone whatever the policy
    const auto permittedZone = std::find_if(zones.zoneList().cbegin(), zones.zoneList().cend(), [](const GeoZone *zone) { return zone->identifier() == QStringLiteral("N"); });
    QVERIFY(permittedZone != zones.zoneList().cend());
    const GeoZonePathEvaluator::ZoneResult permitted = GeoZonePathEvaluator(zones.catalog(), GeoZonePolicy()).evaluateZone(*permittedZone, { east });
    QCOMPARE(permitted.zone, *permittedZone);
    QCOMPARE(permitted.crossings.count(), 1);
    QVERIFY(_near(permitted.crossings[0].entry.distance, 1500));
}

void GeoZoneEvaluatorTest::_testAltitudes()
{
    // From 100 to 200 m above mean sea level, and from 50 to 150 m above the ground
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate groundCenter = center.atDistanceAndAzimuth(5000, 0);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _withLimits(_circleFeature(QStringLiteral("BAND"), QStringLiteral("PROHIBITED"), center, 300), 100, QStringLiteral("AMSL"), 200, QStringLiteral("AMSL")),
        _withLimits(_circleFeature(QStringLiteral("AGL"), QStringLiteral("PROHIBITED"), groundCenter, 300), 50, QStringLiteral("AGL"), 150, QStringLiteral("AGL")),
    }), errorString), qPrintable(errorString));
    GeoZonePathEvaluator evaluator(zones.catalog(), GeoZonePolicy({ GeoZone::Prohibited }));
    const QGeoCoordinate west = center.atDistanceAndAzimuth(1000, 270);
    const QGeoCoordinate east = center.atDistanceAndAzimuth(1000, 90);

    // Level above, below and inside the band
    const auto levelResult = [&](double altitude) { return evaluator.evaluate({ _evaluatorPath({ west, east }, { altitude, altitude }) }); };
    for (const auto &[altitude, position] : { std::make_pair(300.0, GeoZone::VerticalPosition::Above), std::make_pair(50.0, GeoZone::VerticalPosition::Below) }) {
        const GeoZonePathEvaluator::Result result = levelResult(altitude);
        QCOMPARE(result.zones.count(), 1);
        QVERIFY(!result.zones[0].entered());
        QVERIFY(result.activeEnteredZones().isEmpty());
        QCOMPARE(result.zones[0].stretches.count(), 1);
        QCOMPARE(result.zones[0].stretches[0].parts.count(), 1);
        QCOMPARE(result.zones[0].stretches[0].parts[0].position, position);
        QVERIFY(_near(result.zones[0].stretches[0].entry.distance, 700));
    }
    QCOMPARE(levelResult(150).zones[0].crossings.count(), 1);

    // Climbing from below the floor through the band to above the ceiling: -400 m at the west end to 600 m at the east end
    const GeoZonePathEvaluator::Result climb = evaluator.evaluate({ _evaluatorPath({ west, east }, { -400, 600 }) });
    QCOMPARE(climb.zones.count(), 1);
    const GeoZonePathEvaluator::Stretch &stretch = climb.zones[0].stretches.constFirst();
    QCOMPARE(stretch.parts.count(), 3);
    QCOMPARE(stretch.parts[0].position, GeoZone::VerticalPosition::Below);
    QVERIFY(_near(stretch.parts[0].from.distance, 700));
    QVERIFY(_near(stretch.parts[0].to.distance, 1000));
    QCOMPARE(stretch.parts[1].position, GeoZone::VerticalPosition::Inside);
    QVERIFY(_near(stretch.parts[1].to.distance, 1200));
    QVERIFY(_near(stretch.parts[1].to.position.altitudeAMSL, 200));
    QCOMPARE(stretch.parts[2].position, GeoZone::VerticalPosition::Above);
    QVERIFY(_near(stretch.parts[2].to.distance, 1300));
    QCOMPARE(climb.zones[0].crossings.count(), 1);
    QVERIFY(_near(climb.zones[0].crossings[0].entry.distance, 1000));
    QVERIFY(_near(climb.zones[0].crossings[0].exit.distance, 1200));

    // Relative altitudes, which are unknown without the reference
    GeoZonePathEvaluator::Path relative = _evaluatorPath({ west, east }, { 120, 120 });
    relative.altitudeReference = GeoZonePathEvaluator::AltitudeReference::Relative;
    relative.referenceAltitudeAMSL = 100;
    QCOMPARE(evaluator.evaluate({ relative }).zones[0].stretches[0].parts[0].position, GeoZone::VerticalPosition::Above);
    relative.referenceAltitudeAMSL = 0;
    QCOMPARE(evaluator.evaluate({ relative }).activeEnteredZones().count(), 1);
    relative.referenceAltitudeAMSL = qQNaN();
    QCOMPARE(evaluator.evaluate({ relative }).activeEnteredZones().count(), 1);

    // Limits above unknown ground are inside, the terrain lookup gives the ground and positions along the path
    const QGeoCoordinate groundWest = groundCenter.atDistanceAndAzimuth(1000, 270);
    const QGeoCoordinate groundEast = groundCenter.atDistanceAndAzimuth(1000, 90);
    const GeoZonePathEvaluator::Path high = _evaluatorPath({ groundWest, groundEast }, { 600, 600 });
    QCOMPARE(evaluator.evaluate({ high }).activeEnteredZones().count(), 1);
    int lookups = 0;
    evaluator.setTerrainLookup([&lookups](const QGeoCoordinate &) { lookups++; return 400.0; });
    const GeoZonePathEvaluator::Result overGround = evaluator.evaluate({ high });
    QVERIFY(lookups > 2);
    QCOMPARE(overGround.zones.count(), 1);
    QVERIFY(!overGround.zones[0].entered());
    QCOMPARE(overGround.zones[0].stretches[0].parts[0].position, GeoZone::VerticalPosition::Above);
    // Above the terrain, with vertexes of the input path despite the added positions
    GeoZonePathEvaluator::Path aboveGround = _evaluatorPath({ groundWest, groundEast }, { 100, 100 });
    aboveGround.altitudeReference = GeoZonePathEvaluator::AltitudeReference::Terrain;
    const GeoZonePathEvaluator::Result following = evaluator.evaluate({ aboveGround });
    QCOMPARE(following.activeEnteredZones().count(), 1);
    const GeoZonePathEvaluator::Crossing &crossing = following.zones[0].crossings[0];
    QCOMPARE(crossing.entry.segment, 0);
    QVERIFY(_near(crossing.entry.fraction, 0.35, 0.001));
    QVERIFY(_near(crossing.entry.position.altitudeAMSL, 500));
    QVERIFY(_near(crossing.entry.position.terrainAMSL, 400));

    // The conversion of the datums of a path gives the altitudes the evaluation compares, with the terrain of the
    // lookup where the vertices have none
    const GeoZonePathEvaluator::TerrainLookup ground = [](const QGeoCoordinate &) { return 400.0; };
    QVERIFY(!aboveGround.vertices.isEmpty() && !relative.vertices.isEmpty() && !high.vertices.isEmpty());
    QCOMPARE(aboveGround.altitudeAMSL(aboveGround.vertices.constFirst(), ground), 500.0);
    QVERIFY(qIsNaN(aboveGround.altitudeAMSL(aboveGround.vertices.constFirst())));
    aboveGround.vertices[0].terrainAMSL = 350;
    QCOMPARE(aboveGround.altitudeAMSL(aboveGround.vertices.constFirst(), ground), 450.0);
    QCOMPARE(aboveGround.altitudeAMSL(aboveGround.vertices.constFirst()), 450.0);
    QCOMPARE(aboveGround.altitudeAMSL(100, 400), 500.0);
    QVERIFY(qIsNaN(relative.altitudeAMSL(relative.vertices.constFirst(), ground)));
    relative.referenceAltitudeAMSL = 100;
    QCOMPARE(relative.altitudeAMSL(relative.vertices.constFirst(), ground), 220.0);
    QCOMPARE(high.altitudeAMSL(high.vertices.constFirst(), ground), 600.0);
}

void GeoZoneEvaluatorTest::_testAuthorisationScope()
{
    // A zone requiring authorisation 300 m around the center at all altitudes, authorised up to 120 m AMSL
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("CTR"), QStringLiteral("REQ_AUTHORISATION"), center, 300),
    }), errorString), qPrintable(errorString));
    const GeoZone *const zone = zones.zoneList().constFirst();
    GeoZoneAuthorisations authorisations{ QString() };
    GeoZoneAuthorisation authorisation;
    authorisation.zoneKey = GeoZoneAuthorisations::zoneKey(zone);
    authorisation.ceiling = 120;
    authorisations.set(authorisation);
    const GeoZonePolicy policy = GeoZonePolicy({ GeoZone::ReqAuthorisation }).excludingAuthorised(&authorisations);
    QVERIFY(policy.authorisation(zone));
    QVERIFY(!policy.authorised(zone));
    QVERIFY(policy.accepts(zone));
    const GeoZonePathEvaluator evaluator(zones.catalog(), policy);
    const QGeoCoordinate west = center.atDistanceAndAzimuth(1000, 270);
    const QGeoCoordinate east = center.atDistanceAndAzimuth(1000, 90);

    // Paths below the ceiling leave the zone out, those above it, or at unknown altitudes, don't
    QVERIFY(evaluator.evaluate({ _evaluatorPath({ west, east }, { 100, 100 }) }).zones.isEmpty());
    const QList<GeoZonePathEvaluator::Path> climb = { _evaluatorPath({ west, east }, { 100, 200 }) };
    const GeoZonePathEvaluator::Result high = evaluator.evaluate(climb);
    QCOMPARE(high.zones.count(), 1);
    QCOMPARE(evaluator.scopeIssue(high.zones[0], authorisation, climb), QStringLiteral("authorised up to 120 m AMSL, the flight reaches 165 m AMSL"));
    const QList<GeoZonePathEvaluator::Path> unknown = { _evaluatorPath({ west, east }) };
    QCOMPARE(evaluator.evaluate(unknown).zones.count(), 1);
    QCOMPARE(evaluator.scopeIssue(evaluator.evaluate(unknown).zones[0], authorisation, unknown),
             QStringLiteral("authorised up to 120 m AMSL, the height flown is unknown"));
    QVERIFY(policy.authorisedAt(zone, GeoZone::Position{ center, 100 }));
    QVERIFY(!policy.authorisedAt(zone, GeoZone::Position{ center, 150 }));
    QVERIFY(!policy.authorisedAt(zone, GeoZone::Position{ center }));

    // An area over the western half of the zone: paths staying in it leave the zone out, those leaving it don't
    authorisation.ceiling.reset();
    authorisation.area = { QGeoCoordinate(47.01, 7.98), QGeoCoordinate(47.01, 8), QGeoCoordinate(46.99, 8), QGeoCoordinate(46.99, 7.98) };
    authorisations.set(authorisation);
    QVERIFY(evaluator.evaluate({ _evaluatorPath({ west, center.atDistanceAndAzimuth(50, 270) }, { 100, 100 }) }).zones.isEmpty());
    const QList<GeoZonePathEvaluator::Path> across = { _evaluatorPath({ west, east }, { 100, 100 }) };
    const GeoZonePathEvaluator::Result leaving = evaluator.evaluate(across);
    QCOMPARE(leaving.zones.count(), 1);
    QCOMPARE(evaluator.scopeIssue(leaving.zones[0], authorisation, across), QStringLiteral("the flight leaves the authorised area"));
    QVERIFY(policy.authorisedAt(zone, GeoZone::Position{ center.atDistanceAndAzimuth(100, 270), 100 }));
    QVERIFY(!policy.authorisedAt(zone, GeoZone::Position{ center.atDistanceAndAzimuth(100, 90), 100 }));
    // Pattern areas must be inside it too
    GeoZone::Area pattern;
    pattern.polygon = { center.atDistanceAndAzimuth(200, 315), center.atDistanceAndAzimuth(200, 225), center.atDistanceAndAzimuth(50, 270) };
    pattern.lowest.altitudeAMSL = pattern.highest.altitudeAMSL = 100;
    QVERIFY(evaluator.evaluate({}, { pattern }).zones.isEmpty());
    pattern.polygon.append(center.atDistanceAndAzimuth(100, 90));
    QCOMPARE(evaluator.evaluate({}, { pattern }).zones.count(), 1);

    // Daily hours in local time, also across midnight
    const QDate day(2026, 6, 15);
    authorisation.dailyFrom = QTime(8, 0);
    authorisation.dailyTo = QTime(18, 0);
    QVERIFY(authorisation.covers(QDateTime(day, QTime(8, 0)), QDateTime(day, QTime(18, 0))));
    QVERIFY(!authorisation.covers(QDateTime(day, QTime(7, 30)), QDateTime(day, QTime(9, 0))));
    QVERIFY(!authorisation.covers(QDateTime(day, QTime(17, 0)), QDateTime(day, QTime(18, 30))));
    QCOMPARE(GeoZoneAuthorisations::uncoveredText(authorisation, QDateTime(day, QTime(7, 30)), QDateTime(day, QTime(9, 0))), QStringLiteral("authorised 08:00–18:00 daily"));
    authorisation.dailyFrom = QTime(22, 0);
    authorisation.dailyTo = QTime(6, 0);
    QVERIFY(authorisation.covers(QDateTime(day, QTime(23, 0)), QDateTime(day.addDays(1), QTime(5, 0))));
    QVERIFY(authorisation.covers(QDateTime(day, QTime(1, 0)), QDateTime(day, QTime(2, 0))));
    QVERIFY(!authorisation.covers(QDateTime(day, QTime(21, 0)), QDateTime(day, QTime(23, 0))));

    // Kept in files and plans, where authorisations without scope cover the whole zone
    authorisation.ceiling = 90;
    authorisation.ceilingAboveGround = true;
    authorisation.aircraft = QStringLiteral("HB-ABC");
    QCOMPARE(GeoZoneAuthorisation::fromJson(authorisation.toJson()), authorisation);
    QCOMPARE(GeoZoneAuthorisations::scopeText(authorisation), QStringLiteral("up to 90 m AGL, 22:00–06:00 daily, within its area, aircraft HB-ABC"));
    const std::optional<GeoZoneAuthorisation> unlimited = GeoZoneAuthorisation::fromJson(QJsonObject{ { QStringLiteral("zone"), QStringLiteral("CHE/X") } });
    QVERIFY(unlimited && !unlimited->limitsPath() && !unlimited->hasDailyHours());
}

void GeoZoneEvaluatorTest::_testAuthorisationScopeKept()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("CTR"), QStringLiteral("REQ_AUTHORISATION"), center, 300),
        _circleFeature(QStringLiteral("TMA"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(2000, 90), 300),
    });
    QVERIFY2(zones.loadFile(fileName, errorString), qPrintable(errorString));
    QCOMPARE(zones.zoneList().count(), 2);
    const GeoZone *zone = zones.zoneList().constFirst();
    const GeoZone *const other = zones.zoneList().constLast();
    QVERIFY(!zone->data().contentHash.isEmpty());
    GeoZoneAuthorisation authorisation;
    authorisation.zoneKey = GeoZoneAuthorisations::zoneKey(zone);
    authorisation.ceiling = 120;
    const GeoZone::Position low{ center, 100 };

    // The scope of a zone is made once for the positions checked against it
    const std::shared_ptr<const GeoZone> scope = authorisation.scopeZone(zone);
    QVERIFY(scope);
    QVERIFY(authorisation.scopeContains(zone, low));
    QVERIFY(!authorisation.scopeContains(zone, GeoZone::Position{ center, 150 }));
    QCOMPARE(authorisation.scopeZone(zone).get(), scope.get());

    // and again when its limits or its zone change
    authorisation.ceiling = 50;
    const std::shared_ptr<const GeoZone> lower = authorisation.scopeZone(zone);
    QVERIFY(lower.get() != scope.get());
    QVERIFY(!authorisation.scopeContains(zone, low));
    authorisation.ceilingAboveGround = true;
    QVERIFY(authorisation.scopeZone(zone).get() != lower.get());
    authorisation.ceiling.reset();
    authorisation.ceilingAboveGround = false;
    authorisation.area = { QGeoCoordinate(47.01, 7.98), QGeoCoordinate(47.01, 8), QGeoCoordinate(46.99, 8), QGeoCoordinate(46.99, 7.98) };
    const std::shared_ptr<const GeoZone> western = authorisation.scopeZone(zone);
    QVERIFY(authorisation.scopeContains(zone, GeoZone::Position{ center.atDistanceAndAzimuth(100, 270), 100 }));
    QVERIFY(!authorisation.scopeContains(zone, GeoZone::Position{ center.atDistanceAndAzimuth(100, 90), 100 }));
    QCOMPARE(authorisation.scopeZone(zone).get(), western.get());
    authorisation.areaCircle = QGeoCircle(center.atDistanceAndAzimuth(100, 90), 50);
    QVERIFY(authorisation.scopeZone(zone).get() != western.get());
    QVERIFY(authorisation.scopeContains(zone, GeoZone::Position{ center.atDistanceAndAzimuth(100, 90), 100 }));
    authorisation.area.clear();
    authorisation.areaCircle = QGeoCircle();
    const std::shared_ptr<const GeoZone> whole = authorisation.scopeZone(zone);
    QVERIFY(authorisation.scopeContains(zone, low));
    const std::shared_ptr<const GeoZone> otherScope = authorisation.scopeZone(other);
    QVERIFY(otherScope.get() != whole.get());
    QVERIFY(!authorisation.scopeContains(other, low));
    QVERIFY(authorisation.scopeContains(zone, low));

    // It is no part of the authorisation, whose copies make their own
    const GeoZoneAuthorisation copy = authorisation;
    QCOMPARE(copy, authorisation);
    const std::shared_ptr<const GeoZone> kept = authorisation.scopeZone(zone);
    QVERIFY(copy.scopeZone(zone).get() != kept.get());
    QCOMPARE(copy, authorisation);
    QCOMPARE(GeoZoneAuthorisation::fromJson(authorisation.toJson()), authorisation);

    // A zone whose content changed is another one for the scope, whatever its address: the scope follows it
    authorisation.ceiling = 120;
    const GeoZone::Position east{ center.atDistanceAndAzimuth(100, 90), 100 };
    QVERIFY(authorisation.scopeContains(zone, east));
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("CTR"), QStringLiteral("REQ_AUTHORISATION"), center, 50),
        _circleFeature(QStringLiteral("TMA"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(2000, 90), 300),
    }), errorString), qPrintable(errorString));
    QCOMPARE(zones.zoneList().count(), 2);
    zone = zones.zoneList().constFirst();
    QCOMPARE(zone->identifier(), QStringLiteral("CTR"));
    QVERIFY(!authorisation.scopeContains(zone, east));
    QVERIFY(authorisation.scopeContains(zone, low));
}

void GeoZoneEvaluatorTest::_testAuthorisationLimits_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<QJsonValue>("value");
    QTest::addColumn<bool>("accepted");

    const QJsonArray area{ QJsonArray{ 47.01, 7.98 }, QJsonArray{ 47.01, 8 }, QJsonArray{ 46.99, 8 } };
    const auto circle = [](const QJsonValue &lat, const QJsonValue &radius) {
        return QJsonObject{ { QStringLiteral("lat"), lat }, { QStringLiteral("lon"), 8 }, { QStringLiteral("radius"), radius } };
    };
    // As written
    QTest::newRow("validFrom") << QStringLiteral("validFrom") << QJsonValue(QStringLiteral("2026-06-01T00:00:00Z")) << true;
    QTest::newRow("validTo") << QStringLiteral("validTo") << QJsonValue(QStringLiteral("2026-12-31T00:00:00Z")) << true;
    QTest::newRow("area") << QStringLiteral("area") << QJsonValue(area) << true;
    QTest::newRow("areaCircle") << QStringLiteral("areaCircle") << QJsonValue(circle(47, 250)) << true;
    QTest::newRow("kind notification") << QStringLiteral("kind") << QJsonValue(QStringLiteral("notification")) << true;
    QTest::newRow("kind authorisation") << QStringLiteral("kind") << QJsonValue(QStringLiteral("authorisation")) << true;
    // Limits which are there but can't be read would be no limits
    QTest::newRow("validTo not a date") << QStringLiteral("validTo") << QJsonValue(QStringLiteral("31.12.2026")) << false;
    QTest::newRow("validTo a number") << QStringLiteral("validTo") << QJsonValue(20261231) << false;
    QTest::newRow("validTo empty") << QStringLiteral("validTo") << QJsonValue(QString()) << false;
    QTest::newRow("validFrom not a date") << QStringLiteral("validFrom") << QJsonValue(QStringLiteral("tomorrow")) << false;
    QTest::newRow("ceiling a text") << QStringLiteral("ceiling") << QJsonValue(QStringLiteral("120")) << false;
    QTest::newRow("ceiling without reference") << QStringLiteral("ceiling") << QJsonValue(120) << false;
    QTest::newRow("reference without ceiling") << QStringLiteral("ceilingReference") << QJsonValue(QStringLiteral("AGL")) << false;
    QTest::newRow("area not a list") << QStringLiteral("area") << QJsonValue(QStringLiteral("47,8 47,9 46,9")) << false;
    QTest::newRow("area of two vertices") << QStringLiteral("area") << QJsonValue(QJsonArray{ area[0], area[1] }) << false;
    QTest::newRow("area with an invalid vertex") << QStringLiteral("area") << QJsonValue(QJsonArray{ area[0], area[1], area[2], QJsonArray{ 147.0, 8 } }) << false;
    QTest::newRow("area with a text vertex") << QStringLiteral("area") << QJsonValue(QJsonArray{ area[0], area[1], area[2], QStringLiteral("47,8") }) << false;
    QTest::newRow("areaCircle not an object") << QStringLiteral("areaCircle") << QJsonValue(250) << false;
    QTest::newRow("areaCircle without center") << QStringLiteral("areaCircle") << QJsonValue(circle(QJsonValue(), 250)) << false;
    QTest::newRow("areaCircle without radius") << QStringLiteral("areaCircle") << QJsonValue(circle(47, QJsonValue())) << false;
    QTest::newRow("areaCircle of no radius") << QStringLiteral("areaCircle") << QJsonValue(circle(47, 0)) << false;
    QTest::newRow("dailyFrom alone") << QStringLiteral("dailyFrom") << QJsonValue(QStringLiteral("08:00")) << false;
    QTest::newRow("dailyTo alone") << QStringLiteral("dailyTo") << QJsonValue(QStringLiteral("18:00")) << false;
    QTest::newRow("kind unknown") << QStringLiteral("kind") << QJsonValue(QStringLiteral("request")) << false;
    QTest::newRow("kind a number") << QStringLiteral("kind") << QJsonValue(1) << false;
}

void GeoZoneEvaluatorTest::_testAuthorisationLimits()
{
    QFETCH(QString, key);
    QFETCH(QJsonValue, value);
    QFETCH(bool, accepted);

    // An authorisation without limits, with the one of the row
    QJsonObject json{ { QStringLiteral("zone"), QStringLiteral("CHE/X") }, { QStringLiteral("permit"), QStringLiteral("P-1") } };
    QVERIFY(GeoZoneAuthorisation::fromJson(json));
    json.insert(key, value);
    const std::optional<GeoZoneAuthorisation> authorisation = GeoZoneAuthorisation::fromJson(json);
    QCOMPARE(authorisation.has_value(), accepted);
    if (authorisation) {
        QVERIFY(authorisation->limitsValid());
        QCOMPARE(GeoZoneAuthorisation::fromJson(authorisation->toJson()), authorisation);
    }

    if (key == QStringLiteral("ceiling")) {
        // Read with its reference only
        json.insert(QStringLiteral("ceilingReference"), QStringLiteral("AGL"));
        QCOMPARE(GeoZoneAuthorisation::fromJson(json).has_value(), value.isDouble());
        json.insert(QStringLiteral("ceilingReference"), QStringLiteral("FL"));
        QVERIFY(!GeoZoneAuthorisation::fromJson(json));
        for (const double ceiling : { 0.0, -120.0 }) {
            json.insert(QStringLiteral("ceiling"), ceiling);
            json.insert(QStringLiteral("ceilingReference"), QStringLiteral("AMSL"));
            QVERIFY(!GeoZoneAuthorisation::fromJson(json));
        }
    } else if (key.startsWith(QStringLiteral("daily"))) {
        // Both hours, as times of day
        json.insert(QStringLiteral("dailyFrom"), QStringLiteral("08:00"));
        json.insert(QStringLiteral("dailyTo"), QStringLiteral("18:00"));
        QVERIFY(GeoZoneAuthorisation::fromJson(json));
        json.insert(key, QStringLiteral("25:00"));
        QVERIFY(!GeoZoneAuthorisation::fromJson(json));
    } else if (key == QStringLiteral("validFrom")) {
        // A period which ends before it starts
        json.insert(QStringLiteral("validFrom"), QStringLiteral("2026-06-02T00:00:00Z"));
        json.insert(QStringLiteral("validTo"), QStringLiteral("2026-06-01T00:00:00Z"));
        QVERIFY(!GeoZoneAuthorisation::fromJson(json));
    }
}

void GeoZoneEvaluatorTest::_testAuthorisationCeiling()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("CTR"), QStringLiteral("REQ_AUTHORISATION"), center, 300),
    }), errorString), qPrintable(errorString));
    const GeoZone *const zone = zones.zoneList().constFirst();
    const QVariant zoneInfo = QVariant::fromValue(GeoZoneInfo(zone));
    GeoZoneAuthorisations authorisations{ QString() };
    const auto authorise = [&authorisations, &zoneInfo](const QString &ceiling) {
        return authorisations.authorise(zoneInfo, QStringLiteral("P-1"), QString(), QString(), { { QStringLiteral("ceiling"), ceiling } });
    };
    const auto shownCeiling = [&authorisations, &zoneInfo]() { return authorisations.authorisationOf(zoneInfo).value(QStringLiteral("ceiling")).toString(); };

    // From the height published for zones without ceiling a ceiling would limit nothing, and huge ones can't be shown
    for (const char *const ceiling : { "1e300", "1e9", "100000", "99999", "inf", "nan", "0", "-120" }) {
        QVERIFY2(!authorise(QString::fromLatin1(ceiling)).isEmpty(), ceiling);
    }
    QCOMPARE(authorisations.count(), 0);
    QCOMPARE(authorise(QStringLiteral("99998")), QString());
    QCOMPARE(shownCeiling(), QStringLiteral("99998"));
    QVERIFY(GeoZonePolicy::alerts().excludingAuthorised(&authorisations).authorisedAt(zone, GeoZone::Position{ center, 99997 }));
    QVERIFY(!GeoZonePolicy::alerts().excludingAuthorised(&authorisations).authorisedAt(zone, GeoZone::Position{ center, 100500 }));
    {
        // In the units shown
        const auto feet = _setVerticalUnits(true);
        QVERIFY(!authorise(QStringLiteral("328084")).isEmpty());
        QCOMPARE(authorise(QStringLiteral("328000")), QString());
        QCOMPARE(shownCeiling(), QStringLiteral("328000"));
    }

    // Read the same
    QJsonObject json = authorisations.authorisations().constFirst().toJson();
    QVERIFY(GeoZoneAuthorisation::fromJson(json));
    for (const double ceiling : { 1e300, 1e9, 99999.0 }) {
        json.insert(QStringLiteral("ceiling"), ceiling);
        QVERIFY2(!GeoZoneAuthorisation::fromJson(json), qPrintable(QString::number(ceiling)));
    }
    GeoZoneAuthorisation unlimited = authorisations.authorisations().constFirst();
    unlimited.ceiling = GeoZoneVertical::unlimitedSentinel;
    QVERIFY(!unlimited.limitsValid());
}

void GeoZoneEvaluatorTest::_testProhibitedAuthorisations()
{
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(12, 0)));
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(2000, 90), 300),
    }), errorString), qPrintable(errorString));
    QCOMPARE(zones.zoneList().count(), 2);
    const GeoZone *const prohibited = zones.zoneList().constFirst();
    const GeoZone *const restricted = zones.zoneList().constLast();
    QCOMPARE(prohibited->restriction(), GeoZone::Prohibited);
    const QVariant prohibitedInfo = QVariant::fromValue(GeoZoneInfo(prohibited));
    GeoZoneAuthorisations authorisations{ QString() };
    const GeoZonePolicy policy = GeoZonePolicy::alerts().excludingAuthorised(&authorisations);
    const QString lastDay = QStringLiteral("2026-12-31");

    // A prohibited zone is only authorised with a permit reference and a last day
    QVERIFY(!authorisations.authorise(prohibitedInfo, QString(), QString(), QString()).isEmpty());
    QVERIFY(!authorisations.authorise(prohibitedInfo, QStringLiteral("P-1"), QString(), QString()).isEmpty());
    QVERIFY(!authorisations.authorise(prohibitedInfo, QStringLiteral("  "), QString(), lastDay).isEmpty());
    QCOMPARE(authorisations.count(), 0);
    QVERIFY(policy.accepts(prohibited));
    QCOMPARE(authorisations.authorise(prohibitedInfo, QStringLiteral("P-1"), QString(), lastDay), QString());
    QCOMPARE(authorisations.count(), 1);
    QVERIFY(authorisations.find(prohibited));
    QVERIFY(!policy.accepts(prohibited));
    // Which ends
    environment.setNow(_utc(QDate(2027, 1, 2), QTime(12, 0)));
    QVERIFY(policy.accepts(prohibited));
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(12, 0)));

    // Kept as the authorisation of a prohibited zone, which isn't read without them
    const GeoZoneAuthorisation stored = authorisations.authorisations().constFirst();
    QVERIFY(stored.prohibitedZone);
    QJsonObject json = stored.toJson();
    QCOMPARE(GeoZoneAuthorisation::fromJson(json), stored);
    json.remove(QStringLiteral("validTo"));
    QVERIFY(!GeoZoneAuthorisation::fromJson(json));
    json = stored.toJson();
    json.remove(QStringLiteral("permit"));
    QVERIFY(!GeoZoneAuthorisation::fromJson(json));
    json = stored.toJson();
    json.insert(QStringLiteral("prohibited"), QStringLiteral("no"));
    QVERIFY(!GeoZoneAuthorisation::fromJson(json));

    // An entry which doesn't say so, like one of an earlier version or of a plan, doesn't stand for the zone, which is
    // prohibited now. It is shown to be completed.
    authorisations.revoke(stored.zoneKey);
    for (const GeoZoneAuthorisation &incomplete : { GeoZoneAuthorisation{ stored.zoneKey, stored.zoneName, QStringLiteral("P-1"), QDateTime(), QDateTime() },
                                                    GeoZoneAuthorisation{ stored.zoneKey, stored.zoneName, QString(), QDateTime(), stored.validTo } }) {
        QVERIFY(!incomplete.prohibitedZone);
        authorisations.set(incomplete);
        QCOMPARE(authorisations.count(), 1);
        QVERIFY(!authorisations.find(prohibited));
        QVERIFY(policy.accepts(prohibited));
        QVERIFY(!policy.authorisedAt(prohibited, GeoZone::Position{ center, 100 }));
        const QVariantMap shown = authorisations.authorisationOf(prohibitedInfo);
        QCOMPARE(shown.value(QStringLiteral("permit")).toString(), incomplete.permit);
        QCOMPARE(shown.value(QStringLiteral("statusText")).toString(),
                 incomplete.permit.isEmpty() ? QStringLiteral("Not valid for a prohibited zone: add the permit reference") :
                                               QStringLiteral("Not valid for a prohibited zone: add the last day"));
    }
    GeoZoneAuthorisation complete{ stored.zoneKey, stored.zoneName, QStringLiteral("P-1"), QDateTime(), stored.validTo };
    authorisations.set(complete);
    QVERIFY(authorisations.find(prohibited));
    QVERIFY(!policy.accepts(prohibited));
    authorisations.revoke(stored.zoneKey);

    // The notification of its authority authorises nothing, its reference and end stay optional
    QCOMPARE(authorisations.recordNotification(prohibitedInfo, QString(), QString(), QString()), QString());
    QVERIFY(authorisations.findNotification(prohibited));
    QVERIFY(!authorisations.find(prohibited));
    QVERIFY(policy.accepts(prohibited));

    // Other restricted zones keep their optional permit and end
    QCOMPARE(authorisations.authorise(QVariant::fromValue(GeoZoneInfo(restricted)), QString(), QString(), QString()), QString());
    QVERIFY(authorisations.find(restricted));
    QVERIFY(!authorisations.find(restricted)->prohibitedZone);
    QVERIFY(!policy.accepts(restricted));

    // The list shows both by the names of its roles
    QCOMPARE(authorisations.rowCount(), 2);
    const QHash<int, QByteArray> roles = authorisations.roleNames();
    QCOMPARE(roles.count(), 5);
    QCOMPARE(roles, authorisations.roleNames());
    const auto shown = [&authorisations, &roles](int row, const char *role) { return authorisations.data(authorisations.index(row), roles.key(role)); };
    QCOMPARE(shown(0, "zoneKey").toString(), GeoZoneAuthorisations::zoneKey(prohibited));
    QVERIFY(shown(0, "notification").toBool());
    QCOMPARE(shown(0, "statusText").toString(), QStringLiteral("Notified, valid"));
    QCOMPARE(shown(1, "zoneKey").toString(), GeoZoneAuthorisations::zoneKey(restricted));
    QCOMPARE(shown(1, "zoneName").toString(), restricted->displayName());
    QVERIFY(shown(1, "permit").toString().isEmpty());
    QVERIFY(!shown(1, "notification").toBool());
    QCOMPARE(shown(1, "statusText").toString(), QStringLiteral("Valid"));
}

void GeoZoneEvaluatorTest::_testAuthorisationFile()
{
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(12, 0)));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filePath = dir.filePath(QString::fromLatin1(GeoZoneAuthorisations::fileName));
    const auto fileBytes = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto writeFile = [&filePath](const QByteArray &bytes) {
        QFile file(filePath);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && (file.write(bytes) == bytes.size());
    };
    const auto backups = [&dir]() { return QDir(dir.path()).entryList({ QStringLiteral("*.bak") }, QDir::Files); };
    const auto entryCount = [&fileBytes, &filePath]() {
        return QJsonDocument::fromJson(fileBytes(filePath)).object().value(QStringLiteral("authorisations")).toArray().count();
    };
    const GeoZoneAuthorisation first{ QStringLiteral("CHE/A"), QStringLiteral("A"), QStringLiteral("P-1"), QDateTime(), _utc(QDate(2026, 12, 31), QTime(0, 0)) };
    const GeoZoneAuthorisation second{ QStringLiteral("CHE/B"), QStringLiteral("B"), QStringLiteral("P-2"), QDateTime(), QDateTime() };

    // Written with its version and read again
    {
        GeoZoneAuthorisations authorisations(filePath);
        QVERIFY(authorisations.loadError().isEmpty());
        authorisations.set(first);
    }
    QCOMPARE(QJsonDocument::fromJson(fileBytes(filePath)).object().value(QStringLiteral("version")).toInt(), 1);
    {
        const GeoZoneAuthorisations authorisations(filePath);
        QVERIFY(authorisations.loadError().isEmpty());
        QCOMPARE(authorisations.authorisations(), QList<GeoZoneAuthorisation>{ first });
    }
    QVERIFY(backups().isEmpty());
    const QByteArray valid = fileBytes(filePath);

    // A file which can't be read isn't replaced by the next change: it is moved to a backup, and the failure is told
    for (const QByteArray &unreadable : { valid.left(valid.size() / 2), QByteArray("[]"), QByteArray(R"({ "authorisations": [] })"),
                                          QByteArray(R"({ "version": 1, "authorisations": {} })"), QByteArray(R"({ "version": "1", "authorisations": [] })") }) {
        QVERIFY(writeFile(unreadable));
        {
            GeoZoneAuthorisations authorisations(filePath);
            QSignalSpy loadSpy(&authorisations, &GeoZoneAuthorisations::loadFailed);
            QSignalSpy saveSpy(&authorisations, &GeoZoneAuthorisations::saveFailed);
            QCOMPARE(authorisations.count(), 0);
            QVERIFY(!authorisations.loadError().isEmpty());
            QTRY_COMPARE(loadSpy.count(), 1);
            QCOMPARE(loadSpy.constFirst().constFirst().toString(), authorisations.loadError());
            const QStringList kept = backups();
            QCOMPARE(kept.count(), 1);
            QVERIFY2(authorisations.loadError().contains(kept.constFirst()), qPrintable(authorisations.loadError()));
            QCOMPARE(fileBytes(dir.filePath(kept.constFirst())), unreadable);
            QVERIFY(!QFile::exists(filePath));
            // Authorisations added then are kept
            authorisations.set(second);
            QCOMPARE(saveSpy.count(), 0);
            QCOMPARE(entryCount(), 1);
            QCOMPARE(fileBytes(dir.filePath(kept.constFirst())), unreadable);
            QVERIFY(QFile::remove(dir.filePath(kept.constFirst())));
        }
        QCOMPARE(GeoZoneAuthorisations(filePath).authorisations(), QList<GeoZoneAuthorisation>{ second });
    }

    // Entries which can't be read are left out, the file with them is copied to a backup before the others are saved
    QJsonObject root = QJsonDocument::fromJson(valid).object();
    QJsonObject damaged = second.toJson();
    damaged.insert(QStringLiteral("validTo"), QStringLiteral("never"));
    root.insert(QStringLiteral("authorisations"), QJsonArray{ first.toJson(), damaged, 7 });
    const QByteArray partial = QJsonDocument(root).toJson();
    QVERIFY(writeFile(partial));
    {
        GeoZoneAuthorisations authorisations(filePath);
        QSignalSpy loadSpy(&authorisations, &GeoZoneAuthorisations::loadFailed);
        QCOMPARE(authorisations.authorisations(), QList<GeoZoneAuthorisation>{ first });
        QVERIFY2(authorisations.loadError().startsWith(QStringLiteral("2 geozone authorisation(s) could not be read")), qPrintable(authorisations.loadError()));
        QTRY_COMPARE(loadSpy.count(), 1);
        const QStringList kept = backups();
        QCOMPARE(kept.count(), 1);
        QCOMPARE(fileBytes(dir.filePath(kept.constFirst())), partial);
        QCOMPARE(fileBytes(filePath), partial);
        authorisations.revoke(first.zoneKey);
        QCOMPARE(entryCount(), 0);
        QCOMPARE(fileBytes(dir.filePath(kept.constFirst())), partial);
        QVERIFY(QFile::remove(dir.filePath(kept.constFirst())));
    }

    // A file of a newer version is neither read nor written, what is added is kept for the session
    root.insert(QStringLiteral("version"), 2);
    root.insert(QStringLiteral("authorisations"), QJsonArray{ first.toJson() });
    const QByteArray newer = QJsonDocument(root).toJson();
    QVERIFY(writeFile(newer));
    {
        GeoZoneAuthorisations authorisations(filePath);
        QSignalSpy loadSpy(&authorisations, &GeoZoneAuthorisations::loadFailed);
        QSignalSpy saveSpy(&authorisations, &GeoZoneAuthorisations::saveFailed);
        QCOMPARE(authorisations.count(), 0);
        QVERIFY2(authorisations.loadError().contains(QStringLiteral("newer version")), qPrintable(authorisations.loadError()));
        QTRY_COMPARE(loadSpy.count(), 1);
        authorisations.set(second);
        authorisations.set(first);
        QCOMPARE(authorisations.count(), 2);
        QCOMPARE(saveSpy.count(), 1);
    }
    QCOMPARE(fileBytes(filePath), newer);
    QVERIFY(backups().isEmpty());

    // A file which can neither be read nor moved away is left as it is
    QVERIFY(writeFile("{"));
    const QFile::Permissions permissions = QFile::permissions(dir.path());
    const auto restore = qScopeGuard([&dir, permissions]() { (void) QFile::setPermissions(dir.path(), permissions); });
    QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::ExeOwner));
    if (!QFileInfo(dir.path()).isWritable()) {
        GeoZoneAuthorisations authorisations(filePath);
        QSignalSpy saveSpy(&authorisations, &GeoZoneAuthorisations::saveFailed);
        QVERIFY2(authorisations.loadError().contains(QStringLiteral("left as it is")), qPrintable(authorisations.loadError()));
        authorisations.set(second);
        QCOMPARE(authorisations.count(), 1);
        QCOMPARE(saveSpy.count(), 1);
        QCOMPARE(fileBytes(filePath), QByteArray("{"));
    }
}

void GeoZoneEvaluatorTest::_testAuthorisationList()
{
    // The list of the authorisations as a value, without the model which keeps it in a file
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center, 300),
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2000, 90), 300),
    }), errorString), qPrintable(errorString));
    QCOMPARE(zones.zoneList().count(), 2);
    const GeoZone *const restricted = zones.zoneList().constFirst();
    const GeoZone *const prohibited = zones.zoneList().constLast();
    const QDateTime june = _utc(QDate(2026, 6, 30), QTime(0, 0));
    const QDateTime december = _utc(QDate(2026, 12, 31), QTime(0, 0));
    const GeoZoneAuthorisation permit{ GeoZoneAuthorisations::zoneKey(restricted), QStringLiteral("A"), QStringLiteral("P-1"), QDateTime(), june };
    GeoZoneAuthorisation notification = permit;
    notification.notification = true;
    const GeoZoneAuthorisation incomplete{ GeoZoneAuthorisations::zoneKey(prohibited), QStringLiteral("P"), QString(), QDateTime(), QDateTime() };

    // Entries are set by zone and kind
    GeoZoneAuthorisationList list;
    QVERIFY(list.isEmpty());
    QVERIFY(!list.find(restricted));
    QVERIFY(list.set(permit));
    QVERIFY(!list.set(permit));
    QVERIFY(list.set(notification));
    QVERIFY(list.set(incomplete));
    QCOMPARE(list.count(), 3);
    const GeoZoneAuthorisation *const found = list.find(restricted);
    QVERIFY(found);
    QCOMPARE(*found, permit);
    const GeoZoneAuthorisation *const notified = list.findNotification(restricted);
    QVERIFY(notified);
    QCOMPARE(*notified, notification);
    QVERIFY(!list.find(nullptr));
    // The entry of a prohibited zone is there, and doesn't stand for it without permit and end
    QVERIFY(list.entry(prohibited, false));
    QVERIFY(!list.find(prohibited));
    QVERIFY(!list.findNotification(prohibited));

    // Merging keeps, of the same zone and kind, the one which ends later
    GeoZoneAuthorisation longer = permit;
    longer.permit = QStringLiteral("P-2");
    longer.validTo = december;
    GeoZoneAuthorisation shorter = permit;
    shorter.permit = QStringLiteral("P-0");
    shorter.validTo = june.addDays(-1);
    QCOMPARE(list.merged({ shorter, permit }), list);
    const GeoZoneAuthorisationList merged = list.merged({ shorter, longer });
    QVERIFY(merged != list);
    QCOMPARE(merged.count(), 3);
    const GeoZoneAuthorisation *const extended = merged.find(restricted);
    QVERIFY(extended);
    QCOMPARE(extended->permit, QStringLiteral("P-2"));
    QCOMPARE(list.entries().constFirst(), permit);

    // Saved and read, without the entries which can't be
    QCOMPARE(GeoZoneAuthorisationList::fromJson(list.toJson()), list);
    QJsonArray values = list.toJson();
    QJsonObject damaged = values[0].toObject();
    damaged.insert(QStringLiteral("validTo"), QStringLiteral("soon"));
    values[0] = damaged;
    int unread = 0;
    const GeoZoneAuthorisationList read = GeoZoneAuthorisationList::fromJson(values, &unread);
    QCOMPARE(unread, 1);
    QCOMPARE(read.entries(), (QList<GeoZoneAuthorisation>{ notification, incomplete }));

    // Removed by zone and kind
    QVERIFY(list.remove(permit.zoneKey, true));
    QVERIFY(!list.remove(permit.zoneKey, true));
    QVERIFY(!list.findNotification(restricted));
    QVERIFY(list.find(restricted));
    QVERIFY(list.remove(permit.zoneKey, false));
    QCOMPARE(list.entries(), QList<GeoZoneAuthorisation>{ incomplete });

    // The model keeps one
    GeoZoneAuthorisations model{ QString() };
    model.merge(merged.entries());
    QCOMPARE(model.list(), merged);
    QCOMPARE(model.authorisations(), merged.entries());
    QCOMPARE(model.find(restricted), model.list().find(restricted));
}

void GeoZoneEvaluatorTest::_testPreparedPaths()
{
    // Two zones on a path, one above the ground and one requiring an authorisation the operator has up to 120 m AMSL
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _withLimits(_circleFeature(QStringLiteral("AGL"), QStringLiteral("PROHIBITED"), center, 300), 50, QStringLiteral("AGL"), 150, QStringLiteral("AGL")),
        _circleFeature(QStringLiteral("CTR"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(2000, 90), 300),
    }), errorString), qPrintable(errorString));
    QCOMPARE(zones.zoneList().count(), 2);
    const GeoZone *const controlZone = zones.zoneList().constLast();
    GeoZoneAuthorisation authorisation;
    authorisation.zoneKey = GeoZoneAuthorisations::zoneKey(controlZone);
    authorisation.ceiling = 120;
    int lookups = 0;
    GeoZonePathEvaluator evaluator(zones.catalog(), GeoZonePolicy({ GeoZone::Prohibited, GeoZone::ReqAuthorisation }));
    evaluator.setTerrainLookup([&lookups](const QGeoCoordinate &) { lookups++; return 400.0; });
    const QList<GeoZonePathEvaluator::Path> paths = { _evaluatorPath({ center.atDistanceAndAzimuth(1000, 270), center.atDistanceAndAzimuth(3000, 90) }, { 500, 500 }) };

    // The terrain is looked up when the paths are prepared, not again by each evaluation of them
    const GeoZonePathEvaluator::PreparedPaths prepared = evaluator.prepare(paths);
    const int prepareLookups = lookups;
    QVERIFY(prepareLookups > 2);
    const GeoZonePathEvaluator::Result result = evaluator.evaluate(prepared);
    QCOMPARE(result.zones.count(), 2);
    QStringList issues;
    for (const GeoZone *zone : zones.zoneList()) {
        const GeoZonePathEvaluator::ZoneResult zoneResult = evaluator.evaluateZone(zone, prepared);
        QCOMPARE(zoneResult.zone, zone);
        QCOMPARE(zoneResult.crossings.count(), 1);
        issues.append(evaluator.scopeIssue(zoneResult, authorisation, prepared));
    }
    QCOMPARE(lookups, prepareLookups);

    // With the results of the evaluations which prepare the paths themselves, each time
    const GeoZonePathEvaluator::Result unprepared = evaluator.evaluate(paths);
    QCOMPARE(lookups, 2 * prepareLookups);
    QCOMPARE(unprepared.zones.count(), result.zones.count());
    for (qsizetype i = 0; i < result.zones.count(); i++) {
        QCOMPARE(unprepared.zones[i].zone, result.zones[i].zone);
        QCOMPARE(unprepared.zones[i].crossings.count(), result.zones[i].crossings.count());
        QCOMPARE(unprepared.zones[i].crossings.constFirst().entry.distance, result.zones[i].crossings.constFirst().entry.distance);
        QCOMPARE(unprepared.zones[i].crossings.constFirst().entry.position.terrainAMSL, 400.0);
        QCOMPARE(evaluator.scopeIssue(evaluator.evaluateZone(result.zones[i].zone, paths), authorisation, paths), issues[i]);
    }
    QCOMPARE(issues.constLast(), QStringLiteral("authorised up to 120 m AMSL, the flight reaches 500 m AMSL"));

    // Without paths nothing is entered
    QVERIFY(evaluator.evaluate(evaluator.prepare({})).zones.isEmpty());
    QVERIFY(!evaluator.evaluateZone(controlZone, evaluator.prepare({})).zone);
}

void GeoZoneEvaluatorTest::_testUnknownTerrain()
{
    // From 50 to 150 m above the ground, which is at 400 m except in a gap without terrain data around the zone center
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _withLimits(_circleFeature(QStringLiteral("AGL"), QStringLiteral("PROHIBITED"), center, 300), 50, QStringLiteral("AGL"), 150, QStringLiteral("AGL")),
    }), errorString), qPrintable(errorString));
    GeoZonePathEvaluator evaluator(zones.catalog(), GeoZonePolicy({ GeoZone::Prohibited }));
    evaluator.setTerrainLookup([center](const QGeoCoordinate &coordinate) { return (center.distanceTo(coordinate) < 150) ? qQNaN() : 400.0; });
    const QGeoCoordinate west = center.atDistanceAndAzimuth(1000, 270);
    const QGeoCoordinate east = center.atDistanceAndAzimuth(1000, 90);

    // 200 m above the known ground, over the ceiling. The gap isn't bridged with the terrain on its sides, so the
    // ceiling is unknown from the last position with terrain before it to the first after it, which enter the zone.
    const GeoZonePathEvaluator::Result across = evaluator.evaluate({ _evaluatorPath({ west, east }, { 600, 600 }) });
    QCOMPARE(across.activeEnteredZones().count(), 1);
    const GeoZonePathEvaluator::ZoneResult &zone = across.zones[0];
    QCOMPARE(zone.crossings.count(), 1);
    QVERIFY(_near(zone.crossings[0].entry.distance, 800));
    QVERIFY(_near(zone.crossings[0].exit.distance, 1200));
    QVERIFY(zone.approximations.testFlag(GeoZonePathEvaluator::TerrainUnknown));
    QCOMPARE(zone.stretches.count(), 1);
    QCOMPARE(zone.stretches[0].parts.count(), 3);
    QCOMPARE(zone.stretches[0].parts[0].position, GeoZone::VerticalPosition::Above);
    QVERIFY(across.approximations.testFlag(GeoZonePathEvaluator::TerrainUnknown));

    // A vertex in the gap doesn't take the terrain of the previous position
    QCOMPARE(evaluator.evaluate({ _evaluatorPath({ west, center }, { 600, 600 }) }).activeEnteredZones().count(), 1);

    // With the terrain known along the path the zone is below it, and the result is exact
    evaluator.setTerrainLookup([](const QGeoCoordinate &) { return 400.0; });
    const GeoZonePathEvaluator::Result known = evaluator.evaluate({ _evaluatorPath({ west, east }, { 600, 600 }) });
    QCOMPARE(known.zones.count(), 1);
    QVERIFY(!known.zones[0].entered());
    QCOMPARE(known.approximations, GeoZonePathEvaluator::Approximations());
}

void GeoZoneEvaluatorTest::_testGreatCircles()
{
    // A leg along the 60th parallel over one degree of longitude, whose great circle passes about 105 m north of the
    // parallel halfway, with a small zone on each, and a zone 50 to 150 m above the ground near its start
    const QPointF west(0, 60);
    const QPointF east(1, 60);
    const QGeoCoordinate start = GeoZone::toCoordinate(west);
    const QGeoCoordinate end = GeoZone::toCoordinate(east);
    const QGeoCoordinate onGreatCircle = GeoZone::toCoordinate(GeoZoneGeometry::greatCircleInterpolate(west, east, 0.5));
    const QGeoCoordinate onParallel(60, 0.5);
    const QGeoCoordinate groundCenter = start.atDistanceAndAzimuth(2000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("CIRCLE"), QStringLiteral("PROHIBITED"), onGreatCircle, 30),
        _circleFeature(QStringLiteral("PARALLEL"), QStringLiteral("PROHIBITED"), onParallel, 30),
        _withLimits(_circleFeature(QStringLiteral("AGL"), QStringLiteral("PROHIBITED"), groundCenter, 300), 50, QStringLiteral("AGL"), 150, QStringLiteral("AGL")),
    }), errorString), qPrintable(errorString));
    GeoZonePathEvaluator evaluator(zones.catalog(), GeoZonePolicy({ GeoZone::Prohibited }));

    // The vehicle flies the great circle, entering the zone on it about halfway and passing the one on the parallel.
    // Without terrain the zone above ground counts as entered.
    const GeoZonePathEvaluator::Result result = evaluator.evaluate({ _evaluatorPath({ start, end }, { 600, 600 }) });
    QStringList entered;
    for (const GeoZone *zone : result.activeEnteredZones()) {
        entered.append(zone->identifier());
    }
    QCOMPARE(entered, QStringList({ QStringLiteral("AGL"), QStringLiteral("CIRCLE") }));
    const GeoZonePathEvaluator::Crossing &crossing = result.zones[1].crossings[0];
    QCOMPARE(crossing.entry.segment, 0);
    QVERIFY(_near(crossing.entry.fraction, 0.4995, 0.0005));
    QVERIFY(_near((crossing.entry.distance + crossing.exit.distance) / 2, start.distanceTo(end) / 2, 2));
    QVERIFY(onGreatCircle.distanceTo(crossing.entry.position.coordinate) < 35);
    QCOMPARE(result.zones[1].approximations, GeoZonePathEvaluator::Approximations());
    QVERIFY(!result.approximations.testFlag(GeoZonePathEvaluator::GreatCircleDeviation));

    // The terrain is sampled at the spacing up to the maximum positions, farther apart beyond, which is approximate
    // for limits above ground
    const auto groundZone = std::find_if(zones.zoneList().cbegin(), zones.zoneList().cend(), [](const GeoZone *zone) { return zone->identifier() == QStringLiteral("AGL"); });
    QVERIFY(groundZone != zones.zoneList().cend());
    evaluator.setTerrainLookup([](const QGeoCoordinate &) { return 400.0; }, 100, 1000);
    const GeoZonePathEvaluator::Result sampled = evaluator.evaluate({ _evaluatorPath({ start, end }, { 600, 600 }) });
    const GeoZonePathEvaluator::ZoneResult *const ground = sampled.find(*groundZone);
    QVERIFY(ground);
    QVERIFY(!ground->entered());
    QCOMPARE(sampled.approximations, GeoZonePathEvaluator::Approximations());
    evaluator.setTerrainLookup([](const QGeoCoordinate &) { return 400.0; }, 100, 100);
    const GeoZonePathEvaluator::Result coarse = evaluator.evaluate({ _evaluatorPath({ start, end }, { 600, 600 }) });
    QVERIFY(coarse.find(*groundZone)->approximations.testFlag(GeoZonePathEvaluator::TerrainCoarse));
    QVERIFY(coarse.approximations.testFlag(GeoZonePathEvaluator::TerrainCoarse));
    // Only zones with limits above ground are affected
    QCOMPARE(coarse.find(result.zones[1].zone)->approximations, GeoZonePathEvaluator::Approximations());
}

void GeoZoneEvaluatorTest::_testApproximations()
{
    // East through a zone above a flight level, one with limits in an unknown reference, and one active from an unknown
    // daylight event to sunset
    const QGeoCoordinate start(47, 8);
    QJsonObject scheduled = _circleFeature(QStringLiteral("SCHEDULED"), QStringLiteral("PROHIBITED"), start.atDistanceAndAzimuth(3000, 90), 200);
    scheduled[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "schedule", QJsonArray{ QJsonObject{
        { "day", QJsonArray{ "ANY" } }, { "startEvent", "MOONRISE" }, { "endEvent", "SS" } } } } } };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _withLimits(_circleFeature(QStringLiteral("LEVEL"), QStringLiteral("PROHIBITED"), start.atDistanceAndAzimuth(1000, 90), 200), 0, QStringLiteral("STD"), 3000, QStringLiteral("STD")),
        _withLimits(_circleFeature(QStringLiteral("UNKNOWN"), QStringLiteral("PROHIBITED"), start.atDistanceAndAzimuth(2000, 90), 200), 100, QStringLiteral("XYZ"), 200, QStringLiteral("XYZ")),
        scheduled,
    }), errorString), qPrintable(errorString));
    const auto zoneNamed = [&zones](const QString &identifier) {
        const auto it = std::find_if(zones.zoneList().cbegin(), zones.zoneList().cend(), [&identifier](const GeoZone *zone) { return zone->identifier() == identifier; });
        return (it == zones.zoneList().cend()) ? nullptr : *it;
    };
    QVERIFY(zoneNamed(QStringLiteral("SCHEDULED"))->scheduleAssumed());
    QVERIFY(!zoneNamed(QStringLiteral("LEVEL"))->scheduleAssumed());

    const QDateTime flightStart = _utc(QDate(2027, 6, 1), QTime(10, 0));
    const QDateTime flightEnd = flightStart.addSecs(600);
    const GeoZonePathEvaluator evaluator(zones.catalog(), GeoZonePolicy({ GeoZone::Prohibited }).activeDuring(flightStart, flightEnd), flightStart);
    GeoZonePathEvaluator::Path path = _evaluatorPath({ start, start.atDistanceAndAzimuth(4000, 90) }, { 500, 500 });
    path.setTimes(flightStart, flightEnd);
    const GeoZonePathEvaluator::Result result = evaluator.evaluate({ path });
    QCOMPARE(result.activeEnteredZones().count(), 3);
    QCOMPARE(result.find(zoneNamed(QStringLiteral("LEVEL")))->approximations, GeoZonePathEvaluator::Approximations(GeoZonePathEvaluator::FlightLevel));
    QCOMPARE(result.find(zoneNamed(QStringLiteral("UNKNOWN")))->approximations, GeoZonePathEvaluator::Approximations(GeoZonePathEvaluator::AltitudeReferenceUnknown));
    QCOMPARE(result.find(zoneNamed(QStringLiteral("SCHEDULED")))->approximations, GeoZonePathEvaluator::Approximations(GeoZonePathEvaluator::ScheduleAssumed));
    QCOMPARE(result.approximations, GeoZonePathEvaluator::FlightLevel | GeoZonePathEvaluator::AltitudeReferenceUnknown | GeoZonePathEvaluator::ScheduleAssumed);
    // Whatever the schedule, it isn't assumed
    QCOMPARE(GeoZonePathEvaluator(zones.catalog(), GeoZonePolicy({ GeoZone::Prohibited })).evaluate({ path }).find(zoneNamed(QStringLiteral("SCHEDULED")))->approximations,
             GeoZonePathEvaluator::Approximations());

    // Relative altitudes without their reference are unknown
    GeoZonePathEvaluator::Path relative = _evaluatorPath({ start, start.atDistanceAndAzimuth(1500, 90) }, { 100, 100 });
    relative.altitudeReference = GeoZonePathEvaluator::AltitudeReference::Relative;
    QVERIFY(evaluator.evaluate({ relative }).zones[0].approximations.testFlag(GeoZonePathEvaluator::AltitudeReferenceUnknown));

    // Keys for exports and texts for the pilot
    QCOMPARE(GeoZonePathEvaluator::approximationTexts(result.approximations, true),
             QStringList({ QStringLiteral("flight_level"), QStringLiteral("schedule_assumed"), QStringLiteral("altitude_reference_unknown") }));
    QCOMPARE(GeoZonePathEvaluator::approximationTexts(GeoZonePathEvaluator::TerrainUnknown | GeoZonePathEvaluator::TerrainCoarse).count(), 2);
    QVERIFY(GeoZonePathEvaluator::approximationTexts(GeoZonePathEvaluator::Approximations(), true).isEmpty());

    // The briefing has them per zone and overall, see GeoZonePlanningTest::_testApproximationReports for its
    // documents and the mission check
    GeoZoneBriefing::Input input;
    input.mission.name = QStringLiteral("Approximate");
    input.paths = { path };
    input.start = flightStart;
    input.end = flightEnd;
    const GeoZoneBriefing briefing = GeoZoneBriefing::build(zones.catalog(), input);
    QCOMPARE(briefing.approximations, result.approximations);
    const QList<const GeoZoneBriefing::Zone*> entered = briefing.zonesWith(GeoZoneBriefing::Involvement::Entered);
    QCOMPARE(entered.count(), 3);
    QCOMPARE(entered[0]->identifier, QStringLiteral("LEVEL"));
    QCOMPARE(entered[0]->approximations, GeoZonePathEvaluator::Approximations(GeoZonePathEvaluator::FlightLevel));
}

void GeoZoneEvaluatorTest::_testSchedules()
{
    // Active on 1 January 2027 from 10:00 to 12:00 UTC, 1900 to 2100 m along a path of 4000 m
    const QGeoCoordinate start(47, 8);
    QJsonObject temporary = _circleFeature(QStringLiteral("T"), QStringLiteral("PROHIBITED"), start.atDistanceAndAzimuth(2000, 90), 100);
    temporary[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-01-01T10:00:00Z" }, { "endDateTime", "2027-01-01T12:00:00Z" } } };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ temporary }), errorString), qPrintable(errorString));
    const QDate day(2027, 1, 1);
    GeoZonePathEvaluator::Path path = _evaluatorPath({ start, start.atDistanceAndAzimuth(4000, 90) });

    // A minute per 100 m, from the start time
    const auto evaluateFrom = [&](const QTime &time) {
        const QDateTime flightStart = _utc(day, time);
        const QDateTime flightEnd = flightStart.addSecs(40 * 60);
        path.setTimes(flightStart, flightEnd);
        return GeoZonePathEvaluator(zones.catalog(), GeoZonePolicy::mission(flightStart, flightEnd), flightStart).evaluate({ path });
    };

    // Entering after the zone becomes active
    GeoZonePathEvaluator::Result result = evaluateFrom(QTime(10, 0));
    QCOMPARE(result.zones.count(), 1);
    const GeoZonePathEvaluator::Crossing *crossing = result.zones[0].firstCrossing();
    QVERIFY(crossing);
    QVERIFY(qAbs(crossing->entry.time.secsTo(_utc(day, QTime(10, 19)))) <= 2);
    QVERIFY(crossing->active);
    QCOMPARE(result.activeEnteredZones().count(), 1);

    // Entering just before and leaving after it becomes active
    result = evaluateFrom(QTime(9, 40));
    QVERIFY(qAbs(result.zones[0].crossings[0].entry.time.secsTo(_utc(day, QTime(9, 59)))) <= 2);
    QVERIFY(result.zones[0].crossings[0].active);

    // Leaving 10 minutes before it becomes active counts, since the take-off or the pace may differ from the estimate
    QCOMPARE(GeoZonePathEvaluator::defaultScheduleToleranceSecs, 15 * 60);
    result = evaluateFrom(QTime(9, 29));
    QVERIFY(qAbs(result.zones[0].crossings[0].exit.time.secsTo(_utc(day, QTime(9, 50)))) <= 2);
    QVERIFY(result.zones[0].crossings[0].active);
    QCOMPARE(result.activeEnteredZones().count(), 1);
    // Like entering 10 minutes after it ends
    result = evaluateFrom(QTime(11, 51));
    QVERIFY(qAbs(result.zones[0].crossings[0].entry.time.secsTo(_utc(day, QTime(12, 10)))) <= 2);
    QVERIFY(result.zones[0].crossings[0].active);

    // Leaving 30 minutes before it becomes active doesn't, although it is active during the flight
    result = evaluateFrom(QTime(9, 9));
    QCOMPARE(result.zones.count(), 1);
    QVERIFY(result.zones[0].entered());
    QVERIFY(!result.zones[0].active());
    QVERIFY(!result.zones[0].crossings[0].active);
    QVERIFY(qAbs(result.zones[0].crossings[0].exit.time.secsTo(_utc(day, QTime(9, 30)))) <= 2);
    QVERIFY(result.activeEnteredZones().isEmpty());

    // Without times the zone applies when active at some time of the flight, or whatever its schedule without one
    path.setTimes(QDateTime(), QDateTime());
    QVERIFY(GeoZonePathEvaluator(zones.catalog(), GeoZonePolicy::mission(_utc(day, QTime(9, 30)), _utc(day, QTime(10, 30))))
                .evaluate({ path }).zones[0].active());
    QVERIFY(!GeoZonePathEvaluator(zones.catalog(), GeoZonePolicy::mission(_utc(day, QTime(8, 0)), _utc(day, QTime(9, 0))))
                .evaluate({ path }).zones[0].active());
    QVERIFY(GeoZonePathEvaluator(zones.catalog(), GeoZonePolicy::mission(QDateTime(), QDateTime())).evaluate({ path }).zones[0].active());
    // Zones active now are those active at the evaluation time
    QVERIFY(GeoZonePathEvaluator(zones.catalog(), GeoZonePolicy::alerts(), _utc(day, QTime(11, 0))).evaluate({ path }).zones[0].active());
    QVERIFY(!GeoZonePathEvaluator(zones.catalog(), GeoZonePolicy::alerts(), _utc(day, QTime(13, 0))).evaluate({ path }).zones[0].active());
}

void GeoZoneEvaluatorTest::_testAntimeridianQueries()
{
    // A zone from 179.9 E to 179.9 W, one on the other side of the world and one along the equator from 0 to 160 W,
    // spanning more than 180 degrees of longitude
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), { QGeoCoordinate(-16.6, 179.9), QGeoCoordinate(-16.4, 179.9),
                                                                             QGeoCoordinate(-16.4, -179.9), QGeoCoordinate(-16.6, -179.9) }),
        _circleFeature(QStringLiteral("FAR"), QStringLiteral("PROHIBITED"), QGeoCoordinate(-16.5, 0), 5000),
        _polygonFeature(QStringLiteral("WIDE"), QStringLiteral("PROHIBITED"), { QGeoCoordinate(-1, 0), QGeoCoordinate(-1, 50), QGeoCoordinate(-1, 100),
                                                                                QGeoCoordinate(-1, 150), QGeoCoordinate(-1, -160), QGeoCoordinate(1, -160),
                                                                                QGeoCoordinate(1, 150), QGeoCoordinate(1, 100), QGeoCoordinate(1, 50),
                                                                                QGeoCoordinate(1, 0) }),
    }), errorString), qPrintable(errorString));
    const GeoZoneCatalog &catalog = zones.catalog();
    const GeoZonePolicy policy({ GeoZone::Prohibited });
    const auto identifiers = [](const QList<const GeoZone*> &zones) {
        QStringList result;
        for (const GeoZone *zone : zones) {
            result.append(zone->identifier());
        }
        return result;
    };

    // Points on both sides of the antimeridian
    QCOMPARE(identifiers(catalog.zonesAt(QGeoCoordinate(-16.5, 179.95))), QStringList({ QStringLiteral("X") }));
    QCOMPARE(identifiers(catalog.zonesAt(QGeoCoordinate(-16.5, -179.95))), QStringList({ QStringLiteral("X") }));
    QCOMPARE(identifiers(catalog.zonesAt(QGeoCoordinate(-16.5, 180))), QStringList({ QStringLiteral("X") }));
    QVERIFY(catalog.zonesAt(QGeoCoordinate(-16.5, 179.8)).isEmpty());

    // Areas across the antimeridian, and the rest of the world between the same longitudes
    QCOMPARE(identifiers(catalog.zonesInArea(GeoZoneBox(-17, 179.8, -16, -179.8), policy)), QStringList({ QStringLiteral("X") }));
    QCOMPARE(identifiers(catalog.zonesInArea(QGeoRectangle(QGeoCoordinate(-16, 179.8), QGeoCoordinate(-17, -179.8)), policy)), QStringList({ QStringLiteral("X") }));
    QCOMPARE(identifiers(catalog.zonesInArea(GeoZoneBox(-17, -179.8, -16, 179.8), policy)), QStringList({ QStringLiteral("FAR") }));

    // Survey areas across the antimeridian and on the other side of the world
    const GeoZone::Area area{ { QGeoCoordinate(-16.6, 179.8), QGeoCoordinate(-16.4, 179.8), QGeoCoordinate(-16.4, -179.8), QGeoCoordinate(-16.6, -179.8) }, {}, {} };
    QCOMPARE(identifiers(catalog.zonesIntersectingAreas({ area }, policy)), QStringList({ QStringLiteral("X") }));
    const GeoZone::Area farArea{ { QGeoCoordinate(-16.6, -0.1), QGeoCoordinate(-16.4, -0.1), QGeoCoordinate(-16.4, 0.1), QGeoCoordinate(-16.6, 0.1) }, {}, {} };
    QCOMPARE(identifiers(catalog.zonesIntersectingAreas({ farArea }, policy)), QStringList({ QStringLiteral("FAR") }));

    // A mission path across the antimeridian enters the zone at its west edge
    const QGeoCoordinate from(-16.5, 179.8);
    const QGeoCoordinate to(-16.5, -179.8);
    const double entryDistance = from.distanceTo(QGeoCoordinate(-16.5, 179.9));
    const QList<GeoZonePathEvaluator::ZoneResult> hits = GeoZonePathEvaluator(catalog, policy).evaluate({ GeoZonePathEvaluator::Path::fromPositions({ { from }, { to } }) }).zones;
    QCOMPARE(hits.count(), 1);
    QCOMPARE(hits[0].zone->identifier(), QStringLiteral("X"));
    QVERIFY(qAbs(hits[0].crossings.constFirst().entry.distance - entryDistance) < (entryDistance * 0.01));

    // Vehicles past the antimeridian more than 180 degrees from the west end of the wide zone are alerted inside it
    // and near its east end
    const GeoZoneAlertTracker::Limits limits{ 100, 100, 0 };
    GeoZoneAlertTracker::Update update = GeoZoneAlertTracker().update(catalog, policy, GeoZone::Position{ QGeoCoordinate(0, -170) }, {}, limits);
    QCOMPARE(update.alerts.count(), 1);
    QCOMPARE(update.alerts[0].zone->identifier(), QStringLiteral("WIDE"));
    QVERIFY(update.alerts[0].inside);
    const QGeoCoordinate nearEastEnd(0, -159.9995);
    update = GeoZoneAlertTracker().update(catalog, policy, GeoZone::Position{ nearEastEnd }, {}, limits);
    QCOMPARE(update.alerts.count(), 1);
    QCOMPARE(update.alerts[0].zone->identifier(), QStringLiteral("WIDE"));
    QVERIFY(!update.alerts[0].inside);
    QVERIFY(qAbs(update.alerts[0].distance - nearEastEnd.distanceTo(QGeoCoordinate(0, -160))) < 1);
}

void GeoZoneEvaluatorTest::_testAlertThresholds()
{
    using Thresholds = GeoZoneAlertThresholds;

    // The manufacturer's profiles are compiled in, the classes complete theirs from the default class
    const QHash<QString, Thresholds::Profile> &profiles = Thresholds::profiles();
    QCOMPARE(QSet<QString>(profiles.keyBegin(), profiles.keyEnd()), QSet<QString>({ QStringLiteral("default"), QStringLiteral("multirotor"), QStringLiteral("fixedwing"), QStringLiteral("vtol") }));
    const Thresholds::Profile multirotor = Thresholds::profile(QStringLiteral("multirotor"));
    QCOMPARE(multirotor.name, QStringLiteral("Multirotor"));
    QCOMPARE(multirotor.deceleration, 2.5);
    QCOMPARE(multirotor.positionErrorFactor, 2.0);
    QCOMPARE(multirotor.pilotReactionTime, 5.0);
    QCOMPARE(multirotor.automaticReactionTime, 3.0);
    // The fence warning and the GNSS quality of geo-awareness are the manufacturer's too
    QCOMPARE(multirotor.fenceWarningMargin, 3.0);
    QCOMPARE(multirotor.minSatellites, 6.0);
    QCOMPARE(multirotor.maxHdop, 5.0);
    QCOMPARE(multirotor.maxHorizontalAccuracy, 5.0);
    QVERIFY(Thresholds::profile(QStringLiteral("fixedwing")).manoeuvre == Thresholds::Manoeuvre::Turn);
    QCOMPARE(Thresholds::profile(QStringLiteral("rover")).name, QStringLiteral("Generic vehicle"));

    // The default profile at 15 m/s without a known accuracy: 2 x 10 m of position error, 5 s of reaction and 1 s
    // between evaluations, braking at 2 m/s² over 56 m in 7.5 s, 10 m/s of wind over 12.5 s and 20 m of margin
    const Thresholds::Profile generic = Thresholds::profile(QStringLiteral("default"));
    Thresholds::Input input;
    input.velocity = GeoZoneAlertTracker::Velocity{ 15, 0, 0, true };
    Thresholds::Result result = Thresholds::compute(generic, input);
    QVERIFY(_near(result.positionError, 20, 0.01));
    QVERIFY(_near(result.manoeuvreSpan, 56.25, 0.01));
    QVERIFY(_near(result.manoeuvreTime, 7.5, 0.01));
    QVERIFY(_near(result.windDrift, 125, 0.01));
    QVERIFY(_near(result.distance, 20 + 90 + 56.25 + 125 + 20, 0.01));
    QVERIFY(_near(result.verticalDistance, 30 + 5 + 10, 0.01));
    QVERIFY(_near(result.time, 5 + 7.5 + 1 + (165.0 / 15), 0.01));
    QCOMPARE(result.limits().distance, result.distance);

    // Hovering, the minimum distances apply
    input.velocity = GeoZoneAlertTracker::Velocity{ 0, 0, 0, true };
    result = Thresholds::compute(generic, input);
    QCOMPARE(result.distance, generic.minDistance);
    QVERIFY(_near(result.verticalDistance, 45, 0.01));

    // Fixed wing aircraft turn away at 30 degrees of bank: a turn radius of 71 m at 20 m/s, a quarter turn in 5.5 s
    input.velocity = GeoZoneAlertTracker::Velocity{ 0, 20, 0, true };
    result = Thresholds::compute(Thresholds::profile(QStringLiteral("fixedwing")), input);
    QVERIFY(_near(result.manoeuvreSpan, 70.6, 0.1));
    QVERIFY(_near(result.manoeuvreTime, 5.55, 0.01));

    // Poor GNSS widens the thresholds: the accuracy the vehicle reports, else its HDOP times the range error
    input.velocity = GeoZoneAlertTracker::Velocity{ 15, 0, 0, true };
    input.horizontalAccuracy = 1;
    const Thresholds::Result accurate = Thresholds::compute(multirotor, input);
    QVERIFY(_near(accurate.positionError, 2, 0.01));
    QVERIFY(_near(accurate.verticalPositionError, 3, 0.01));
    input.horizontalAccuracy = qQNaN();
    input.hdop = 5;
    const Thresholds::Result poor = Thresholds::compute(multirotor, input);
    QVERIFY(_near(poor.positionError, 50, 0.01));
    QVERIFY(_near(poor.distance - accurate.distance, 48, 0.01));
    QVERIFY(poor.verticalDistance > accurate.verticalDistance);
    QVERIFY(poor.time > accurate.time);
    input.verticalAccuracy = 4;
    QVERIFY(_near(Thresholds::compute(multirotor, input).verticalPositionError, 8, 0.01));

    // An automatic reaction is quicker
    const Thresholds::Result automatic = Thresholds::compute(multirotor, input, {}, Thresholds::Reaction::Automatic);
    QCOMPARE(automatic.reactionTime, 3.0);
    QVERIFY(automatic.distance < poor.distance);

    // The pilot can only add to the manufacturer's thresholds
    const Thresholds::Result extra = Thresholds::compute(multirotor, input, Thresholds::Extras{ 10, 50 });
    QVERIFY(_near(extra.time - poor.time, 10, 0.01));
    QVERIFY(_near(extra.distance - poor.distance, 50 + 150, 0.01));
    const Thresholds::Result reduced = Thresholds::compute(multirotor, input, Thresholds::Extras{ -10, -500 });
    QCOMPARE(reduced.distance, poor.distance);
    QCOMPARE(reduced.time, poor.time);
    input.velocity = GeoZoneAlertTracker::Velocity();
    QCOMPARE(Thresholds::compute(multirotor, input, Thresholds::Extras{ -10, -500 }).distance, multirotor.minDistance);

    // Profiles of custom builds: the classes start from the default class, invalid values reject the file
    QString errorString;
    const QHash<QString, Thresholds::Profile> custom = Thresholds::parseProfiles(R"({ "fileType": "GeoZoneAlertProfiles", "profiles": {
        "default": { "name": "Ours", "pilotReactionTime": 6, "minDistance": 200, "maxHdop": 3, "fenceWarningMargin": 4 },
        "multirotor": { "deceleration": 4, "manoeuvre": "brake" } } })", &errorString);
    QVERIFY2(errorString.isEmpty(), qPrintable(errorString));
    QCOMPARE(custom.count(), 2);
    QCOMPARE(custom[QStringLiteral("multirotor")].pilotReactionTime, 6.0);
    QCOMPARE(custom[QStringLiteral("multirotor")].minDistance, 200.0);
    QCOMPARE(custom[QStringLiteral("multirotor")].deceleration, 4.0);
    QCOMPARE(custom[QStringLiteral("multirotor")].maxHdop, 3.0);
    QCOMPARE(custom[QStringLiteral("multirotor")].fenceWarningMargin, 4.0);
    QVERIFY(Thresholds::parseProfiles(R"({ "fileType": "GeoZoneAlertProfiles", "profiles": { "default": { "maxHdop": 0 } } })").isEmpty());
    QVERIFY(Thresholds::parseProfiles(R"({ "fileType": "GeoZoneAlertProfiles", "profiles": { "multirotor": { "deceleration": 0 } } })", &errorString).isEmpty());
    QVERIFY(errorString.contains(QStringLiteral("deceleration")));
    QVERIFY(Thresholds::parseProfiles(R"({ "fileType": "GeoZoneAlertProfiles", "profiles": { "default": { "safetyMargin": -1 } } })").isEmpty());
    QVERIFY(Thresholds::parseProfiles(R"({ "profiles": {} })").isEmpty());
}

void GeoZoneEvaluatorTest::_testThresholdWarningTimes()
{
    // A zone 300 m around the center, one from 500 m AMSL above it and one up to 300 m AMSL below it
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate high = center.atDistanceAndAzimuth(20000, 90);
    const QGeoCoordinate low = center.atDistanceAndAzimuth(20000, 270);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _withLimits(_circleFeature(QStringLiteral("HIGH"), QStringLiteral("PROHIBITED"), high, 1000), 500, QStringLiteral("AMSL"), 2000, QStringLiteral("AMSL")),
        _withLimits(_circleFeature(QStringLiteral("LOW"), QStringLiteral("PROHIBITED"), low, 1000), 0, QStringLiteral("AMSL"), 300, QStringLiteral("AMSL")),
    }), errorString), qPrintable(errorString));
    const GeoZonePolicy policy = GeoZonePolicy::alerts();
    const GeoZoneAlertThresholds::Profile multirotor = GeoZoneAlertThresholds::profile(QStringLiteral("multirotor"));

    // Flies straight at constant speed, evaluated every second with the thresholds at its speed, from a start offset
    // within the second, and returns the seconds from the approach alert to the entry, and the alert's thresholds
    struct Warning {
        double seconds = -1;
        GeoZoneAlertThresholds::Result thresholds;
    };
    const auto fly = [&](const std::function<GeoZone::Position(double)> &at, double entryTime, const GeoZoneAlertTracker::Velocity &velocity,
                         GeoZoneAlertThresholds::Reaction reaction, double accuracy) {
        Warning warning;
        GeoZoneAlertTracker tracker;
        GeoZoneAlertThresholds::Input input{ velocity, accuracy };
        for (double time = 0; time < entryTime; time += GeoZoneAlertThresholds::evaluationInterval) {
            const GeoZoneAlertThresholds::Result thresholds = GeoZoneAlertThresholds::compute(multirotor, input, {}, reaction);
            const GeoZoneAlertTracker::Update update = tracker.update(zones.catalog(), policy, at(time), velocity, thresholds.limits());
            if (!update.alerts.isEmpty()) {
                if (update.alerts.constFirst().inside) {
                    break;
                }
                warning = Warning{ entryTime - time, thresholds };
                break;
            }
        }
        return warning;
    };

    using Reaction = GeoZoneAlertThresholds::Reaction;
    for (const Reaction reaction : { Reaction::Pilot, Reaction::Automatic }) {
        for (const double offset : { 0.0, 0.5 }) {
            // Horizontally towards the zone from 3 km
            for (const double speed : { 5.0, 15.0, 50.0 }) {
                const double entryTime = (3000 + (offset * speed)) / speed;
                const Warning warning = fly([&](double time) {
                    return GeoZone::Position{ center.atDistanceAndAzimuth(300 + 3000 + (offset * speed) - (speed * time), 180) };
                }, entryTime, { speed, 0, 0, true }, reaction, qQNaN());
                const QString text = QStringLiteral("%1 m/s: %2 s of warning, %3 s needed").arg(speed).arg(warning.seconds).arg(warning.thresholds.minWarningTime());
                QVERIFY2(warning.seconds >= (warning.thresholds.reactionTime + warning.thresholds.manoeuvreTime), qPrintable(text));
                QVERIFY2(warning.seconds >= (warning.thresholds.time - GeoZoneAlertThresholds::evaluationInterval), qPrintable(text));
                QCOMPARE(warning.thresholds.reactionTime, (reaction == Reaction::Pilot) ? 5.0 : 3.0);

                // Poor GNSS warns earlier
                const Warning poor = fly([&](double time) {
                    return GeoZone::Position{ center.atDistanceAndAzimuth(300 + 3000 + (offset * speed) - (speed * time), 180) };
                }, entryTime, { speed, 0, 0, true }, reaction, 100);
                QVERIFY2(poor.seconds > warning.seconds, qPrintable(QStringLiteral("%1 m/s: %2 s with poor GNSS, %3 s").arg(speed).arg(poor.seconds).arg(warning.seconds)));
            }

            // Climbing into the floor of the zone above, and descending into the zone below, from 400 m
            for (const double rate : { 2.0, 5.0, 10.0 }) {
                const double entryTime = (400 + (offset * rate)) / rate;
                const Warning climb = fly([&](double time) {
                    return GeoZone::Position{ high, 100 - (offset * rate) + (rate * time) };
                }, entryTime, { 0, 0, rate, true }, reaction, qQNaN());
                QVERIFY2(climb.seconds >= (climb.thresholds.reactionTime + climb.thresholds.manoeuvreTime),
                         qPrintable(QStringLiteral("Climbing at %1 m/s: %2 s").arg(rate).arg(climb.seconds)));
                const Warning descent = fly([&](double time) {
                    return GeoZone::Position{ low, 700 + (offset * rate) - (rate * time) };
                }, entryTime, { 0, 0, -rate, true }, reaction, qQNaN());
                QVERIFY2(descent.seconds >= (descent.thresholds.reactionTime + descent.thresholds.manoeuvreTime),
                         qPrintable(QStringLiteral("Descending at %1 m/s: %2 s").arg(rate).arg(descent.seconds)));
                QVERIFY(_near(descent.thresholds.manoeuvreTime, rate / multirotor.verticalDeceleration, 0.01));
            }
        }
    }
}

void GeoZoneEvaluatorTest::_testActivationTracker()
{
    // Every day from 08:00 to 09:00 UTC
    const QGeoCoordinate center(47, 8);
    QJsonObject daily = _circleFeature(QStringLiteral("D"), QStringLiteral("REQ_AUTHORISATION"), center, 300);
    daily[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-01-01T00:00:00Z" }, { "schedule", QJsonArray{
        QJsonObject{ { "day", QJsonArray{ "ANY" } }, { "startTime", "08:00:00Z" }, { "endTime", "09:00:00Z" } } } } } };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ daily }), errorString), qPrintable(errorString));
    GeoZone *const zone = zones.zoneList().constFirst();
    const QDate day(2027, 5, 3);
    const GeoZonePolicy policy = GeoZonePolicy::alerts().anyTime();
    const GeoZoneAlertTracker::Limits limits{ 100, 30, 0, 0 };
    const GeoZone::Position inside{ center };

    GeoZoneActivationTracker tracker;
    const auto update = [&](const QTime &time) {
        const QDateTime now = _utc(day, time);
        (void) zone->updateActive(now);
        return tracker.update(zones.catalog(), policy, inside, limits, now, 600);
    };
    QVERIFY(zone->isActiveAt(_utc(day, QTime(8, 30))));
    QVERIFY(!zone->isActiveAt(_utc(day, QTime(7, 30))));
    QVERIFY(update(QTime(7, 45)).isEmpty());
    const QList<GeoZoneActivationTracker::Alert> alerts = update(QTime(7, 52, 10));
    QCOMPARE(alerts.count(), 1);
    QCOMPARE(alerts[0].zone, zone);
    QVERIFY(alerts[0].inside);
    QCOMPARE(alerts[0].activation, _utc(day, QTime(8, 0)));
    QVERIFY(update(QTime(7, 55)).isEmpty());
    QVERIFY(update(QTime(8, 30)).isEmpty());

    // The next day's activation alerts again
    const QDateTime nextDay = _utc(day.addDays(1), QTime(7, 51));
    (void) zone->updateActive(nextDay);
    QCOMPARE(tracker.update(zones.catalog(), policy, inside, limits, nextDay, 600).count(), 1);

    // Out of the approach distance nothing, the zone needn't be active to be found
    tracker.reset();
    QVERIFY(tracker.update(zones.catalog(), policy, GeoZone::Position{ center.atDistanceAndAzimuth(500, 0) }, limits, nextDay, 600).isEmpty());
    QCOMPARE(tracker.update(zones.catalog(), policy, GeoZone::Position{ center.atDistanceAndAzimuth(350, 0) }, limits, nextDay, 600).count(), 1);
}

GEOZONE_TEST(GeoZoneEvaluatorTest)
