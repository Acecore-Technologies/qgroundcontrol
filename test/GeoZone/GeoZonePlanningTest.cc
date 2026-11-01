/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePlanningTest.h"
#include "GeoZoneBriefing.h"
#include "GeoZoneBriefingExport.h"
#include "GeoZoneFenceTags.h"
#include "GeoZonePlanAudit.h"
#include "GeoZoneRouteSearch.h"
#include "GeoZoneSourceRepository.h"
#include "GeoZoneSourceVersion.h"
#include "GeoZoneTestHelpers.h"
#include "GeoFenceManager.h"
#include "MockLink.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "RallyPoint.h"
#include "RallyPointController.h"
#include "Vehicle.h"

using namespace GeoZoneTestHelpers;

void GeoZonePlanningTest::_testPathHits()
{
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate square = center.atDistanceAndAzimuth(3000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
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
    const QList<GeoZonePathEvaluator::ZoneResult> hits = GeoZonePathEvaluator(manager.catalog(), GeoZonePolicy::alerts())
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
    const QList<GeoZonePathEvaluator::ZoneResult> pathHits = GeoZonePathEvaluator(manager.catalog(), GeoZonePolicy({ GeoZone::Prohibited })).evaluate({
        GeoZonePathEvaluator::Path::fromPositions({ GeoZone::Position{ center.atDistanceAndAzimuth(1000, 0) },
                                                    GeoZone::Position{ center.atDistanceAndAzimuth(1000, 0).atDistanceAndAzimuth(4000, 90) } }),
        GeoZonePathEvaluator::Path::fromPositions(east) }).zones;
    QCOMPARE(pathHits.count(), 1);
    QCOMPARE(pathHits[0].crossings.constFirst().entry.path, 1);

    // Restricted zones and unknown restrictions are warned about, others are information
    GeoZoneMonitor monitor(&manager);
    const QString warning = monitor.guidedCheck()->pathWarning(east);
    QVERIFY(warning.contains(QStringLiteral("A (Prohibited)")));
    QVERIFY(warning.contains(QStringLiteral("X (Unknown)")));
    QVERIFY(!warning.contains(QStringLiteral("N (")));
    QVERIFY(GeoZonePolicy::isRestricted(GeoZone::Unknown));
    QVERIFY(!GeoZonePolicy::isRestricted(GeoZone::USpace));
}

namespace {

QJsonObject _withLimits(QJsonObject feature, double lower, const QString &lowerReference, double upper, const QString &upperReference)
{
    QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = lower;
    layer[QStringLiteral("lowerVerticalReference")] = lowerReference;
    layer[QStringLiteral("upperLimit")] = upper;
    layer[QStringLiteral("upperVerticalReference")] = upperReference;
    geometry[0] = layer;
    feature[QStringLiteral("geometry")] = geometry;
    return feature;
}

GeoZonePathEvaluator::Path _evaluatorPath(const QList<QGeoCoordinate> &coordinates, const QList<double> &altitudes = {}, const QString &label = QString())
{
    GeoZonePathEvaluator::Path path;
    path.label = label;
    for (qsizetype i = 0; i < coordinates.count(); i++) {
        path.vertices.append(GeoZonePathEvaluator::Vertex{ coordinates[i], altitudes.value(i, qQNaN()), qQNaN(), QDateTime() });
    }
    return path;
}

/// @return Smallest horizontal distance from the path, sampled every few meters, to the zones
double _pathClearance(const QList<QGeoCoordinate> &path, const QList<const GeoZone*> &zones, double step = 5)
{
    double clearance = std::numeric_limits<double>::infinity();
    for (qsizetype i = 1; i < path.count(); i++) {
        const double length = path[i - 1].distanceTo(path[i]);
        const double azimuth = path[i - 1].azimuthTo(path[i]);
        for (double along = 0; along <= length; along += step) {
            const QGeoCoordinate point = path[i - 1].atDistanceAndAzimuth(along, azimuth);
            for (const GeoZone *zone : zones) {
                clearance = (std::min)(clearance, zone->distanceTo(point));
            }
        }
    }
    return clearance;
}

bool _near(double value, double expected, double tolerance = 1)
{
    if (qAbs(value - expected) <= tolerance) {
        return true;
    }
    qWarning() << "Expected" << expected << "got" << value;
    return false;
}

} // namespace

void GeoZonePlanningTest::_testEvaluatorCrossings()
{
    // A square 400 m wide 1000 m east of the start and a circle of 100 m radius 2000 m east of it
    const QGeoCoordinate start(47, 8);
    const QGeoCoordinate squareCenter = start.atDistanceAndAzimuth(1000, 90);
    const QGeoCoordinate circleCenter = start.atDistanceAndAzimuth(2000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("SQ"), QStringLiteral("PROHIBITED"), { squareCenter.atDistanceAndAzimuth(282.84, 315), squareCenter.atDistanceAndAzimuth(282.84, 45),
                                                                              squareCenter.atDistanceAndAzimuth(282.84, 135), squareCenter.atDistanceAndAzimuth(282.84, 225) }),
        _circleFeature(QStringLiteral("CI"), QStringLiteral("CONDITIONAL"), circleCenter, 100),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), circleCenter, 500),
    }), errorString), qPrintable(errorString));
    const GeoZonePathEvaluator evaluator(manager.catalog(), GeoZonePolicy({ GeoZone::Prohibited, GeoZone::Conditional }));

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
    const auto permittedZone = std::find_if(manager.zoneList().cbegin(), manager.zoneList().cend(), [](const GeoZone *zone) { return zone->identifier() == QStringLiteral("N"); });
    QVERIFY(permittedZone != manager.zoneList().cend());
    const GeoZonePathEvaluator::ZoneResult permitted = GeoZonePathEvaluator(manager.catalog(), GeoZonePolicy()).evaluateZone(*permittedZone, { east });
    QCOMPARE(permitted.zone, *permittedZone);
    QCOMPARE(permitted.crossings.count(), 1);
    QVERIFY(_near(permitted.crossings[0].entry.distance, 1500));
}

void GeoZonePlanningTest::_testEvaluatorAltitudes()
{
    // From 100 to 200 m above mean sea level, and from 50 to 150 m above the ground
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate groundCenter = center.atDistanceAndAzimuth(5000, 0);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _withLimits(_circleFeature(QStringLiteral("BAND"), QStringLiteral("PROHIBITED"), center, 300), 100, QStringLiteral("AMSL"), 200, QStringLiteral("AMSL")),
        _withLimits(_circleFeature(QStringLiteral("AGL"), QStringLiteral("PROHIBITED"), groundCenter, 300), 50, QStringLiteral("AGL"), 150, QStringLiteral("AGL")),
    }), errorString), qPrintable(errorString));
    GeoZonePathEvaluator evaluator(manager.catalog(), GeoZonePolicy({ GeoZone::Prohibited }));
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

void GeoZonePlanningTest::_testEvaluatorSchedules()
{
    // Active on 1 January 2027 from 10:00 to 12:00 UTC, 1900 to 2100 m along a path of 4000 m
    const QGeoCoordinate start(47, 8);
    QJsonObject temporary = _circleFeature(QStringLiteral("T"), QStringLiteral("PROHIBITED"), start.atDistanceAndAzimuth(2000, 90), 100);
    temporary[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-01-01T10:00:00Z" }, { "endDateTime", "2027-01-01T12:00:00Z" } } };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ temporary }), errorString), qPrintable(errorString));
    const QDate day(2027, 1, 1);
    GeoZonePathEvaluator::Path path = _evaluatorPath({ start, start.atDistanceAndAzimuth(4000, 90) });

    // A minute per 100 m, from the start time
    const auto evaluateFrom = [&](const QTime &time) {
        const QDateTime flightStart = _utc(day, time);
        const QDateTime flightEnd = flightStart.addSecs(40 * 60);
        path.setTimes(flightStart, flightEnd);
        return GeoZonePathEvaluator(manager.catalog(), GeoZonePolicy::mission(flightStart, flightEnd), flightStart).evaluate({ path });
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
    QVERIFY(GeoZonePathEvaluator(manager.catalog(), GeoZonePolicy::mission(_utc(day, QTime(9, 30)), _utc(day, QTime(10, 30))))
                .evaluate({ path }).zones[0].active());
    QVERIFY(!GeoZonePathEvaluator(manager.catalog(), GeoZonePolicy::mission(_utc(day, QTime(8, 0)), _utc(day, QTime(9, 0))))
                .evaluate({ path }).zones[0].active());
    QVERIFY(GeoZonePathEvaluator(manager.catalog(), GeoZonePolicy::mission(QDateTime(), QDateTime())).evaluate({ path }).zones[0].active());
    // Zones active now are those active at the evaluation time
    QVERIFY(GeoZonePathEvaluator(manager.catalog(), GeoZonePolicy::alerts(), _utc(day, QTime(11, 0))).evaluate({ path }).zones[0].active());
    QVERIFY(!GeoZonePathEvaluator(manager.catalog(), GeoZonePolicy::alerts(), _utc(day, QTime(13, 0))).evaluate({ path }).zones[0].active());

    // Mission paths estimate the times in proportion to the distance along the mission
    GeoZoneMissionPath::Segment segment;
    segment.from = start;
    segment.to = start.atDistanceAndAzimuth(4000, 90);
    segment.distance = 4000;
    const GeoZoneMissionPath missionPath = GeoZoneMissionPath::fromSegments({ segment });
    const QList<GeoZonePathEvaluator::Path> missionPaths = missionPath.evaluatorPaths(_utc(day, QTime(10, 0)), _utc(day, QTime(10, 40)));
    QCOMPARE(missionPaths.count(), 1);
    QVERIFY(missionPaths[0].vertices.count() > 2);
    QCOMPARE(missionPaths[0].vertices.constFirst().time, _utc(day, QTime(10, 0)));
    QCOMPARE(missionPaths[0].vertices.constLast().time, _utc(day, QTime(10, 40)));
    const double middleMSecs = (missionPath.paths()[0].distances[1] / 4000) * 40 * 60 * 1000;
    QVERIFY(_near(static_cast<double>(_utc(day, QTime(10, 0)).msecsTo(missionPaths[0].vertices[1].time)), middleMSecs));
}

void GeoZonePlanningTest::_testFenceExcludes()
{
    const QGeoCoordinate center(47, 8);
    const auto local = [&center](double east, double north) {
        return center.atDistanceAndAzimuth(std::hypot(east, north), qRadiansToDegrees(std::atan2(east, north)));
    };
    ZoneOwner owner;
    const auto polygonZone = [&owner](const QList<QList<QGeoCoordinate>> &rings) {
        GeoZone::Data data;
        data.restriction = GeoZone::Prohibited;
        for (const QList<QGeoCoordinate> &ring : rings) {
            GeoZone::Volume volume;
            volume.rings = { GeoZone::toRing(ring) };
            data.volumes.append(volume);
        }
        return static_cast<const GeoZone*>(owner.add(data));
    };
    // A circle published as a polygon of 40 vertices, which simplifies, and a circle
    const GeoZone *const round = polygonZone({ _circleRing(center, 500, 40) });
    GeoZone::Volume circleVolume;
    circleVolume.isCircle = true;
    circleVolume.center = local(2000, 0);
    circleVolume.radius = 200;
    GeoZone::Data circleData;
    circleData.restriction = GeoZone::Prohibited;
    circleData.volumes = { circleVolume };
    const GeoZone *const circle = owner.add(circleData);

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);

    // Zones imported with or without simplification are excluded, the fences are those the importer makes
    for (const double tolerance : { 0.0, 25.0 }) {
        QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, round));
        QCOMPARE(importer.addExclusions({ round, circle }, tolerance), 2);
        const QList<QGeoCoordinate> fence = geoFenceController->polygons()->value<QGCFencePolygon*>(0)->coordinateList();
        QCOMPARE(fence, GeoZoneFences::zoneFences(round, tolerance).polygons.constFirst());
        QVERIFY((tolerance > 0) == (fence.count() < 40));
        QVERIFY(GeoZoneMissionCheck::fenceExcludes(geoFenceController, round));
        QVERIFY(GeoZoneMissionCheck::fenceExcludes(geoFenceController, circle));
        GeoZoneFenceTags::deleteFences(geoFenceController, false);
    }

    // Zones the importer has no fence for, like those without area, are not excluded whatever the fence
    geoFenceController->addInclusionCircle(local(-5000, 5000), local(5000, -5000));
    QCOMPARE(importer.addExclusions({ round }, 0), 1);
    const GeoZone *const line = polygonZone({ { local(-10, 0), local(10, 0) } });
    QVERIFY(GeoZoneFences::zoneFences(line).isEmpty());
    QCOMPARE(importer.addExclusions({ line }), 0);
    QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, line));
    QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, owner.add(GeoZone::Data())));
    // Nor are zones partly outside, like when one of their polygons isn't fenced
    const GeoZone *const twoParts = polygonZone({ _circleRing(center, 500, 40), _circleRing(local(0, 3000), 100, 8) });
    QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, twoParts));
    GeoZoneFenceTags::deleteFences(geoFenceController, false);

    // A fence around all vertices of a square zone whose notch cuts its north edge doesn't exclude it
    const GeoZone *const square = polygonZone({ { local(-100, 100), local(100, 100), local(100, -100), local(-100, -100) } });
    QGCFencePolygon *const notched = new QGCFencePolygon(false, geoFenceController);
    notched->appendVertices({ local(-200, -200), local(200, -200), local(200, 200), local(20, 200), local(20, 50), local(-20, 50), local(-20, 200), local(-200, 200) });
    geoFenceController->polygons()->append(notched);
    QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, square));
    notched->clear();
    notched->appendVertices({ local(-200, -200), local(200, -200), local(200, 200), local(-200, 200) });
    QVERIFY(GeoZoneMissionCheck::fenceExcludes(geoFenceController, square));
    GeoZoneFenceTags::deleteFences(geoFenceController, false);
}

