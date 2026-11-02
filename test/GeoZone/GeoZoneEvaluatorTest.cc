/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneEvaluatorTest.h"
#include "GeoZoneAlertTracker.h"
#include "GeoZoneBriefing.h"
#include "GeoZoneTestHelpers.h"

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