void GeoZonePlanningTest::_testAntimeridianQueries()
{
    // A zone from 179.9 E to 179.9 W, one on the other side of the world and one along the equator from 0 to 160 W,
    // spanning more than 180 degrees of longitude
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), { QGeoCoordinate(-16.6, 179.9), QGeoCoordinate(-16.4, 179.9),
                                                                             QGeoCoordinate(-16.4, -179.9), QGeoCoordinate(-16.6, -179.9) }),
        _circleFeature(QStringLiteral("FAR"), QStringLiteral("PROHIBITED"), QGeoCoordinate(-16.5, 0), 5000),
        _polygonFeature(QStringLiteral("WIDE"), QStringLiteral("PROHIBITED"), { QGeoCoordinate(-1, 0), QGeoCoordinate(-1, 50), QGeoCoordinate(-1, 100),
                                                                                QGeoCoordinate(-1, 150), QGeoCoordinate(-1, -160), QGeoCoordinate(1, -160),
                                                                                QGeoCoordinate(1, 150), QGeoCoordinate(1, 100), QGeoCoordinate(1, 50),
                                                                                QGeoCoordinate(1, 0) }),
    }), errorString), qPrintable(errorString));
    const GeoZoneCatalog &catalog = manager.catalog();
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

    // The profile of a segment across the antimeridian passes through the zone
    GeoZoneMissionPath::Segment segment;
    segment.from = from;
    segment.to = to;
    segment.distance = from.distanceTo(to);
    const QList<GeoZoneProfile::Band> bands = GeoZoneProfile::computeBands({ segment }, catalog);
    QCOMPARE(bands.count(), 1);
    QCOMPARE(bands[0].name, QStringLiteral("X"));
    QVERIFY(qAbs(bands[0].points.constFirst().distance - entryDistance) < 20);
    QVERIFY(qAbs(bands[0].points.constLast().distance - (segment.distance - entryDistance)) < 20);

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

void GeoZonePlanningTest::_testMissionCheck()
{
    // Before the temporary restriction of 2027
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(10, 0)));
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

    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
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
    QCOMPARE(check.crossedZones()[0]->identifier(), QStringLiteral("P"));
    QCOMPARE(check.crossedZones()[1]->identifier(), QStringLiteral("C"));
    QVERIFY(check.warningText().contains(QStringLiteral("P (Prohibited)")));
    // The path north also passes through the U-space airspace
    QCOMPARE(check.uSpaceZones().count(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("U-space airspace: U.")));
    QVERIFY(check.crossedZonesBoundingBox().contains(center));
    QVERIFY(check.crossedZonesBoundingBox().contains(center.atDistanceAndAzimuth(2000, 0)));

    // The fence must exclude the prohibited zones, circles and polygons added from them do
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    QVERIFY(geoFenceController->supported());
    check.setGeoFenceController(geoFenceController);
    QTRY_COMPARE(check.unfencedZones().count(), 1);
    QVERIFY(check.fenceText().contains(QStringLiteral("P")));
    geoFenceController->addInclusionCircle(center.atDistanceAndAzimuth(1000, 315), center.atDistanceAndAzimuth(1000, 135));
    check.update();
    QCOMPARE(check.unfencedZones().count(), 1);
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);
    QCOMPARE(importer.addZones(check.property("unfencedZones").toList()), 1);
    QTRY_VERIFY(check.unfencedZones().isEmpty());
    QVERIFY(check.fenceText().isEmpty());
    GeoZoneFenceTags::deleteFences(geoFenceController, false);
    QTRY_COMPARE(check.unfencedZones().count(), 1);
    // A polygon zone excluded by a simplified polygon fence
    const QList<QGeoCoordinate> squareZone = { center.atDistanceAndAzimuth(300, 315), center.atDistanceAndAzimuth(300, 45),
                                               center.atDistanceAndAzimuth(300, 135), center.atDistanceAndAzimuth(300, 225) };
    GeoZone::Data squareData;
    squareData.restriction = GeoZone::Prohibited;
    GeoZone::Volume squareVolume;
    squareVolume.rings = { GeoZone::toRing(squareZone) };
    squareData.volumes = { squareVolume };
    GeoZone square(squareData);
    QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, &square));
    QCOMPARE(importer.addExclusions({ &square }, 25), 1);
    QVERIFY(GeoZoneMissionCheck::fenceExcludes(geoFenceController, &square));
    GeoZoneFenceTags::deleteFences(geoFenceController, false);
    check.setGeoFenceController(nullptr);

    // A temporary restriction on the path counts at any time, or when it is active during the planned flight
    QJsonObject temporary = _circleFeature(QStringLiteral("T"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(700, 270), 100);
    temporary[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-01-01T10:00:00Z" }, { "endDateTime", "2027-01-01T12:00:00Z" } } };
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("temporary.json"), QJsonArray{ temporary }), errorString), qPrintable(errorString));
    QTRY_COMPARE(check.crossedZones().count(), 3);
    QCOMPARE(check.inactiveZoneCount(), 0);
    manager.planning()->setPlannedStart(_utc(QDate(2027, 1, 1), QTime(8, 0)));
    QTRY_COMPARE(check.crossedZones().count(), 2);
    QCOMPARE(check.inactiveZoneCount(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("1 zone(s) on the path are not active at the planned flight time.")));
    // The mission takes a few minutes, so a start just before the restriction overlaps it
    const QDateTime overlappingStart = _utc(QDate(2027, 1, 1), QTime(10, 0)).addSecs(-std::llround(missionController->missionTime() / 2));
    QVERIFY(missionController->missionTime() > 10);
    manager.planning()->setPlannedStart(overlappingStart);
    QTRY_COMPARE(check.crossedZones().count(), 3);
    // A mission started now, like one on the vehicle, ignores the planned start
    check.setFlightNow(true);
    QTRY_COMPARE(check.crossedZones().count(), 2);
    QVERIFY(!check.infoText().contains(QStringLiteral("planned flight time")));
    check.setFlightNow(false);
    manager.planning()->clearPlannedStart();
    QTRY_COMPARE(check.inactiveZoneCount(), 0);
    QCOMPARE(check.crossedZones().count(), 3);

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
        const QList<const GeoZone*> crossed = check.crossedZones();
        return std::any_of(crossed.cbegin(), crossed.cend(), [](const GeoZone *zone) {
            return zone->identifier() == QStringLiteral("S");
        });
    };
    QTRY_VERIFY(surveyZoneCrossed());

    // Zones are released before the manager deletes them
    manager.clear();
    QVERIFY(check.crossedZones().isEmpty());
}

void GeoZonePlanningTest::_testMissionCoverage()
{
    const ScopedEnvironment environment;
    // L under a loiter circle away from the legs, V past a back transition, and R on the way home above the drawn flight
    // path, which descends home, at the altitude of the return
    const QGeoCoordinate home(47, 8);
    const QGeoCoordinate loiter = home.atDistanceAndAzimuth(2000, 90);
    const QGeoCoordinate last = loiter.atDistanceAndAzimuth(2000, 0);
    QJsonObject returnZone = _circleFeature(QStringLiteral("R"), QStringLiteral("PROHIBITED"), home.atDistanceAndAzimuth(home.distanceTo(last) / 2, home.azimuthTo(last)), 100);
    QJsonArray geometry = returnZone[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = 440;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
    layer[QStringLiteral("upperLimit")] = 2000;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AMSL");
    geometry[0] = layer;
    returnZone[QStringLiteral("geometry")] = geometry;
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("L"), QStringLiteral("PROHIBITED"), loiter.atDistanceAndAzimuth(300, 180), 40),
        _circleFeature(QStringLiteral("V"), QStringLiteral("PROHIBITED"), last.atDistanceAndAzimuth(150, 0), 30),
        returnZone,
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    SimpleMissionItem *const loiterItem = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(loiter, 1));
    QVERIFY(loiterItem);
    loiterItem->setCommand(MAV_CMD_NAV_LOITER_UNLIM);
    loiterItem->setRadius(300);
    (void) missionController->insertSimpleMissionItem(last, 2);
    SimpleMissionItem *const transitionItem = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(last, 3));
    transitionItem->setCommand(MAV_CMD_DO_VTOL_TRANSITION);
    transitionItem->missionItem().setParam1(MAV_VTOL_STATE_MC);
    SimpleMissionItem *const returnItem = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(last, 4));
    returnItem->setCommand(MAV_CMD_NAV_RETURN_TO_LAUNCH);

    // The drawn flight path enters none of them. Offline the PX4 multicopter returns at its default altitude, 30 m above
    // home, which is below the items 50 m high.
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(check.crossedZones().count(), 3);
    const auto zone = [&check](const QString &identifier) {
        const auto it = std::find_if(check.crossedZones().cbegin(), check.crossedZones().cend(), [&identifier](const GeoZone *crossed) { return crossed->identifier() == identifier; });
        return (it != check.crossedZones().cend()) ? *it : nullptr;
    };
    QCOMPARE(check.pathLabels(zone(QStringLiteral("L"))), QStringList({ QStringLiteral("the loiter at item 1") }));
    QCOMPARE(check.pathLabels(zone(QStringLiteral("V"))), QStringList({ QStringLiteral("the VTOL transition at item 3") }));
    QCOMPARE(check.pathLabels(zone(QStringLiteral("R"))), QStringList({ QStringLiteral("the return to launch assuming the PX4 default return altitude of 30 m") }));
    QVERIFY2(check.warningText().contains(QStringLiteral("Only the loiter at item 1 enters L.")), qPrintable(check.warningText()));

    // A smaller loiter stays clear of L
    loiterItem->setRadius(100);
    QTRY_VERIFY(!zone(QStringLiteral("L")));
    QCOMPARE(check.crossedZones().count(), 2);
}

namespace {

/// The feature with AMSL limits in meters
QJsonObject withAmslLimits(QJsonObject feature, double lower, double upper)
{
    QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = lower;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
    layer[QStringLiteral("upperLimit")] = upper;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AMSL");
    geometry[0] = layer;
    feature[QStringLiteral("geometry")] = geometry;
    return feature;
}

QStringList zoneIdentifiers(const QList<const GeoZone*> &zones)
{
    QStringList identifiers;
    for (const GeoZone *zone : zones) {
        identifiers.append(zone->identifier());
    }
    return identifiers;
}

/// Meters north and east of the coordinate
QGeoCoordinate offsetFrom(const QGeoCoordinate &coordinate, double north, double east)
{
    return coordinate.atDistanceAndAzimuth(std::hypot(north, east), qRadiansToDegrees(std::atan2(east, north)));
}

} // namespace

void GeoZonePlanningTest::_testMissionJumps()
{
    const ScopedEnvironment environment;
    // A loop north and east of home flown again by a DO_JUMP, then on east. J is on the jump back, away from the drawn path.
    const QGeoCoordinate home(47, 8);
    const QGeoCoordinate north = offsetFrom(home, 1000, 0);
    const QGeoCoordinate northEast = offsetFrom(home, 1000, 1000);
    const QGeoCoordinate east = offsetFrom(home, 0, 1000);
    const QGeoCoordinate farEast = offsetFrom(home, 0, 3000);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("J"), QStringLiteral("PROHIBITED"), offsetFrom(home, 500, 500), 50),
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    (void) missionController->insertSimpleMissionItem(north, 1);
    (void) missionController->insertSimpleMissionItem(northEast, 2);
    (void) missionController->insertSimpleMissionItem(east, 3);
    SimpleMissionItem *const jumpItem = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(east, 4));
    QVERIFY(jumpItem);
    jumpItem->setCommand(MAV_CMD_DO_JUMP);
    jumpItem->missionItem().setParam1(1);
    jumpItem->missionItem().setParam2(2);
    (void) missionController->insertSimpleMissionItem(farEast, 5);
    QCOMPARE(jumpItem->sequenceNumber(), 4);
    QTRY_COMPARE(GeoZoneMissionPath::fromMission(missionController).segments().count(), 4);

    // The loop from the first item to the third and the jump back to the first are flown twice more
    const GeoZoneMissionPath path = GeoZoneMissionPath::fromMission(missionController);
    const double loop = path.itemDistance(3) - path.itemDistance(1) + east.distanceTo(north);
    QVERIFY(loop > 3000);
    QVERIFY2(std::abs(path.flightDistance() - (path.totalDistance() + (2 * loop))) < 1, qPrintable(QString::number(path.flightDistance())));
    QCOMPARE(path.itemFlightDistance(3), path.itemDistance(3));
    QVERIFY(std::abs(path.itemFlightDistance(5) - (path.itemDistance(5) + (2 * loop))) < 1);
    QVERIFY(!path.repeatsIndefinitely());
    int jumps = 0;
    for (const GeoZoneMissionPath::Path &flown : path.paths()) {
        if (flown.label == QStringLiteral("the jump at item 4")) {
            jumps++;
            QVERIFY(flown.positions.first().coordinate.distanceTo(east) < 1);
            QVERIFY(flown.positions.last().coordinate.distanceTo(north) < 1);
            // At the distance the vehicle flies it, after the first pass of the loop
            QVERIFY(flown.distances.first() >= path.itemDistance(3));
        }
    }
    QCOMPARE(jumps, 2);
    // The flight lasts longer than the estimate without the loops, at the same pace
    const QDateTime start = _utc(QDate(2026, 6, 15), QTime(10, 0));
    const QDateTime end = start.addSecs(1000);
    QVERIFY(std::abs(start.msecsTo(path.flightEnd(start, end)) - (1000000 * path.flightDistance() / path.totalDistance())) < 2);

    // The mission check reports the zone only the jump enters
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(zoneIdentifiers(check.crossedZones()), QStringList{ QStringLiteral("J") });
    QCOMPARE(check.pathLabels(check.crossedZones().first()), QStringList{ QStringLiteral("the jump at item 4") });
    // Without repeats the jump isn't flown
    for (int i = 0; i < jumpItem->textFieldFacts()->count(); i++) {
        Fact *const fact = jumpItem->textFieldFacts()->value<Fact*>(i);
        if (fact->rawValue().toInt() == 2) {
            fact->setRawValue(0);
        }
    }
    QCOMPARE(qRound(jumpItem->missionItem().param2()), 0);
    QTRY_VERIFY(check.crossedZones().isEmpty());

    // Repeating without end the loop is flown once more
    jumpItem->missionItem().setParam2(-1);
    const GeoZoneMissionPath endless = GeoZoneMissionPath::fromMission(missionController);
    QVERIFY(endless.repeatsIndefinitely());
    QVERIFY(std::abs(endless.flightDistance() - (endless.totalDistance() + loop)) < 1);
    const auto endlessJump = std::find_if(endless.paths().cbegin(), endless.paths().cend(), [](const GeoZoneMissionPath::Path &flown) {
        return flown.label == QStringLiteral("the jump at item 4, repeated without end");
    });
    QVERIFY(endlessJump != endless.paths().cend());

    // Many repeats stop at the limit of the segments flown
    jumpItem->missionItem().setParam2(1000000);
    const GeoZoneMissionPath capped = GeoZoneMissionPath::fromMission(missionController);
    QVERIFY(capped.flightDistance() > (capped.totalDistance() + (100 * loop)));
    QVERIFY(capped.flightDistance() < (capped.totalDistance() + (((GeoZoneMissionPath::maxFlightSegments / 3) + 2) * loop)));
}

void GeoZonePlanningTest::_testMissionContingencies()
{
    const ScopedEnvironment environment;
    // Around a square north and east of home. X is in the middle of the square, where returns home from the second
    // item pass, R on the way from it to a rally point east of it.
    const QGeoCoordinate home(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        withAmslLimits(_circleFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), offsetFrom(home, 1000, 1000), 100), 440, 1000),
        _circleFeature(QStringLiteral("R"), QStringLiteral("PROHIBITED"), offsetFrom(home, 2000, 2500), 50),
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    (void) missionController->insertSimpleMissionItem(offsetFrom(home, 2000, 0), 1);
    (void) missionController->insertSimpleMissionItem(offsetFrom(home, 2000, 2000), 2);
    (void) missionController->insertSimpleMissionItem(offsetFrom(home, 0, 2000), 3);

    // The offline PX4 multicopter returns home at the altitude of the items, 50 m above home, higher than its default
    // return altitude. The mission doesn't enter X, returns from around the second item would.
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(zoneIdentifiers(check.contingencyZones()), QStringList{ QStringLiteral("X") });
    QVERIFY(check.crossedZones().isEmpty());
    QVERIFY(check.warningText().isEmpty());
    const GeoZone *const x = check.contingencyZones().first();
    QCOMPARE(check.contingencyLabels(x), QStringList{ QStringLiteral(u"a return to launch assuming the PX4 default return altitude of 30 m from items 1\u20133") });
    QCOMPARE(check.contingencyText(), QStringLiteral(u"A return to launch assuming the PX4 default return altitude of 30 m from items 1\u20133 would enter X (Prohibited)."));

    // The starts are grouped into ranges of items
    const GeoZoneMissionPath path = GeoZoneMissionPath::fromMission(missionController, true);
    const QList<GeoZoneMissionPath::ContingencyStart> &starts = path.contingencyStarts();
    QVERIFY(starts.count() > 10);
    QVERIFY(starts.count() <= GeoZoneMissionPath::maxContingencyStarts);
    const auto atItem = [&starts](int sequenceNumber) {
        const auto it = std::find_if(starts.cbegin(), starts.cend(), [sequenceNumber](const GeoZoneMissionPath::ContingencyStart &start) {
            return (start.fromSequenceNumber == sequenceNumber) && (start.toSequenceNumber == sequenceNumber);
        });
        return (it != starts.cend()) ? static_cast<int>(it - starts.cbegin()) : -1;
    };
    const int second = atItem(2);
    QVERIFY((atItem(1) == 0) && (second > 0) && (atItem(3) == (starts.count() - 1)));
    QCOMPARE(path.startsText({ second }), QStringLiteral("item 2"));
    QCOMPARE(path.startsText({ second + 1, second, second - 1 }), QStringLiteral(u"items 1\u20133"));
    QCOMPARE(path.startsText({ 0, second }), QStringLiteral("items 1, 2"));
    QCOMPARE(path.contingencyPaths().count(), starts.count());

    // A rally point closer than home, the returns from around the second item go there
    RallyPointController *const rallyPointController = masterController.rallyPointController();
    rallyPointController->addPoint(offsetFrom(home, 2000, 3000));
    QTRY_COMPARE(zoneIdentifiers(check.contingencyZones()), QStringList{ QStringLiteral("R") });
    QVERIFY(check.contingencyLabels(check.contingencyZones().first()).first().startsWith(QStringLiteral("a return to rally point 1 assuming the PX4 default return altitude of 30 m from items")));
    // Moved away it isn't closer anymore
    rallyPointController->points()->value<RallyPoint*>(0)->setCoordinate(offsetFrom(home, 6000, 6000));
    QTRY_COMPARE(zoneIdentifiers(check.contingencyZones()), QStringList{ QStringLiteral("X") });

    // Zones are released before the manager deletes them
    manager.clear();
    QVERIFY(check.contingencyZones().isEmpty());
    QVERIFY(check.contingencyText().isEmpty());
}

void GeoZonePlanningTest::_testMissionReturnParameters()
{
    const ScopedEnvironment environment;
    // X is above the returns at 60 m from around the second item, 70 m above home and higher. B is on the way from
    // the first item to the breach return point west of it.
    const QGeoCoordinate home(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        withAmslLimits(_circleFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), offsetFrom(home, 1000, 1000), 100), 470, 1000),
        _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), offsetFrom(home, 2000, -500), 50),
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    for (const QGeoCoordinate &coordinate : { offsetFrom(home, 2000, 0), offsetFrom(home, 2000, 2000), offsetFrom(home, 0, 2000) }) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
        QVERIFY(item);
        item->altitude()->setRawValue(30);
    }
    QTRY_COMPARE(GeoZoneMissionPath::fromMission(missionController).segments().count(), 3);
    const auto contingencies = [&manager, missionController](const GeoZoneReturnModel::Parameters &parameters) {
        return GeoZoneMissionCheck::evaluate(manager.catalog(), GeoZoneMissionPath::fromMission(missionController, parameters, true));
    };

    // Of unknown firmware at an unknown altitude, entering X
    GeoZoneMissionCheck::Evaluation evaluation = contingencies(GeoZoneReturnModel::Parameters());
    QCOMPARE(zoneIdentifiers(evaluation.contingencyZones), QStringList{ QStringLiteral("X") });
    QCOMPARE(evaluation.contingencyLabels.value(evaluation.contingencyZones.first()),
             QStringList{ QStringLiteral(u"a return to launch at an unknown altitude from items 1\u20133") });

    // ArduCopter at 60 m stays below X, at 100 m it enters it
    GeoZoneReturnModel::Parameters copter;
    copter.autopilot = GeoZoneReturnModel::Autopilot::ArduCopter;
    copter.multicopter = true;
    copter.returnAltitude = 60;
    evaluation = contingencies(copter);
    QVERIFY(evaluation.zones.isEmpty());
    QVERIFY(evaluation.contingencyZones.isEmpty());
    QVERIFY(contingencies(GeoZoneReturnModel::defaultParameters(GeoZoneReturnModel::Firmware::ArduPilot, GeoZoneReturnModel::VehicleClass::Multirotor))
                .contingencyZones.isEmpty());
    copter.returnAltitude = 100;
    evaluation = contingencies(copter);
    QCOMPARE(zoneIdentifiers(evaluation.contingencyZones), QStringList{ QStringLiteral("X") });
    QCOMPARE(evaluation.contingencyLabels.value(evaluation.contingencyZones.first()), QStringList{ QStringLiteral(u"a return to launch from items 1\u20133") });
    // The cone of a PX4 multicopter doesn't lower the return this far from home
    GeoZoneReturnModel::Parameters px4;
    px4.autopilot = GeoZoneReturnModel::Autopilot::PX4;
    px4.multicopter = true;
    px4.returnAltitude = 100;
    px4.coneHalfAngle = 45;
    px4.coneRadius = 10;
    QCOMPARE(zoneIdentifiers(contingencies(px4).contingencyZones), QStringList{ QStringLiteral("X") });

    // ArduPlane flying to the breach return point after a fence breach, at FENCE_RET_ALT
    masterController.geoFenceController()->setBreachReturnPoint(offsetFrom(home, 2000, -1000));
    GeoZoneReturnModel::Parameters plane;
    plane.autopilot = GeoZoneReturnModel::Autopilot::ArduPlane;
    plane.returnAltitude = 50;
    plane.loiterRadius = 80;
    plane.breachAction = GeoZoneReturnModel::BreachAction::ReturnPoint;
    plane.breachReturnAltitude = 40;
    evaluation = contingencies(plane);
    QCOMPARE(zoneIdentifiers(evaluation.contingencyZones), QStringList{ QStringLiteral("B") });
    QCOMPARE(evaluation.contingencyLabels.value(evaluation.contingencyZones.first()),
             QStringList{ QStringLiteral(u"a flight to the breach return point after a fence breach from items 1\u20133") });
    // Without the guided breach action only the returns home are checked
    plane.breachAction = GeoZoneReturnModel::BreachAction::Return;
    QVERIFY(contingencies(plane).contingencyZones.isEmpty());

    // The return to launch at the end is part of the planned flight, to the rally point closer than home
    masterController.rallyPointController()->addPoint(offsetFrom(home, 1000, 1000));
    SimpleMissionItem *const returnItem = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(home, missionController->visualItems()->count()));
    QVERIFY(returnItem);
    returnItem->setCommand(MAV_CMD_NAV_RETURN_TO_LAUNCH);
    evaluation = contingencies(copter);
    QCOMPARE(zoneIdentifiers(evaluation.zones), QStringList{ QStringLiteral("X") });
    QCOMPARE(evaluation.pathLabels.value(evaluation.zones.first()), QStringList{ QStringLiteral("the return to rally point 1") });
}

void GeoZonePlanningTest::_testAuthorisations()
{
    // Authorisations are for local days, the test stays within one
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(12, 0)));
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString storage = dir.filePath(QStringLiteral("storage"));
    GeoZoneManager manager(storage);
    QString errorString;
    QJsonObject controlZone = _circleFeature(QStringLiteral("CTR"), QStringLiteral("REQ_AUTHORISATION"), center, 300);
    controlZone[QStringLiteral("country")] = QStringLiteral("CHE");
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ controlZone }), errorString), qPrintable(errorString));
    GeoZone *const zone = manager.zoneList().constFirst();
    GeoZoneAuthorisations *const authorisations = manager.planning()->authorisations();
    const QVariant zoneInfo = QVariant::fromValue(GeoZoneInfo(zone));
    QCOMPARE(GeoZoneAuthorisations::zoneKey(zone), QStringLiteral("CHE/CTR"));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 2);
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(check.crossedZones().count(), 1);

    // Dates are local days, the last one included
    QVERIFY(!authorisations->authorise(zoneInfo, QString(), QStringLiteral("2026-13-40"), QString()).isEmpty());
    QVERIFY(!authorisations->authorise(zoneInfo, QString(), QStringLiteral("2026-11-02"), QStringLiteral("2026-11-01")).isEmpty());
    QCOMPARE(authorisations->count(), 0);

    // An authorisation without end is listed for information instead of warned about
    const QDate today = environment.now().toLocalTime().date();
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral(" P-123 "), QString(), QString()).isEmpty());
    QCOMPARE(authorisations->count(), 1);
    QCOMPARE(authorisations->authorisationOf(zoneInfo)[QStringLiteral("permit")].toString(), QStringLiteral("P-123"));
    QCOMPARE(authorisations->authorisationOf(zoneInfo)[QStringLiteral("statusText")].toString(), QStringLiteral("Valid"));
    QTRY_VERIFY(check.crossedZones().isEmpty());
    QCOMPARE(check.authorisedZones().count(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("Authorised: CTR.")));

    // Alerts and guided commands leave the zone out while authorised
    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 0);
    const QList<GeoZone::Position> path = { { center.atDistanceAndAzimuth(1000, 270) }, { center.atDistanceAndAzimuth(1000, 90) } };
    QVERIFY(monitor.guidedCheck()->pathWarning(path).isEmpty());

    // Expired authorisations don't count
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-122"), QString(), today.addDays(-1).toString(Qt::ISODate)).isEmpty());
    QCOMPARE(authorisations->count(), 1);
    QVERIFY(authorisations->authorisationOf(zoneInfo)[QStringLiteral("statusText")].toString().startsWith(QStringLiteral("Expired")));
    QTRY_COMPARE(check.crossedZones().count(), 1);
    QVERIFY(!monitor.guidedCheck()->pathWarning(path).isEmpty());
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 1);

    // An authorisation from tomorrow covers a flight planned then, not one now
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-124"), today.addDays(1).toString(Qt::ISODate), today.addDays(3).toString(Qt::ISODate)).isEmpty());
    manager.planning()->setPlannedStart(QDateTime(today, QTime(12, 0)));
    QTRY_COMPARE(check.crossedZones().count(), 1);
    manager.planning()->setPlannedStart(QDateTime(today.addDays(2), QTime(12, 0)));
    QTRY_VERIFY(check.crossedZones().isEmpty());

    // Plans entering the zone keep the planned start and the authorisation
    QJsonObject planJson;
    GeoZonePlanData::save(&masterController, planJson, &manager);
    const QJsonObject geoZones = planJson[QString(GeoZonePlanData::jsonKey)].toObject();
    QCOMPARE(QDateTime::fromString(geoZones[QStringLiteral("plannedStart")].toString(), Qt::ISODate), QDateTime(today.addDays(2), QTime(12, 0)).toUTC());
    QCOMPARE(geoZones[QStringLiteral("authorisations")].toArray().count(), 1);
    QCOMPARE(geoZones[QStringLiteral("authorisations")].toArray()[0].toObject()[QStringLiteral("permit")].toString(), QStringLiteral("P-124"));

    // Opening the plan offers its authorisations, which are only added when accepted
    authorisations->revoke(QStringLiteral("CHE/CTR"));
    manager.planning()->clearPlannedStart();
    QCOMPARE(authorisations->count(), 0);
    GeoZonePlanData::load(nullptr, planJson, &manager);
    QCOMPARE(manager.planning()->plannedStart(), QDateTime(today.addDays(2), QTime(12, 0)).toUTC());
    QCOMPARE(authorisations->count(), 0);
    QCOMPARE(authorisations->pendingAuthorisations().count(), 1);
    QCOMPARE(authorisations->pendingAuthorisations()[0].toMap()[QStringLiteral("permit")].toString(), QStringLiteral("P-124"));
    authorisations->dismissPending();
    QCOMPARE(authorisations->count(), 0);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());
    GeoZonePlanData::load(nullptr, planJson, &manager);
    authorisations->acceptPending();
    QCOMPARE(authorisations->count(), 1);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());
    // Authorisations already known aren't offered again
    GeoZonePlanData::load(nullptr, planJson, &manager);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());

    // Kept across restarts next to the stored sources
    GeoZoneManager restarted(storage);
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QCOMPARE(restarted.zoneCount(), 1);
    QCOMPARE(restarted.planning()->authorisations()->count(), 1);
    QVERIFY(restarted.planning()->authorisations()->find(restarted.zoneList().constFirst()));
    manager.planning()->clearPlannedStart();
}

void GeoZonePlanningTest::_testBriefing()
{
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2027, 1, 1), QTime(8, 0)));
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));

    // A control zone with its authority, message, schedule and limits, a prison, and zones near the path
    QJsonObject controlZone = _circleFeature(QStringLiteral("CTR-1"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1000, 90), 200);
    controlZone[QStringLiteral("name")] = QStringLiteral("CTR <script>alert(1)</script>");
    controlZone[QStringLiteral("country")] = QStringLiteral("CHE");
    controlZone[QStringLiteral("message")] = QStringLiteral("Call the tower");
    controlZone[QStringLiteral("zoneAuthority")] = QJsonArray{ QJsonObject{ { "name", "Skyguide" }, { "email", "ops@example.org" }, { "phone", "+41 43 931 61 11" } } };
    controlZone[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-01-01T10:45:00Z" }, { "endDateTime", "2027-01-01T12:00:00Z" } } };
    QJsonObject geometry = controlZone[QStringLiteral("geometry")].toArray()[0].toObject();
    geometry[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    geometry[QStringLiteral("lowerLimit")] = 0;
    geometry[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AGL");
    geometry[QStringLiteral("upperLimit")] = 500;
    geometry[QStringLiteral("upperVerticalReference")] = QStringLiteral("AMSL");
    controlZone[QStringLiteral("geometry")] = QJsonArray{ geometry };
    QJsonObject prison = _circleFeature(QStringLiteral("P-1"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2500, 90), 200);
    prison[QStringLiteral("name")] = QStringLiteral("Prison");
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        controlZone, prison,
        _circleFeature(QStringLiteral("NEAR"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1500, 90).atDistanceAndAzimuth(400, 0), 150),
        _circleFeature(QStringLiteral("FAR"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 0), 150),
        _circleFeature(QStringLiteral("RET"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2000, 90).atDistanceAndAzimuth(1000, 180), 200),
        // Zones without restriction near the path aren't briefed
        _circleFeature(QStringLiteral("INFO"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(1500, 90).atDistanceAndAzimuth(400, 180), 100),
    }), errorString), qPrintable(errorString));
    const GeoZone *const ctr = manager.zoneList().constFirst();

    GeoZoneAuthorisations authorisations{ QString() };
    authorisations.set(GeoZoneAuthorisation{ GeoZoneAuthorisations::zoneKey(ctr), ctr->name(), QStringLiteral("P-77"), QDateTime(), QDateTime() });

    // Ten minutes east at 100 m, a return from halfway would enter RET
    const QDateTime start = _utc(QDate(2027, 1, 1), QTime(10, 30));
    GeoZonePathEvaluator::Path path = GeoZonePathEvaluator::Path::fromPositions({ GeoZone::Position{ center, 100 }, GeoZone::Position{ center.atDistanceAndAzimuth(4000, 90), 100 } });
    path.setTimes(start, start.addSecs(600));
    GeoZonePathEvaluator::Path contingency = GeoZonePathEvaluator::Path::fromPositions({ GeoZone::Position{ center.atDistanceAndAzimuth(2000, 90), 100 },
                                                                                         GeoZone::Position{ center.atDistanceAndAzimuth(2000, 90).atDistanceAndAzimuth(1000, 180), 100 } },
                                                                                       QStringLiteral("a return to rally point 1"));
    contingency.setTimes(start.addSecs(300), QDateTime());
    GeoZoneBriefing::Input input;
    input.mission.name = QStringLiteral("Survey");
    input.mission.plannedStart = start;
    input.mission.plannedEnd = start.addSecs(600);
    input.paths = { path };
    input.contingencyPaths = { contingency };
    input.items = { { 1, 0 }, { 2, 4000 } };
    input.start = start;
    input.end = start.addSecs(600);
    input.authorisations = &authorisations;
    input.sources = GeoZoneSourceVersion::covering(manager.repository()->sourceList(), GeoZoneBox());
    const GeoZoneBriefing briefing = GeoZoneBriefing::build(manager.catalog(), input);

    // The zones entered in the order the path enters them, where, when and at which altitudes
    const QList<const GeoZoneBriefing::Zone*> entered = briefing.zonesWith(GeoZoneBriefing::Involvement::Entered);
    QCOMPARE(entered.count(), 2);
    const GeoZoneBriefing::Zone &briefedCtr = *entered[0];
    QCOMPARE(briefedCtr.identifier, QStringLiteral("CTR-1"));
    QCOMPARE(briefedCtr.restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(briefedCtr.country, QStringLiteral("CHE"));
    QCOMPARE(briefedCtr.crossings.count(), 1);
    const GeoZoneBriefing::Crossing &crossing = briefedCtr.crossings.constFirst();
    QCOMPARE(crossing.entryItem, 1);
    QVERIFY(qAbs(crossing.entryDistance - 800) < 2);
    QVERIFY(qAbs(crossing.exitDistance - 1200) < 2);
    QVERIFY(qAbs(crossing.entryTime.secsTo(start.addSecs(120))) <= 1);
    QCOMPARE(crossing.pathAltitudeMin, 100.0);
    QCOMPARE(crossing.pathAltitudeMax, 100.0);
    QVERIFY(qIsNaN(crossing.zoneFloor));
    QCOMPARE(crossing.zoneCeiling, 500.0);
    QVERIFY(briefedCtr.authorities.join(QChar(u'\n')).contains(QStringLiteral("Skyguide")));
    QVERIFY(briefedCtr.authorities.join(QChar(u'\n')).contains(QStringLiteral("ops@example.org")));
    QCOMPARE(briefedCtr.message, QStringLiteral("Call the tower"));
    QVERIFY(!briefedCtr.schedule.isEmpty());
    // Active from 10:45 within the flight widened by the tolerance, 10:15 to 10:55
    QVERIFY(briefedCtr.activeDuringFlight);
    QCOMPARE(briefedCtr.activePeriods.count(), 1);
    QCOMPARE(briefedCtr.activePeriods[0].start, _utc(QDate(2027, 1, 1), QTime(10, 45)));
    QCOMPARE(briefedCtr.activePeriods[0].end, _utc(QDate(2027, 1, 1), QTime(10, 55)));
    QVERIFY(briefedCtr.authorisation.has_value());
    QCOMPARE(briefedCtr.authorisation->permit, QStringLiteral("P-77"));
    QVERIFY(briefedCtr.authorisation->coversFlight);
    QCOMPARE(briefedCtr.contentHash, ctr->data().contentHash);
    QCOMPARE(entered[1]->displayName(), QStringLiteral("Prison"));
    QCOMPARE(entered[1]->activePeriods, (QList<GeoZoneBriefing::Interval>{ { start.addSecs(-15 * 60), start.addSecs(25 * 60) } }));
    QVERIFY(!entered[1]->authorisation);

    // The zone only the return enters, and the restricted zone near the path
    const QList<const GeoZoneBriefing::Zone*> contingencyZones = briefing.zonesWith(GeoZoneBriefing::Involvement::Contingency);
    QCOMPARE(contingencyZones.count(), 1);
    QCOMPARE(contingencyZones[0]->identifier, QStringLiteral("RET"));
    QCOMPARE(contingencyZones[0]->contingencies, QStringList{ QStringLiteral("a return to rally point 1") });
    const QList<const GeoZoneBriefing::Zone*> nearby = briefing.zonesWith(GeoZoneBriefing::Involvement::Nearby);
    QCOMPARE(nearby.count(), 1);
    QCOMPARE(nearby[0]->identifier, QStringLiteral("NEAR"));
    QVERIFY(qAbs(nearby[0]->distance - 250) < 5);

    // The version of the data
    QCOMPARE(briefing.sources.count(), 1);
    QVERIFY(!briefing.sources[0].contentHash.isEmpty());
    QCOMPARE(GeoZoneSourceVersion::airacCycleOf(QDate(2026, 1, 22)), QStringLiteral("2601"));
    QCOMPARE(GeoZoneSourceVersion::airacCycleOf(QDate(2026, 1, 21)), QStringLiteral("2513"));
    QCOMPARE(GeoZoneSourceVersion::airacCycleOf(QDate(2020, 12, 31)), QStringLiteral("2014"));
    QCOMPARE(GeoZoneSourceVersion::airacCycleOf(QDate(2021, 1, 28)), QStringLiteral("2101"));
    QCOMPARE(GeoZoneSourceVersion::airacCycleOf(QDate(2019, 12, 31)), QStringLiteral("1913"));

    // JSON with its schema and version
    const QJsonObject json = GeoZoneBriefingExport::toJson(briefing);
    QCOMPARE(json[QStringLiteral("schema")].toString(), QStringLiteral("qgc-geozone-briefing"));
    QCOMPARE(json[QStringLiteral("version")].toInt(), 1);
    QCOMPARE(json[QStringLiteral("mission")].toObject()[QStringLiteral("name")].toString(), QStringLiteral("Survey"));
    QCOMPARE(json[QStringLiteral("mission")].toObject()[QStringLiteral("plannedStart")].toString(), QStringLiteral("2027-01-01T10:30:00Z"));
    const QJsonArray zones = json[QStringLiteral("zones")].toArray();
    QCOMPARE(zones.count(), 4);
    const QJsonObject ctrJson = zones[0].toObject();
    QCOMPARE(ctrJson[QStringLiteral("involvement")].toString(), QStringLiteral("entered"));
    QCOMPARE(ctrJson[QStringLiteral("restriction")].toString(), QStringLiteral("req_authorisation"));
    QCOMPARE(ctrJson[QStringLiteral("crossings")].toArray()[0].toObject()[QStringLiteral("entryItem")].toInt(), 1);
    QVERIFY(ctrJson[QStringLiteral("crossings")].toArray()[0].toObject()[QStringLiteral("zoneFloor")].isNull());
    QCOMPARE(ctrJson[QStringLiteral("activePeriods")].toArray()[0].toObject()[QStringLiteral("start")].toString(), QStringLiteral("2027-01-01T10:45:00Z"));
    QCOMPARE(ctrJson[QStringLiteral("authorisation")].toObject()[QStringLiteral("permit")].toString(), QStringLiteral("P-77"));
    QCOMPARE(ctrJson[QStringLiteral("contentHash")].toString(), QString::fromLatin1(ctr->data().contentHash.toHex()));
    QVERIFY(ctrJson[QStringLiteral("distance")].isNull());
    QCOMPARE(zones[2].toObject()[QStringLiteral("involvement")].toString(), QStringLiteral("contingency"));
    QCOMPARE(zones[3].toObject()[QStringLiteral("involvement")].toString(), QStringLiteral("nearby"));
    QVERIFY(zones[3].toObject()[QStringLiteral("distance")].isDouble());
    const QJsonObject sourceJson = json[QStringLiteral("sources")].toArray()[0].toObject();
    QCOMPARE(sourceJson[QStringLiteral("contentHash")].toString(), QString::fromLatin1(briefing.sources[0].contentHash.toHex()));
    QVERIFY(sourceJson.contains(QStringLiteral("airacCycle")));

    // The web page escapes the text of the zones
    const QString html = GeoZoneBriefingExport::toHtml(briefing);
    QVERIFY(html.startsWith(QStringLiteral("<!DOCTYPE html>")));
    QVERIFY(html.contains(QStringLiteral("name=\"viewport\"")));
    QVERIFY(!html.contains(QStringLiteral("<script")));
    QVERIFY(html.contains(QStringLiteral("CTR &lt;script&gt;alert(1)&lt;/script&gt;")));
    for (const QString &text : { QStringLiteral("Skyguide"), QStringLiteral("P-77"), QStringLiteral("Call the tower"), QStringLiteral("Prison"), QStringLiteral("NEAR"),
                                 QStringLiteral("RET"), QString::fromLatin1(briefing.sources[0].contentHash.toHex()) }) {
        QVERIFY2(html.contains(text), qPrintable(text));
    }
    QVERIFY(!html.contains(QStringLiteral("FAR")));

    // A4 PDF
    const QString pdfFile = dir.filePath(QStringLiteral("briefing.pdf"));
    QVERIFY2(GeoZoneBriefingExport::write(briefing, pdfFile, GeoZoneBriefingExport::Format::Pdf, errorString), qPrintable(errorString));
    QFile pdf(pdfFile);
    QVERIFY(pdf.open(QIODevice::ReadOnly));
    const QByteArray content = pdf.readAll();
    QVERIFY(content.startsWith("%PDF-"));
    QVERIFY(content.trimmed().endsWith("%%EOF"));
    QVERIFY(content.contains("/Type /Page"));
}

void GeoZonePlanningTest::_testBriefingExport()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1000, 90), 200),
        _circleFeature(QStringLiteral("B"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(2500, 90), 200),
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(500, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(3500, 90), 2);

    // The flight path is updated after the items
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(check.crossedZones().count(), 2);

    GeoZoneBriefingExport exporter(&manager);
    exporter.setMissionController(missionController);
    QCOMPARE(exporter.defaultFileName(), QStringLiteral("geozone-briefing.pdf"));
    QVERIFY(!exporter.defaultFolder().isEmpty());

    // The zones the mission enters after its first item
    const QString jsonFile = dir.filePath(QStringLiteral("mission.json"));
    QVERIFY2(exporter.exportBriefing(jsonFile), qPrintable(exporter.errorString()));
    QFile file(jsonFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(json[QStringLiteral("version")].toInt(), GeoZoneBriefingExport::jsonVersion);
    const QJsonObject mission = json[QStringLiteral("mission")].toObject();
    QCOMPARE(mission[QStringLiteral("itemCount")].toInt(), 2);
    QVERIFY(mission[QStringLiteral("distanceMeters")].toDouble() > 3900);
    QVERIFY(!mission[QStringLiteral("returnAssumptions")].toArray().isEmpty());
    QVERIFY(mission[QStringLiteral("plannedStart")].isNull());
    QStringList identifiers;
    for (const QJsonValue &zone : json[QStringLiteral("zones")].toArray()) {
        if (zone.toObject()[QStringLiteral("involvement")].toString() == QStringLiteral("entered")) {
            identifiers.append(zone.toObject()[QStringLiteral("identifier")].toString());
            QCOMPARE(zone.toObject()[QStringLiteral("crossings")].toArray()[0].toObject()[QStringLiteral("entryItem")].toInt(), 1);
        }
    }
    QCOMPARE(identifiers, (QStringList{ QStringLiteral("A"), QStringLiteral("B") }));
    QCOMPARE(json[QStringLiteral("sources")].toArray().count(), 1);

    // PDF without an extension, errors reported
    QVERIFY2(exporter.exportBriefing(dir.filePath(QStringLiteral("briefing"))), qPrintable(exporter.errorString()));
    QVERIFY(QFileInfo(dir.filePath(QStringLiteral("briefing.pdf"))).size() > 0);
    QVERIFY2(exporter.exportBriefing(dir.filePath(QStringLiteral("briefing.html"))), qPrintable(exporter.errorString()));
    QVERIFY(!exporter.exportBriefing(dir.filePath(QStringLiteral("missing/briefing.json"))));
    QVERIFY(!exporter.errorString().isEmpty());
}

void GeoZonePlanningTest::_testPlanAudit()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    const QJsonObject zoneA = _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1000, 90), 200);
    const QJsonObject zoneB = _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2500, 90), 200);
    const QJsonObject zoneD = _circleFeature(QStringLiteral("D"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2000, 0), 200);
    const auto load = [&](const QJsonArray &features) {
        QString errorString;
        return manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString);
    };
    QVERIFY(load({ zoneA, zoneB, zoneD }));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(500, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(3500, 90), 2);
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    const auto relevant = [&check]() {
        QStringList identifiers;
        for (const GeoZone *zone : check.relevantZones()) {
            identifiers.append(zone->identifier());
        }
        identifiers.sort();
        return identifiers;
    };
    QTRY_COMPARE(relevant(), (QStringList{ QStringLiteral("A"), QStringLiteral("B") }));

    // Saved with the hashes of the zones the mission enters and the version of the source
    QJsonObject planJson;
    GeoZonePlanData::save(&masterController, planJson, &manager);
    const QJsonObject audit = planJson[QString(GeoZonePlanData::jsonKey)].toObject()[QStringLiteral("audit")].toObject();
    QCOMPARE(audit[QStringLiteral("version")].toInt(), GeoZonePlanAudit::version);
    QCOMPARE(audit[QStringLiteral("zones")].toArray().count(), 2);
    QVERIFY(!audit[QStringLiteral("zones")].toArray()[0].toObject()[QStringLiteral("contentHash")].toString().isEmpty());
    QCOMPARE(audit[QStringLiteral("sources")].toArray().count(), 1);
    QVERIFY(manager.planning()->planAudit().has_value());
    check.update();
    QVERIFY(check.auditText().isEmpty());

    // Reopened with the same data
    manager.planning()->setPlanAudit(std::nullopt);
    GeoZonePlanData::load(nullptr, planJson, &manager);
    QVERIFY(manager.planning()->planAudit().has_value());
    check.update();
    QVERIFY(check.auditText().isEmpty());

    // Editing the mission into a zone published before isn't a change of the data
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 0), 3);
    QTRY_VERIFY(relevant().contains(QStringLiteral("D")));
    QVERIFY(check.auditText().isEmpty());
    missionController->removeVisualItem(3);
    QTRY_COMPARE(relevant(), (QStringList{ QStringLiteral("A"), QStringLiteral("B") }));

    // A zone whose geometry changed
    const QJsonObject largerA = _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1000, 90), 250);
    QVERIFY(load({ largerA, zoneB, zoneD }));
    check.update();
    QVERIFY2(check.auditText().contains(QStringLiteral("1 zone(s) modified")), qPrintable(check.auditText()));
    QVERIFY(check.auditText().contains(QStringLiteral("1 source(s) updated")));
    QVERIFY2(check.auditDetails().contains(QStringLiteral("Modified: A (Authorisation required)")), qPrintable(check.auditDetails().join(QChar(u'\n'))));

    // A zone removed and a new one on the path
    const QJsonObject zoneE = _circleFeature(QStringLiteral("E"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1800, 90), 100);
    QVERIFY(load({ largerA, zoneD, zoneE }));
    check.update();
    QVERIFY(check.auditText().contains(QStringLiteral("1 new")));
    QVERIFY(check.auditText().contains(QStringLiteral("1 removed")));
    QVERIFY(check.auditDetails().contains(QStringLiteral("Removed: B (Prohibited)")));
    QVERIFY(check.auditDetails().contains(QStringLiteral("New: E (Prohibited)")));

    // Acknowledged until the plan is opened again, the file keeps the data it was saved with
    check.acknowledgeAudit();
    check.update();
    QVERIFY(check.auditText().isEmpty());
    GeoZonePlanData::load(nullptr, planJson, &manager);
    check.update();
    QVERIFY(!check.auditText().isEmpty());

    // Saving again records the current data
    QJsonObject resaved;
    GeoZonePlanData::save(&masterController, resaved, &manager);
    check.update();
    QVERIFY(check.auditText().isEmpty());
    GeoZonePlanData::load(nullptr, resaved, &manager);
    check.update();
    QVERIFY(check.auditText().isEmpty());

    // Plans without the audit load silently
    QJsonObject legacy = planJson;
    QJsonObject legacyZones = legacy[QString(GeoZonePlanData::jsonKey)].toObject();
    legacyZones.remove(QStringLiteral("audit"));
    legacy[QString(GeoZonePlanData::jsonKey)] = legacyZones;
    GeoZonePlanData::load(nullptr, legacy, &manager);
    QVERIFY(!manager.planning()->planAudit().has_value());
    check.update();
    QVERIFY(check.auditText().isEmpty());
    GeoZonePlanData::load(nullptr, planJson, &manager);
    GeoZonePlanData::load(nullptr, QJsonObject(), &manager);
    QVERIFY(!manager.planning()->planAudit().has_value());
}

void GeoZonePlanningTest::_testRoutePlanner()
{
    ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate west = center.atDistanceAndAzimuth(2000, 270);
    const QGeoCoordinate east = center.atDistanceAndAzimuth(2000, 90);
    QJsonObject raised = _circleFeature(QStringLiteral("R"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1200, 90), 200);
    QJsonArray geometry = raised[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = 30;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AGL");
    geometry[0] = layer;
    raised[QStringLiteral("geometry")] = geometry;
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"),
        QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300), raised }), errorString), qPrintable(errorString));
    const GeoZone *const prohibited = manager.searchZones(QStringLiteral("P")).isEmpty() ? nullptr : GeoZoneInfo::fromVariant(manager.searchZones(QStringLiteral("P")).first());
    QVERIFY(prohibited && (prohibited->identifier() == QStringLiteral("P")));

    // Around the zone, keeping the margin
    const std::optional<QList<QGeoCoordinate>> detour = GeoZoneRoutePlanner::detourAround(west, east, { prohibited });
    QVERIFY(detour && !detour->isEmpty());
    QList<QGeoCoordinate> path = { west };
    path.append(*detour);
    path.append(east);
    QVERIFY(!prohibited->intersectsPath(path));
    for (qsizetype i = 1; i < path.count(); i++) {
        for (int step = 0; step <= 10; step++) {
            const QGeoCoordinate point = path[i - 1].atDistanceAndAzimuth(path[i - 1].distanceTo(path[i]) * step / 10, path[i - 1].azimuthTo(path[i]));
            QVERIFY2(prohibited->distanceTo(point) > (GeoZoneRoutePlanner::detourMargin / 2), qPrintable(QString::number(prohibited->distanceTo(point))));
        }
    }
    // Not needed, or impossible from inside
    QVERIFY(GeoZoneRoutePlanner::detourAround(west, center.atDistanceAndAzimuth(2000, 0), { prohibited })->isEmpty());
    QVERIFY(!GeoZoneRoutePlanner::detourAround(center.atDistanceAndAzimuth(350, 0), east, { prohibited }));

    // Suggestions for a mission through both zones at 50 m
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    for (const QGeoCoordinate &coordinate : { west, east }) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
        QVERIFY(item);
        item->altitude()->setRawValue(50);
    }
    GeoZoneRoutePlanner planner(&manager);
    planner.setMissionController(missionController);
    planner.update();
    QStringList texts;
    int detourIndex = -1;
    for (int i = 0; i < planner.suggestions().count(); i++) {
        const QVariantMap suggestion = planner.suggestions()[i].toMap();
        texts.append(suggestion[QStringLiteral("text")].toString());
        if (suggestion[QStringLiteral("canApply")].toBool()) {
            detourIndex = i;
        }
    }
    QVERIFY2(texts.filter(QStringLiteral("fly below the floor of R")).count() == 1, qPrintable(texts.join(QStringLiteral(" | "))));
    QVERIFY2(detourIndex >= 0, qPrintable(texts.join(QStringLiteral(" | "))));
    QVERIFY(texts[detourIndex].contains(QStringLiteral("detour around P, R")));

    // Inserting the detour leaves the zones
    const int itemCount = missionController->visualItems()->count();
    QVERIFY(planner.applySuggestion(detourIndex));
    QVERIFY(missionController->visualItems()->count() > itemCount);
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_VERIFY_WITH_TIMEOUT(check.crossedZones().isEmpty(), 5000);
    QTRY_VERIFY(planner.suggestions().isEmpty());
    QVERIFY(!planner.applySuggestion(0));

    // Zones on either side of a leg are passed between instead of around both
    ZoneOwner owner;
    const auto circleZone = [&owner](const QGeoCoordinate &zoneCenter, double radius) {
        GeoZone::Volume volume;
        volume.isCircle = true;
        volume.center = zoneCenter;
        volume.radius = radius;
        GeoZone::Data data;
        data.restriction = GeoZone::Prohibited;
        data.volumes.append(volume);
        return static_cast<const GeoZone*>(owner.add(data));
    };
    const QList<const GeoZone*> sides = { circleZone(center.atDistanceAndAzimuth(1000, 270).atDistanceAndAzimuth(300, 0), 400),
                                          circleZone(center.atDistanceAndAzimuth(1000, 90).atDistanceAndAzimuth(300, 180), 400) };
    const std::optional<QList<QGeoCoordinate>> weave = GeoZoneRoutePlanner::detourAround(west, east, sides);
    QVERIFY(weave && !weave->isEmpty());
    QList<QGeoCoordinate> weavePath = { west };
    weavePath.append(*weave);
    weavePath.append(east);
    for (qsizetype i = 1; i < weavePath.count(); i++) {
        QVERIFY2(weavePath[i].distanceTo(QGeoCoordinate(center.latitude(), weavePath[i].longitude())) < 400, qPrintable(QString::number(i)));
        for (int step = 0; step <= 20; step++) {
            const QGeoCoordinate point = weavePath[i - 1].atDistanceAndAzimuth(weavePath[i - 1].distanceTo(weavePath[i]) * step / 20, weavePath[i - 1].azimuthTo(weavePath[i]));
            for (const GeoZone *side : sides) {
                QVERIFY(side->distanceTo(point) > (GeoZoneRoutePlanner::detourMargin / 2));
            }
        }
    }

    // A leg through a zone above the sea can be lowered below its floor, where the terrain is known whether or not
    // the terrain tiles are received
    environment.setTerrain(0.0);
    const QGeoCoordinate seaCenter(0.5, -25);
    const auto seaZone = [&seaCenter](const QString &identifier, double floor) {
        QJsonObject feature = _circleFeature(identifier, QStringLiteral("REQ_AUTHORISATION"), seaCenter, 200);
        QJsonArray seaGeometry = feature[QStringLiteral("geometry")].toArray();
        QJsonObject seaLayer = seaGeometry[0].toObject();
        seaLayer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
        seaLayer[QStringLiteral("lowerLimit")] = floor;
        seaLayer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
        seaGeometry[0] = seaLayer;
        feature[QStringLiteral("geometry")] = seaGeometry;
        return feature;
    };
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("sea.json"), QJsonArray{ seaZone(QStringLiteral("S"), 40) }), errorString), qPrintable(errorString));
    PlanMasterController lowerController;
    lowerController.setFlyView(false);
    lowerController.start();
    MissionController *const lowerMission = lowerController.missionController();
    const QGeoCoordinate legStart = seaCenter.atDistanceAndAzimuth(800, 270);
    lowerMission->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(legStart.latitude(), legStart.longitude(), 0));
    QList<SimpleMissionItem*> legItems;
    for (const QGeoCoordinate &coordinate : { legStart, seaCenter.atDistanceAndAzimuth(800, 90) }) {
        legItems.append(qobject_cast<SimpleMissionItem*>(lowerMission->insertSimpleMissionItem(coordinate, lowerMission->visualItems()->count())));
        legItems.last()->altitude()->setRawValue(50);
    }
    GeoZoneRoutePlanner lowerPlanner(&manager);
    lowerPlanner.setMissionController(lowerMission);
    const auto floorSuggestion = [&lowerPlanner]() {
        lowerPlanner.update();
        for (int i = 0; i < lowerPlanner.suggestions().count(); i++) {
            const QVariantMap suggestion = lowerPlanner.suggestions()[i].toMap();
            if (suggestion[QStringLiteral("canApply")].toBool() && (suggestion[QStringLiteral("applyText")].toString() == QStringLiteral("Apply"))) {
                return i;
            }
        }
        return -1;
    };
    const auto suggestionTexts = [&lowerPlanner]() {
        QStringList texts;
        for (const QVariant &suggestion : lowerPlanner.suggestions()) {
            texts.append(suggestion.toMap()[QStringLiteral("text")].toString());
        }
        return texts.join(QStringLiteral(" | "));
    };
    QTRY_VERIFY2(floorSuggestion() >= 0, qPrintable(suggestionTexts()));
    QVERIFY(suggestionTexts().contains(QStringLiteral("Apply lowers 2 waypoint(s) to 10 m below it.")));
    QVERIFY(lowerPlanner.applySuggestion(floorSuggestion()));
    QCOMPARE(legItems[0]->altitude()->rawValue().toDouble(), 30.0);
    QCOMPARE(legItems[1]->altitude()->rawValue().toDouble(), 30.0);
    QTRY_VERIFY2((lowerPlanner.update(), lowerPlanner.suggestions().isEmpty()), qPrintable(suggestionTexts()));

    // Not when the lowered leg would come too close to the sea
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("sea_low.json"), QJsonArray{ seaZone(QStringLiteral("T"), 15) }), errorString), qPrintable(errorString));
    QTRY_VERIFY((lowerPlanner.update(), suggestionTexts().contains(QStringLiteral("floor of T"))));
    QCOMPARE(floorSuggestion(), -1);
}

namespace {

/// Prohibited zone with a polygon volume given in meters around the origin, with an optional hole
const GeoZone *_localPolygonZone(ZoneOwner &owner, const GeoZoneGeometry::LocalProjection &projection, const QString &name, const QList<QPointF> &ring,
                                 const QList<QPointF> &hole = {})
{
    const auto toRing = [&projection](const QList<QPointF> &points) {
        QList<QGeoCoordinate> coordinates;
        for (const QPointF &point : points) {
            coordinates.append(projection.toGeo(point));
        }
        return GeoZone::toRing(coordinates);
    };
    GeoZone::Volume volume;
    volume.rings.append(toRing(ring));
    if (!hole.isEmpty()) {
        volume.rings.append(toRing(hole));
    }
    GeoZone::Data data;
    data.identifier = name;
    data.restriction = GeoZone::Prohibited;
    data.volumes.append(volume);
    return owner.add(data);
}

/// U open to the north, width by height meters, with a notch of the width and depth
QList<QPointF> _uShape(double width, double height, double notchWidth, double notchDepth)
{
    const double w = width / 2;
    const double h = height / 2;
    const double n = notchWidth / 2;
    return { { -w, -h }, { w, -h }, { w, h }, { n, h }, { n, h - notchDepth }, { -n, h - notchDepth }, { -n, h }, { -w, h } };
}

QList<QPointF> _square(double halfSize)
{
    return { { -halfSize, -halfSize }, { halfSize, -halfSize }, { halfSize, halfSize }, { -halfSize, halfSize } };
}

/// @return The path from the start to the end through the waypoints of the route
QList<QGeoCoordinate> _routePath(const QGeoCoordinate &from, const GeoZoneRouteSearch::Route &route, const QGeoCoordinate &to)
{
    QList<QGeoCoordinate> path = { from };
    path.append(route.waypoints);
    path.append(to);
    return path;
}

} // namespace

void GeoZonePlanningTest::_testRouteConcave_data()
{
    QTest::addColumn<QList<QPointF>>("ring");
    QTest::addColumn<QList<QPointF>>("hole");
    QTest::addColumn<QPointF>("start");
    QTest::addColumn<QPointF>("end");
    QTest::addColumn<int>("failure");
    QTest::addColumn<int>("minWaypoints");
    QTest::addColumn<double>("maxLength");   ///< Meters, 0 for no limit

    using Failure = GeoZoneRouteSearch::Failure;
    // A notch wider than twice the margin can be entered, the corner of the U's arm is passed at the margin. Convex
    // hulls would block it.
    QTest::newRow("into wide notch") << _uShape(1500, 1500, 600, 1000) << QList<QPointF>() << QPointF(1500, 2500) << QPointF(0, 0)
                                     << int(Failure::None) << 1 << 3000.0;
    // A notch narrower than twice the margin is passed around, not through
    QTest::newRow("across narrow notch") << _uShape(1500, 1500, 120, 1000) << QList<QPointF>() << QPointF(-2000, 300) << QPointF(2000, 300)
                                         << int(Failure::None) << 2 << 5200.0;
    // Around the closed side of a U opening towards the start, not into the notch
    QTest::newRow("around U") << _uShape(1500, 1500, 600, 1000) << QList<QPointF>() << QPointF(0, 2000) << QPointF(0, -2000)
                              << int(Failure::None) << 2 << 5200.0;
    // Inside a hole the zone is open, so the leg needs no detour, but it surrounds the hole
    QTest::newRow("within hole") << _square(2000) << _square(1500) << QPointF(-1000, -1000) << QPointF(1000, 1000) << int(Failure::None) << 0 << 0.0;
    QTest::newRow("into hole") << _square(2000) << _square(1500) << QPointF(-3000, 0) << QPointF(0, 0) << int(Failure::Enclosed) << 0 << 0.0;
    QTest::newRow("start inside") << _square(500) << QList<QPointF>() << QPointF(0, 0) << QPointF(2000, 0) << int(Failure::StartInside) << 0 << 0.0;
    QTest::newRow("end inside") << _square(500) << QList<QPointF>() << QPointF(-2000, 0) << QPointF(100, 100) << int(Failure::EndInside) << 0 << 0.0;
}

void GeoZonePlanningTest::_testRouteConcave()
{
    QFETCH(QList<QPointF>, ring);
    QFETCH(QList<QPointF>, hole);
    QFETCH(QPointF, start);
    QFETCH(QPointF, end);
    QFETCH(int, failure);
    QFETCH(int, minWaypoints);
    QFETCH(double, maxLength);

    const GeoZoneGeometry::LocalProjection projection(QGeoCoordinate(47, 8));
    ZoneOwner owner;
    const GeoZone *const zone = _localPolygonZone(owner, projection, QStringLiteral("U"), ring, hole);
    const QGeoCoordinate from = projection.toGeo(start);
    const QGeoCoordinate to = projection.toGeo(end);
    const GeoZoneRouteSearch::Settings settings;
    const GeoZoneRouteSearch::Route route = GeoZoneRouteSearch::detourAround(from, to, { zone }, settings);
    QCOMPARE(int(route.failure), failure);
    if (!route.isValid()) {
        QCOMPARE(route.zones, QStringList{ QStringLiteral("U") });
        return;
    }
    QVERIFY2(route.waypoints.count() >= minWaypoints, qPrintable(QString::number(route.waypoints.count())));
    const QList<QGeoCoordinate> path = _routePath(from, route, to);
    QVERIFY(!zone->intersectsPath(path));
    // The margin from the concave outline, not only from its corners
    const double clearance = _pathClearance(path, { zone });
    QVERIFY2(clearance > (settings.margin - 1), qPrintable(QString::number(clearance)));
    if (maxLength > 0) {
        QVERIFY2(route.length < maxLength, qPrintable(QString::number(route.length)));
    }
    QVERIFY(route.statistics.rounds >= 1);
}

void GeoZonePlanningTest::_testRouteVertical_data()
{
    QTest::addColumn<double>("radius");
    QTest::addColumn<double>("floor");          ///< Meters AMSL, NaN for the ground
    QTest::addColumn<double>("ceiling");        ///< Meters AMSL, NaN without
    QTest::addColumn<double>("terrain");        ///< Meters AMSL, of a hill of 600 m radius at the zone center with hill
    QTest::addColumn<bool>("hill");             ///< The terrain is 0 elsewhere
    QTest::addColumn<int>("best");              ///< Kind, -1 without
    QTest::addColumn<int>("climbFailure");
    QTest::addColumn<int>("descentFailure");
    QTest::addColumn<double>("climbLimit");     ///< Height above ground of the failed climb, NaN to skip
    QTest::addColumn<double>("descentLimit");   ///< Height above ground of the failed descent, NaN to skip

    using Failure = GeoZoneRouteSearch::Failure;
    using Kind = GeoZoneRouteSearch::Kind;
    const double nan = qQNaN();
    // The leg flies at 50 m AMSL from 6 km west to 6 km east of the zone center
    QTest::newRow("climb cheaper") << 3000.0 << nan << 100.0 << 0.0 << false << int(Kind::Climb) << int(Failure::None) << int(Failure::ReachesGround)
                                   << nan << nan;
    QTest::newRow("ceiling too high") << 3000.0 << nan << 500.0 << 0.0 << false << int(Kind::Detour) << int(Failure::AboveMaxAltitude)
                                      << int(Failure::ReachesGround) << 510.0 << nan;
    QTest::newRow("detour cheaper") << 300.0 << nan << 100.0 << 0.0 << false << int(Kind::Detour) << int(Failure::None) << int(Failure::ReachesGround)
                                    << nan << nan;
    QTest::newRow("descent") << 3000.0 << 40.0 << 1000.0 << 0.0 << false << int(Kind::Descent) << int(Failure::AboveMaxAltitude) << int(Failure::None)
                             << 1010.0 << nan;
    QTest::newRow("descent too low") << 3000.0 << 40.0 << 1000.0 << 25.0 << false << int(Kind::Detour) << int(Failure::AboveMaxAltitude)
                                     << int(Failure::BelowMinClearance) << 985.0 << 5.0;
    QTest::newRow("descent over hill") << 3000.0 << 40.0 << 1000.0 << 25.0 << true << int(Kind::Detour) << int(Failure::AboveMaxAltitude)
                                       << int(Failure::BelowMinClearance) << 1010.0 << 5.0;
    QTest::newRow("descent terrain unknown") << 3000.0 << 40.0 << 1000.0 << nan << false << int(Kind::Detour) << int(Failure::TerrainUnknown)
                                             << int(Failure::TerrainUnknown) << nan << nan;
}

void GeoZonePlanningTest::_testRouteVertical()
{
    QFETCH(double, radius);
    QFETCH(double, floor);
    QFETCH(double, ceiling);
    QFETCH(double, terrain);
    QFETCH(bool, hill);
    QFETCH(int, best);
    QFETCH(int, climbFailure);
    QFETCH(int, descentFailure);
    QFETCH(double, climbLimit);
    QFETCH(double, descentLimit);

    const QGeoCoordinate center(47, 8);
    QJsonObject feature = _circleFeature(QStringLiteral("Z"), QStringLiteral("PROHIBITED"), center, radius);
    if (!qIsNaN(floor) || !qIsNaN(ceiling)) {
        feature = _withLimits(feature, qIsNaN(floor) ? 0 : floor, qIsNaN(floor) ? QStringLiteral("AGL") : QStringLiteral("AMSL"), ceiling, QStringLiteral("AMSL"));
    }
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ feature }), errorString), qPrintable(errorString));

    const GeoZonePathEvaluator::TerrainLookup lookup = [&center, terrain, hill](const QGeoCoordinate &coordinate) {
        return (!hill || (coordinate.distanceTo(center) < 600)) ? terrain : 0.0;
    };
    const GeoZonePolicy policy = GeoZonePolicy::routing(QDateTime(), QDateTime());
    GeoZoneRouteSearch search(manager.catalog(), policy);
    search.setTerrainLookup(lookup);
    GeoZoneRouteSearch::Leg leg;
    leg.from = GeoZone::Position{ center.atDistanceAndAzimuth(6000, 270), 50 };
    leg.to = GeoZone::Position{ center.atDistanceAndAzimuth(6000, 90), 50 };
    const GeoZoneRouteSearch::Result result = search.plan(leg);
    QCOMPARE(result.crossedZones, QStringList{ QStringLiteral("Z") });
    QCOMPARE(result.routes.count(), 3);
    const GeoZoneRouteSearch::Route *const bestRoute = result.best();
    QVERIFY(bestRoute);
    QCOMPARE(int(bestRoute->kind), best);
    for (const GeoZoneRouteSearch::Route &route : result.routes) {
        if (route.kind == GeoZoneRouteSearch::Kind::Detour) {
            QVERIFY(route.isValid());
        } else {
            const bool climb = (route.kind == GeoZoneRouteSearch::Kind::Climb);
            QVERIFY2(int(route.failure) == (climb ? climbFailure : descentFailure), qPrintable(QStringLiteral("%1 %2").arg(int(route.kind)).arg(int(route.failure))));
            const double limit = climb ? climbLimit : descentLimit;
            if (!qIsNaN(limit)) {
                QVERIFY2(_near(route.limit, limit), qPrintable(QString::number(route.limit)));
            }
        }
        if (!route.isValid()) {
            QCOMPARE(route.zones, QStringList{ QStringLiteral("Z") });
            continue;
        }
        // Valid routes keep out of the zone, as the evaluator finds
        GeoZonePathEvaluator evaluator(manager.catalog(), policy);
        evaluator.setTerrainLookup(lookup);
        QList<GeoZone::Position> positions;
        if (route.kind == GeoZoneRouteSearch::Kind::Detour) {
            for (const QGeoCoordinate &coordinate : _routePath(leg.from.coordinate, route, leg.to.coordinate)) {
                positions.append(GeoZone::Position{ coordinate, 50 });
            }
            QVERIFY(_pathClearance(_routePath(leg.from.coordinate, route, leg.to.coordinate), manager.catalog().search(QStringLiteral("Z"), 1), 20) > 99);
            QVERIFY(_near(route.cost, route.length - leg.from.coordinate.distanceTo(leg.to.coordinate), 5));
        } else {
            const double target = (route.kind == GeoZoneRouteSearch::Kind::Climb) ? (ceiling + 10) : (floor - 10);
            QVERIFY(_near(route.altitude, target, 0.01));
            QVERIFY(_near(route.fromAltitude, target, 0.01));
            QVERIFY(_near(route.toAltitude, target, 0.01));
            QVERIFY(_near(route.cost, std::abs(target - 50) * (search.settings().climbWeight + search.settings().descentWeight), 0.1));
            positions = { GeoZone::Position{ leg.from.coordinate, route.fromAltitude }, GeoZone::Position{ leg.to.coordinate, route.toAltitude } };
        }
        QVERIFY(evaluator.evaluate({ GeoZonePathEvaluator::Path::fromPositions(positions) }).activeEnteredZones().isEmpty());
    }
}

void GeoZonePlanningTest::_testRouteFailures()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    // A wall of 20 km across the leg, and a zone around the start
    const GeoZoneGeometry::LocalProjection projection(center);
    QList<QGeoCoordinate> wall;
    for (const QPointF &point : { QPointF(-100, -10000), QPointF(100, -10000), QPointF(100, 10000), QPointF(-100, 10000) }) {
        wall.append(projection.toGeo(point));
    }
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("Wall"), QStringLiteral("PROHIBITED"), wall),
        _circleFeature(QStringLiteral("Start"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 90), 300) }), errorString), qPrintable(errorString));
    GeoZoneRouteSearch search(manager.catalog(), GeoZonePolicy::routing(QDateTime(), QDateTime()));
    search.setTerrainLookup([](const QGeoCoordinate &) { return 0.0; });
    const auto failureOf = [](const GeoZoneRouteSearch::Result &result, GeoZoneRouteSearch::Kind kind) {
        for (const GeoZoneRouteSearch::Route &route : result.routes) {
            if (route.kind == kind) {
                return route;
            }
        }
        return GeoZoneRouteSearch::Route();
    };

    // Going around the wall is longer than the limit, three times the leg or 3 km more
    GeoZoneRouteSearch::Leg leg{ GeoZone::Position{ projection.toGeo(QPointF(-500, 0)), 50 }, GeoZone::Position{ projection.toGeo(QPointF(500, 0)), 50 } };
    GeoZoneRouteSearch::Result result = search.plan(leg);
    QVERIFY(!result.best());
    GeoZoneRouteSearch::Route detour = failureOf(result, GeoZoneRouteSearch::Kind::Detour);
    QCOMPARE(detour.failure, GeoZoneRouteSearch::Failure::TooLong);
    QCOMPARE(detour.zones, QStringList{ QStringLiteral("Wall") });
    QVERIFY(_near(detour.limit, 4000, 1));
    QVERIFY2(detour.length > 20000, qPrintable(QString::number(detour.length)));
    QCOMPARE(failureOf(result, GeoZoneRouteSearch::Kind::Climb).failure, GeoZoneRouteSearch::Failure::NoCeiling);
    QCOMPARE(failureOf(result, GeoZoneRouteSearch::Kind::Descent).failure, GeoZoneRouteSearch::Failure::ReachesGround);
    // Unless the leg is long enough
    GeoZoneRouteSearch::Settings settings;
    settings.maxDetourFactor = 30;
    search.setSettings(settings);
    QVERIFY(search.plan(leg).best());
    search.setSettings(GeoZoneRouteSearch::Settings());

    // From inside a zone
    leg.from = GeoZone::Position{ center.atDistanceAndAzimuth(5000, 90), 50 };
    leg.to = GeoZone::Position{ center.atDistanceAndAzimuth(5000, 90).atDistanceAndAzimuth(2000, 0), 50 };
    detour = failureOf(search.plan(leg), GeoZoneRouteSearch::Kind::Detour);
    QCOMPARE(detour.failure, GeoZoneRouteSearch::Failure::StartInside);
    QCOMPARE(detour.zones, QStringList{ QStringLiteral("Start") });
    std::swap(leg.from, leg.to);
    QCOMPARE(failureOf(search.plan(leg), GeoZoneRouteSearch::Kind::Detour).failure, GeoZoneRouteSearch::Failure::EndInside);

    // Without altitudes, only a detour
    leg = GeoZoneRouteSearch::Leg{ GeoZone::Position{ center.atDistanceAndAzimuth(5000, 270) }, GeoZone::Position{ center.atDistanceAndAzimuth(5000, 90) } };
    leg.from.coordinate = leg.from.coordinate.atDistanceAndAzimuth(3000, 0);
    leg.to.coordinate = leg.to.coordinate.atDistanceAndAzimuth(3000, 0);
    result = search.plan(leg);
    QCOMPARE(failureOf(result, GeoZoneRouteSearch::Kind::Climb).failure, GeoZoneRouteSearch::Failure::AltitudeUnknown);

    // Cancelled before or during the search
    GeoZoneCancelToken cancel;
    cancel.cancel();
    search.setCancelToken(cancel);
    result = search.plan(leg);
    QVERIFY(result.cancelled);
    QVERIFY(result.routes.isEmpty());
    QCOMPARE(GeoZoneRouteSearch::detourAround(leg.from.coordinate, leg.to.coordinate, manager.catalog().search(QStringLiteral("Wall"), 1), settings, cancel).failure,
             GeoZoneRouteSearch::Failure::Cancelled);
}

void GeoZonePlanningTest::_testProfileBands()
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
    GeoZoneMissionPath::Segment west;
    west.from = center.atDistanceAndAzimuth(1000, 270);
    west.to = center;
    west.distance = 1000;
    west.terrainSpacing = 100;
    west.terrainAMSL = QList<double>(11, 400);
    GeoZoneMissionPath::Segment east = west;
    east.from = center;
    east.to = center.atDistanceAndAzimuth(1000, 90);
    GeoZoneMissionPath::Segment gap;
    gap.distance = 250;
    GeoZoneMissionPath::Segment beyond;
    beyond.from = center.atDistanceAndAzimuth(1000, 90);
    beyond.to = center.atDistanceAndAzimuth(1300, 90);
    beyond.distance = 300;

    const QVariantList bands = GeoZoneProfile::bandsForSegments({ west, east, gap, beyond }, manager.catalog());
    // Bands with unbounded limits compare equal, so recomputing unchanged bands doesn't repaint
    QCOMPARE(GeoZoneProfile::computeBands({ west, east, gap, beyond }, manager.catalog()), GeoZoneProfile::computeBands({ west, east, gap, beyond }, manager.catalog()));
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
    const QVariantList unknownTerrain = GeoZoneProfile::bandsForSegments({ beyond }, manager.catalog());
    QCOMPARE(unknownTerrain.count(), 1);
    beyond.from = center.atDistanceAndAzimuth(100, 270);
    beyond.to = center.atDistanceAndAzimuth(100, 90);
    const QVariantList noTerrain = GeoZoneProfile::bandsForSegments({ beyond }, manager.catalog());
    QCOMPARE(noTerrain.count(), 1);
    QVERIFY(qIsNaN(value(noTerrain[0].toMap()[QStringLiteral("points")].toList().first(), "ceiling")));

    // Long paths are sampled more sparsely up to their end
    QList<GeoZoneMissionPath::Segment> longPath;
    for (int i = 0; i < 60; i++) {
        GeoZoneMissionPath::Segment leg;
        leg.from = center.atDistanceAndAzimuth(i * 3000, 90);
        leg.to = center.atDistanceAndAzimuth((i + 1) * 3000, 90);
        leg.distance = 3000;
        longPath.append(leg);
    }
    const QVariantList longBands = GeoZoneProfile::bandsForSegments(longPath, manager.catalog());
    const auto end = std::find_if(longBands.cbegin(), longBands.cend(), [](const QVariant &band) {
        return band.toMap()[QStringLiteral("name")].toString() == QStringLiteral("END");
    });
    QVERIFY(end != longBands.cend());
    const QVariantList endPoints = end->toMap()[QStringLiteral("points")].toList();
    const double longSpacing = 180000.0 / GeoZoneProfile::maxSamples;
    QVERIFY(qAbs(value(endPoints.first(), "distance") - 178700) <= longSpacing + 1);
    QVERIFY(qAbs(value(endPoints.last(), "distance") - 179300) <= longSpacing + 1);
}

void GeoZonePlanningTest::_testGeoFenceImport()
{
    const GeoZoneParser::Result ed269 = _parseResource(QStringLiteral("ed269_test.json"));
    const GeoZoneParser::Result ed318 = _parseResource(QStringLiteral("ed318_test.json"));
    QCOMPARE(ed269.zones.count(), 3);
    QCOMPARE(ed318.zones.count(), 3);

    ZoneOwner zoneParent;
    const QList<const GeoZone*> zones = {
        zoneParent.add(ed269.zones[0]),   // One polygon after merging identical footprints
        zoneParent.add(ed269.zones[1]),   // Circle
        zoneParent.add(ed318.zones[1]),   // Two polygons
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
    QCOMPARE(GeoZoneFenceTag::find(polygon)->name(), zoneName(zones[0]));
    QVERIFY(GeoZoneFenceTag::isGeoZone(polygon));
    QCOMPARE(GeoZoneFenceTag::find(circle)->name(), zoneName(zones[1]));
    QVERIFY(GeoZoneFenceTag::isGeoZone(circle));

    // Which the plan file keeps in the entries of the fences, while drawn fences have no tags there
    geoFenceController->addInclusionCircle(QGeoCoordinate(47.1, 7.9), QGeoCoordinate(46.9, 8.1));
    (void) GeoZoneFenceTag::of(geoFenceController->circles()->get(1));
    const QJsonObject planJson = masterController.saveToJson().object();
    const QJsonObject fenceJson = planJson[PlanMasterController::kJsonGeoFenceObjectKey].toObject();
    const QJsonObject polygonJson = fenceJson[QStringLiteral("polygons")].toArray()[0].toObject();
    QCOMPARE(polygonJson[QStringLiteral("name")].toString(), zoneName(zones[0]));
    QVERIFY(polygonJson[QStringLiteral("geoZone")].toBool());
    const QJsonArray circlesJson = fenceJson[QStringLiteral("circles")].toArray();
    QCOMPARE(circlesJson.count(), 2);
    QCOMPARE(circlesJson[0].toObject()[QStringLiteral("name")].toString(), zoneName(zones[1]));
    QVERIFY(circlesJson[0].toObject()[QStringLiteral("geoZone")].toBool());
    QVERIFY(!circlesJson[1].toObject().contains(QStringLiteral("name")) && !circlesJson[1].toObject().contains(QStringLiteral("geoZone")));
    geoFenceController->deleteCircle(1);

    // Loading the plan restores the tags and leaves the fence clean, while fences without them load untagged
    PlanMasterController loadedController;
    loadedController.setFlyView(false);
    loadedController.start();
    GeoFenceController *const loadedFence = loadedController.geoFenceController();
    QString errorString;
    QVERIFY2(loadedFence->load(fenceJson, errorString), qPrintable(errorString));
    loadedFence->setDirty(false);
    GeoZonePlanData::load(&loadedController, planJson);
    QCOMPARE(loadedFence->polygons()->count(), 3);
    QCOMPARE(GeoZoneFenceTag::find(loadedFence->polygons()->get(0))->name(), zoneName(zones[0]));
    QVERIFY(GeoZoneFenceTag::isGeoZone(loadedFence->circles()->get(0)));
    QVERIFY(!GeoZoneFenceTag::find(loadedFence->circles()->get(1)));
    QVERIFY(!loadedFence->dirty());
    QJsonObject untaggedPolygon = polygonJson;
    untaggedPolygon.remove(QStringLiteral("name"));
    untaggedPolygon.remove(QStringLiteral("geoZone"));
    QJsonArray untaggedPolygons = fenceJson[QStringLiteral("polygons")].toArray();
    untaggedPolygons[0] = untaggedPolygon;
    QJsonObject untaggedFence = fenceJson;
    untaggedFence[QStringLiteral("polygons")] = untaggedPolygons;
    QJsonObject untaggedPlan = planJson;
    untaggedPlan[PlanMasterController::kJsonGeoFenceObjectKey] = untaggedFence;
    QVERIFY2(loadedFence->load(untaggedFence, errorString), qPrintable(errorString));
    GeoZonePlanData::load(&loadedController, untaggedPlan);
    QVERIFY(!GeoZoneFenceTag::find(loadedFence->polygons()->get(0)));
    QVERIFY(GeoZoneFenceTag::isGeoZone(loadedFence->polygons()->get(1)));

    // Importing the same zones again does not duplicate fences
    QCOMPARE(importer.addExclusions(zones), 0);
    QCOMPARE(geoFenceController->polygons()->count(), 3);
    QCOMPARE(geoFenceController->circles()->count(), 1);

    // Deleting the fences from geozones keeps the drawn ones, in one reset per list
    geoFenceController->setDirty(false);
    geoFenceController->addInclusionCircle(QGeoCoordinate(47.1, 7.9), QGeoCoordinate(46.9, 8.1));
    QCOMPARE(geoFenceController->circles()->count(), 2);
    QSignalSpy resetSpy(geoFenceController->polygons(), &QAbstractItemModel::modelReset);
    GeoZoneFenceTags::deleteFences(geoFenceController, true);
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(geoFenceController->polygons()->count(), 0);
    QCOMPARE(geoFenceController->circles()->count(), 1);
    QVERIFY(!GeoZoneFenceTag::isGeoZone(geoFenceController->circles()->get(0)));
    QVERIFY(geoFenceController->dirty());
    GeoZoneFenceTags::deleteFences(geoFenceController, false);
    QCOMPARE(geoFenceController->circles()->count(), 0);
}

void GeoZonePlanningTest::_testGeoFenceSelection()
{
    // Two circular zones published as 40 vertex polygons, and a circle
    ZoneOwner zoneParent;
    QList<const GeoZone*> zones;
    for (int i = 0; i < 2; i++) {
        GeoZone::Volume volume;
        volume.rings.append(GeoZone::toRing(_circleRing(QGeoCoordinate(47, 8 + (i * 0.1)), 500, 40)));
        GeoZone::Data data;
        data.identifier = QStringLiteral("Z%1").arg(i);
        data.restriction = GeoZone::Prohibited;
        data.volumes.append(volume);
        zones.append(zoneParent.add(data));
    }
    GeoZone::Volume circleVolume;
    circleVolume.isCircle = true;
    circleVolume.center = QGeoCoordinate(47, 8.2);
    circleVolume.radius = 300;
    GeoZone::Data circleData;
    circleData.identifier = QStringLiteral("C");
    circleData.restriction = GeoZone::Prohibited;
    circleData.volumes.append(circleVolume);
    zones.append(zoneParent.add(circleData));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);
    QVERIFY(!importer.canUpload());

    // Without a vehicle the fence fits PX4 and ArduPilot copters, with the circle as a polygon. 80 polygon points don't.
    QSignalSpy selectionSpy(&importer, &GeoZoneFenceImporter::selectionChanged);
    importer.select(zones);
    QCOMPARE(selectionSpy.count(), 1);
    QCOMPARE(importer.capacity().maxItems, GeoZoneFences::Capacity::px4MaxItems);
    QVERIFY(!importer.capacity().circles);
    QCOMPARE(importer.selectedCount(), 2);
    QVariantMap capacity = importer.capacityInfo();
    QVERIFY(capacity[QStringLiteral("estimated")].toBool());
    QVERIFY(capacity[QStringLiteral("text")].toString().startsWith(QStringLiteral("Vehicle fence: %1 of 64 points").arg(capacity[QStringLiteral("usedItems")].toInt())));
    QVERIFY(capacity[QStringLiteral("note")].toString().contains(QStringLiteral("No vehicle")));
    QVariantList choices = importer.choicesInfo();
    QCOMPARE(choices.count(), 3);
    QVERIFY(choices[0].toMap()[QStringLiteral("selected")].toBool());
    QVERIFY(!choices[2].toMap()[QStringLiteral("selected")].toBool());
    QCOMPARE(choices[2].toMap()[QStringLiteral("reason")].toString(), QStringLiteral("No room left in the vehicle fence"));

    // Simplified, all of them fit
    importer.select(zones, 25);
    QCOMPARE(importer.selectedCount(), 3);
    QVERIFY(importer.capacity().fits(importer.selection().usage));

    // Drawn fences and the breach return point are kept and take room, fences added from geozones before are replaced
    geoFenceController->addInclusionCircle(QGeoCoordinate(47.1, 7.9), QGeoCoordinate(46.9, 8.3));
    geoFenceController->setBreachReturnPoint(QGeoCoordinate(47, 7.95));
    QCOMPARE(importer.addExclusions({ zones[0] }), 1);
    QCOMPARE(GeoZoneFenceImporter::keptUsage(geoFenceController).items, 2);
    importer.select(zones, 25);
    QCOMPARE(importer.capacityInfo()[QStringLiteral("keptItems")].toInt(), 2);
    QCOMPARE(importer.applySelection(), 3);
    QCOMPARE(geoFenceController->polygons()->count(), 3);
    QCOMPARE(geoFenceController->circles()->count(), 1);
    QCOMPARE(importer.applySelection(), 3);
    QCOMPARE(geoFenceController->polygons()->count(), 3);
    int points = 2;
    for (int i = 0; i < geoFenceController->polygons()->count(); i++) {
        QVERIFY(GeoZoneFenceTag::isGeoZone(geoFenceController->polygons()->get(i)));
        points += geoFenceController->polygons()->value<QGCFencePolygon*>(i)->count();
    }
    QCOMPARE(points, importer.selection().usage.items);
    // The mission check finds the zones excluded by the fences of the selection
    for (const GeoZone *zone : std::as_const(zones)) {
        QVERIFY(GeoZoneMissionCheck::fenceExcludes(geoFenceController, zone));
    }

    // Zones left out by the user
    importer.setZoneIncluded(zones[0]->key(), false);
    QCOMPARE(importer.selectedCount(), 2);
    choices = importer.choicesInfo();
    const QVariantMap leftOut = choices.last().toMap();
    QCOMPARE(leftOut[QStringLiteral("key")].toString(), zones[0]->key());
    QVERIFY(!leftOut[QStringLiteral("included")].toBool());
    QCOMPARE(leftOut[QStringLiteral("reason")].toString(), QStringLiteral("Left out"));
    importer.setZoneIncluded(zones[0]->key(), true);
    QCOMPARE(importer.selectedCount(), 3);
}

void GeoZonePlanningTest::_testGeoFenceUpload()
{
    ZoneOwner zoneParent;
    GeoZone::Volume polygonVolume;
    polygonVolume.rings.append(GeoZone::toRing(_circleRing(QGeoCoordinate(47, 8), 500, 40)));
    GeoZone::Data polygonData;
    polygonData.identifier = QStringLiteral("P");
    polygonData.restriction = GeoZone::Prohibited;
    polygonData.volumes.append(polygonVolume);
    GeoZone::Volume circleVolume;
    circleVolume.isCircle = true;
    circleVolume.center = QGeoCoordinate(47, 8.05);
    circleVolume.radius = 300;
    GeoZone::Data circleData;
    circleData.identifier = QStringLiteral("C");
    circleData.restriction = GeoZone::ReqAuthorisation;
    circleData.volumes.append(circleVolume);
    const QList<const GeoZone*> zones = { zoneParent.add(polygonData), zoneParent.add(circleData) };

    for (const MAV_AUTOPILOT autopilot : { MAV_AUTOPILOT_PX4, MAV_AUTOPILOT_ARDUPILOTMEGA }) {
        _connectMockLink(autopilot);
        Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
        QVERIFY(vehicle);
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->initialPlanRequestComplete(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->parameterManager()->parametersReady(), 30000);

        PlanMasterController masterController;
        masterController.setFlyView(false);
        masterController.start();
        QCOMPARE(masterController.managerVehicle(), vehicle);
        GeoFenceController *const geoFenceController = masterController.geoFenceController();
        GeoZoneFenceImporter importer;
        importer.setGeoFenceController(geoFenceController);
        QVERIFY(importer.canUpload());

        // PX4 stores 64 points, the MockLink ArduCopter is estimated with 16 KB storage. Both store circles.
        importer.select(zones, 25);
        const GeoZoneFences::Capacity capacity = importer.capacity();
        QVERIFY(capacity.circles);
        if (autopilot == MAV_AUTOPILOT_PX4) {
            QCOMPARE(capacity.maxItems, GeoZoneFences::Capacity::px4MaxItems);
            QVERIFY(!capacity.estimated);
            QVERIFY(importer.capacityInfo()[QStringLiteral("warning")].toString().isEmpty());
        } else {
            QCOMPARE(capacity.storageBytes, GeoZoneFences::Capacity::arduPilotCopterBytes);
            QVERIFY(capacity.estimated);
            QVERIFY(importer.capacityInfo()[QStringLiteral("text")].toString().contains(QStringLiteral("of 560 bytes")));
            // The MockLink ArduCopter has its fence disabled
            QVERIFY(importer.capacityInfo()[QStringLiteral("warning")].toString().contains(QStringLiteral("FENCE_ENABLE")));
        }
        QCOMPARE(importer.selectedCount(), 2);
        geoFenceController->setBreachReturnPoint(QGeoCoordinate(47, 7.95, 50));

        // The fence is sent and read back
        importer.uploadToVehicle();
        QCOMPARE(importer.uploadState(), GeoZoneFenceImporter::Uploading);
        QTRY_COMPARE_WITH_TIMEOUT(importer.uploadState(), GeoZoneFenceImporter::UploadVerified, 15000);
        QVERIFY2(importer.uploadMessage().startsWith(QStringLiteral("Verified")), qPrintable(importer.uploadMessage()));
        GeoFenceManager *const manager = vehicle->geoFenceManager();
        QCOMPARE(manager->polygons().count(), 1);
        QCOMPARE(manager->circles().count(), 1);
        QVERIFY(!manager->polygons().first().inclusion());
        QCOMPARE(manager->polygons().first().count(), importer.selection().choices.first().fences.polygons.first().count());
        // The plan keeps its fences from geozones
        QCOMPARE(geoFenceController->polygons()->count(), 1);
        QVERIFY(GeoZoneFenceTag::isGeoZone(geoFenceController->polygons()->get(0)));
        QVERIFY(!geoFenceController->dirty());

        // A vehicle without room for the fence rejects it
        _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailWriteRequest0ErrorAck, MAV_MISSION_NO_SPACE);
        importer.uploadToVehicle();
        QTRY_COMPARE_WITH_TIMEOUT(importer.uploadState(), GeoZoneFenceImporter::UploadFailed, 15000);
        QVERIFY2(importer.uploadMessage().contains(QStringLiteral("no room")), qPrintable(importer.uploadMessage()));
        _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailNone, MAV_MISSION_ACCEPTED);

        _disconnectMockLink();
        QTRY_VERIFY(!importer.canUpload());
        importer.uploadToVehicle();
        QCOMPARE(importer.uploadState(), GeoZoneFenceImporter::UploadFailed);
    }

    // Fences read back are compared within the tolerance, in any order
    GeoZoneFenceImporter::FenceSnapshot sent;
    sent.polygons.append({ false, _circleRing(QGeoCoordinate(47, 8), 500, 8) });
    sent.polygons.append({ true, _circleRing(QGeoCoordinate(47, 8), 5000, 4) });
    sent.circles.append({ false, QGeoCircle(QGeoCoordinate(47, 8.05), 300) });
    sent.returnPoint = QGeoCoordinate(47, 7.95, 50);
    GeoZoneFenceImporter::FenceSnapshot received = sent;
    std::swap(received.polygons[0], received.polygons[1]);
    received.circles[0].circle.setRadius(300.1);
    received.returnPoint.setAltitude(0);
    QVERIFY(GeoZoneFenceImporter::compareFences(sent, received).isEmpty());
    received.circles[0].circle.setRadius(302);
    QVERIFY(GeoZoneFenceImporter::compareFences(sent, received).contains(QStringLiteral("Fences differing from the plan: 1.")));
    received.circles.clear();
    QVERIFY(GeoZoneFenceImporter::compareFences(sent, received).contains(QStringLiteral("0 circles")));
    received = sent;
    received.returnPoint = QGeoCoordinate();
    QVERIFY(!GeoZoneFenceImporter::compareFences(sent, received).isEmpty());
}

void GeoZonePlanningTest::_testMapModel()
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
    QCOMPARE(model.labelZones().count(), 0);

    GeoZoneMapItem *const polygonItem = model.polygons()->value<GeoZoneMapItem*>(0);
    const qsizetype lowDetailCount = polygonItem->mercatorRings().first().count();
    QVERIFY(lowDetailCount < 2000);

    // Zooming in keeps the items and adds detail. Only zones centered in the view are labelled.
    model.setView(view, 16, true);
    QCOMPARE(model.polygons()->value<GeoZoneMapItem*>(0), polygonItem);
    QCOMPARE(model.labelZones().count(), 2);
    QVERIFY(polygonItem->mercatorRings().first().count() > lowDetailCount);

    // Another file adds items, removing a source removes its items
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("other.json"),
        { _circleFeature(QStringLiteral("OTHER"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.0, 8.0), 1000) }), errorString), qPrintable(errorString));
    QCOMPARE(model.polygons()->count(), 1);
    QVERIFY(identifiers(model.circles()).contains(QStringLiteral("OTHER")));
    // Items of the other sources are kept when a source reloads
    QCOMPARE(model.polygons()->value<GeoZoneMapItem*>(0), polygonItem);
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("other.json"),
        { _circleFeature(QStringLiteral("OTHER"), QStringLiteral("CONDITIONAL"), QGeoCoordinate(47.0, 8.0), 1000) }), errorString), qPrintable(errorString));
    QCOMPARE(model.polygons()->value<GeoZoneMapItem*>(0), polygonItem);
    QVERIFY(identifiers(model.circles()).contains(QStringLiteral("OTHER")));
    manager.removeSource(0);
    QCOMPARE(model.polygons()->count(), 0);
    QCOMPARE(identifiers(model.circles()), QStringList({ QStringLiteral("OTHER") }));

    // Restrictions hidden on maps leave out their shapes and labels
    const auto restoreRestrictions = qScopeGuard([&manager]() { manager.display()->setRestrictionVisible(GeoZone::Conditional, true); });
    QVERIFY(!model.labelZones().isEmpty());
    manager.display()->setRestrictionVisible(GeoZone::Conditional, false);
    QVERIFY(!manager.display()->restrictionVisible(GeoZone::Conditional));
    QCOMPARE(model.circles()->count(), 0);
    QVERIFY(model.labelZones().isEmpty());
    manager.display()->setRestrictionVisible(GeoZone::Conditional, true);
    QCOMPARE(identifiers(model.circles()), QStringList({ QStringLiteral("OTHER") }));

    model.setView(QGeoRectangle(), 16, true);
    QCOMPARE(model.circles()->count(), 0);
    QCOMPARE(model.labelZones().count(), 0);

    model.setView(view, 16, true);
    manager.clear();
    QCOMPARE(model.circles()->count(), 0);
}

void GeoZonePlanningTest::_testMapModelShapeLimit()
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
    QVERIFY(model.labelZones().count() > 0);
    for (int i = 0; i < model.labelZones().count(); i++) {
        QVERIFY(GeoZoneInfo::fromVariant(model.labelZones().at(i))->identifier().mid(1).toInt() >= extraZones);
    }
}
