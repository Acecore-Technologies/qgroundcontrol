/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRouteTest.h"
#include "GeoZoneRouteSearch.h"
#include "GeoZoneTestHelpers.h"

#include <QtCore/QElapsedTimer>

using namespace GeoZoneTestHelpers;

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

void GeoZoneRouteTest::_testConcave_data()
{
    QTest::addColumn<QList<QPointF>>("ring");
    QTest::addColumn<QList<QPointF>>("hole");
    QTest::addColumn<QPointF>("start");
    QTest::addColumn<QPointF>("end");
    QTest::addColumn<int>("failure");
    QTest::addColumn<int>("minWaypoints");
    QTest::addColumn<double>("maxLength");   ///< Meters, 0 for no limit
    QTest::addColumn<double>("clearance");   ///< Meters the path keeps from the zone at least

    using Failure = GeoZoneRouteSearch::Failure;
    // A notch wider than twice the margin can be entered, the corner of the U's arm is passed at the margin. Convex
    // hulls would block it.
    QTest::newRow("into wide notch") << _uShape(1500, 1500, 600, 1000) << QList<QPointF>() << QPointF(1500, 2500) << QPointF(0, 0)
                                     << int(Failure::None) << 1 << 3000.0 << 99.0;
    // A notch narrower than twice the margin is passed around, not through
    QTest::newRow("across narrow notch") << _uShape(1500, 1500, 120, 1000) << QList<QPointF>() << QPointF(-2000, 300) << QPointF(2000, 300)
                                         << int(Failure::None) << 2 << 5200.0 << 99.0;
    // Around the closed side of a U opening towards the start, not into the notch
    QTest::newRow("around U") << _uShape(1500, 1500, 600, 1000) << QList<QPointF>() << QPointF(0, 2000) << QPointF(0, -2000)
                              << int(Failure::None) << 2 << 5200.0 << 99.0;
    // Inside a hole the zone is open, so the leg needs no detour, but it surrounds the hole
    QTest::newRow("within hole") << _square(2000) << _square(1500) << QPointF(-1000, -1000) << QPointF(1000, 1000) << int(Failure::None) << 0 << 0.0
                                 << 99.0;
    QTest::newRow("into hole") << _square(2000) << _square(1500) << QPointF(-3000, 0) << QPointF(0, 0) << int(Failure::Enclosed) << 0 << 0.0 << 0.0;
    QTest::newRow("start inside") << _square(500) << QList<QPointF>() << QPointF(0, 0) << QPointF(2000, 0) << int(Failure::StartInside) << 0 << 0.0 << 0.0;
    QTest::newRow("end inside") << _square(500) << QList<QPointF>() << QPointF(-2000, 0) << QPointF(100, 100) << int(Failure::EndInside) << 0 << 0.0 << 0.0;
    // Detailed outlines are simplified outward, but not near the ends: the start is outside the bay of the east side,
    // which the simplified outline closes
    QList<QPointF> bay = { { -500, -500 } };
    for (int i = 0; i <= 30; i++) {
        bay.append(QPointF(500 - (20 * std::sin(i * M_PI / 30)), -500 + (i * 1000.0 / 30)));
    }
    bay.append(QPointF(-500, 500));
    QTest::newRow("start near detailed") << bay << QList<QPointF>() << QPointF(490, 0) << QPointF(-2000, 0) << int(Failure::None) << 1 << 0.0 << 8.0;
}

void GeoZoneRouteTest::_testConcave()
{
    QFETCH(QList<QPointF>, ring);
    QFETCH(QList<QPointF>, hole);
    QFETCH(QPointF, start);
    QFETCH(QPointF, end);
    QFETCH(int, failure);
    QFETCH(int, minWaypoints);
    QFETCH(double, maxLength);
    QFETCH(double, clearance);

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
    const double pathClearance = _pathClearance(path, { zone });
    QVERIFY2(pathClearance > clearance, qPrintable(QString::number(pathClearance)));
    QVERIFY(_pathClearance(path.mid(1, path.count() - 2), { zone }) > (settings.margin - 1));
    if (maxLength > 0) {
        QVERIFY2(route.length < maxLength, qPrintable(QString::number(route.length)));
    }
    QVERIFY(route.statistics.rounds >= 1);
}

void GeoZoneRouteTest::_testVertical_data()
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

void GeoZoneRouteTest::_testVertical()
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
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ feature }), errorString), qPrintable(errorString));

    const GeoZonePathEvaluator::TerrainLookup lookup = [&center, terrain, hill](const QGeoCoordinate &coordinate) {
        return (!hill || (coordinate.distanceTo(center) < 600)) ? terrain : 0.0;
    };
    const GeoZonePolicy policy = GeoZonePolicy::routing(QDateTime(), QDateTime());
    GeoZoneRouteSearch search(zones.catalog(), policy);
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
        GeoZonePathEvaluator evaluator(zones.catalog(), policy);
        evaluator.setTerrainLookup(lookup);
        QList<GeoZone::Position> positions;
        if (route.kind == GeoZoneRouteSearch::Kind::Detour) {
            for (const QGeoCoordinate &coordinate : _routePath(leg.from.coordinate, route, leg.to.coordinate)) {
                positions.append(GeoZone::Position{ coordinate, 50 });
            }
            QVERIFY(_pathClearance(_routePath(leg.from.coordinate, route, leg.to.coordinate), zones.catalog().search(QStringLiteral("Z"), 1), 20) > 99);
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

void GeoZoneRouteTest::_testFailures()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    // A wall of 20 km across the leg, and a zone around the start
    const GeoZoneGeometry::LocalProjection projection(center);
    QList<QGeoCoordinate> wall;
    for (const QPointF &point : { QPointF(-100, -10000), QPointF(100, -10000), QPointF(100, 10000), QPointF(-100, 10000) }) {
        wall.append(projection.toGeo(point));
    }
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("Wall"), QStringLiteral("PROHIBITED"), wall),
        _circleFeature(QStringLiteral("Start"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 90), 300) }), errorString), qPrintable(errorString));
    GeoZoneRouteSearch search(zones.catalog(), GeoZonePolicy::routing(QDateTime(), QDateTime()));
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
    QCOMPARE(GeoZoneRouteSearch::detourAround(leg.from.coordinate, leg.to.coordinate, zones.catalog().search(QStringLiteral("Wall"), 1), settings, cancel).failure,
             GeoZoneRouteSearch::Failure::Cancelled);
}

void GeoZoneRouteTest::_testUnknownAltitude()
{
    // A zone up to 100 m AMSL across a leg, far below the altitude known at one end of it
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _withLimits(_circleFeature(QStringLiteral("Low"), QStringLiteral("PROHIBITED"), center, 300), 0, QStringLiteral("AMSL"), 100, QStringLiteral("AMSL")) }), errorString),
             qPrintable(errorString));
    const GeoZoneRouteSearch search(zones.catalog(), GeoZonePolicy::routing(QDateTime(), QDateTime()));
    const QGeoCoordinate west = center.atDistanceAndAzimuth(2000, 270);
    const QGeoCoordinate east = center.atDistanceAndAzimuth(2000, 90);

    // The altitude of the leg is unknown whichever end it is unknown at: the zone is an obstacle from the first search of
    // the detour, not only once a path through it was found to enter it
    const GeoZoneRouteSearch::Route unknownStart = search.detour(GeoZoneRouteSearch::Leg{ GeoZone::Position{ west }, GeoZone::Position{ east, 500 } });
    const GeoZoneRouteSearch::Route unknownEnd = search.detour(GeoZoneRouteSearch::Leg{ GeoZone::Position{ west, 500 }, GeoZone::Position{ east } });
    for (const GeoZoneRouteSearch::Route &route : { unknownStart, unknownEnd }) {
        QVERIFY2(route.isValid(), qPrintable(QString::number(int(route.failure))));
        QVERIFY(!route.waypoints.isEmpty());
        QCOMPARE(route.zones, QStringList{ QStringLiteral("Low") });
        QCOMPARE(route.statistics.obstacles, 1);
        QCOMPARE(route.statistics.rounds, 1);
    }
    QCOMPARE(unknownEnd.waypoints, unknownStart.waypoints);

    // Known at both ends, the leg passes above the zone
    const GeoZoneRouteSearch::Result above = search.plan(GeoZoneRouteSearch::Leg{ GeoZone::Position{ west, 500 }, GeoZone::Position{ east, 500 } });
    QVERIFY(above.crossedZones.isEmpty());

    // A climb over the zone needs the terrain, which isn't known here, only for its maximum height above it
    const GeoZoneRouteSearch::Leg low{ GeoZone::Position{ west, 50 }, GeoZone::Position{ east, 50 } };
    const auto climbOf = [](const GeoZoneRouteSearch::Result &result) {
        for (const GeoZoneRouteSearch::Route &route : result.routes) {
            if (route.kind == GeoZoneRouteSearch::Kind::Climb) {
                return route;
            }
        }
        return GeoZoneRouteSearch::Route();
    };
    GeoZoneRouteSearch unlimited(zones.catalog(), GeoZonePolicy::routing(QDateTime(), QDateTime()));
    QCOMPARE(climbOf(unlimited.plan(low)).failure, GeoZoneRouteSearch::Failure::TerrainUnknown);
    GeoZoneRouteSearch::Settings settings;
    settings.maxAltitudeAGL = qQNaN();
    unlimited.setSettings(settings);
    const GeoZoneRouteSearch::Route climb = climbOf(unlimited.plan(low));
    QVERIFY2(climb.isValid(), qPrintable(QString::number(int(climb.failure))));
    QCOMPARE(climb.kind, GeoZoneRouteSearch::Kind::Climb);
    QVERIFY(_near(climb.altitude, 100 + settings.verticalMargin, 0.01));
    QVERIFY(_near(climb.fromAltitude, climb.altitude, 0.01));
}

void GeoZoneRouteTest::_testSettings()
{
    const QGeoCoordinate center(47, 8);
    ZoneOwner owner;
    const GeoZoneGeometry::LocalProjection projection(center);
    const GeoZone *const zone = _localPolygonZone(owner, projection, QStringLiteral("Z"), _square(300));
    const QGeoCoordinate west = center.atDistanceAndAzimuth(2000, 270);
    const QGeoCoordinate east = center.atDistanceAndAzimuth(2000, 90);

    // Without a margin, with a negative one or one which isn't a number, the route must not go straight through the zone
    const GeoZoneRouteSearch::Settings defaults;
    for (const double margin : { 0.0, GeoZoneRouteSearch::Settings::minMargin / 1000, -50.0, qQNaN(), qInf() }) {
        GeoZoneRouteSearch::Settings settings;
        settings.margin = margin;
        const double expected = (margin >= 0) && std::isfinite(margin) ? GeoZoneRouteSearch::Settings::minMargin : defaults.margin;
        QCOMPARE(settings.validated().margin, expected);
        const GeoZoneRouteSearch::Route route = GeoZoneRouteSearch::detourAround(west, east, { zone }, settings);
        QVERIFY2(route.isValid(), qPrintable(QString::number(margin)));
        QVERIFY2(!route.waypoints.isEmpty(), qPrintable(QString::number(margin)));
        const double clearance = _pathClearance(_routePath(west, route, east), { zone }, 2);
        QVERIFY2(clearance > (expected - 0.1), qPrintable(QStringLiteral("%1: %2").arg(margin).arg(clearance)));
    }

    // The other settings which a search can't work with are replaced by the defaults, the valid ones kept
    GeoZoneRouteSearch::Settings settings;
    settings.margin = 40;
    settings.verticalMargin = -1;
    settings.minGroundClearance = qQNaN();
    settings.maxAltitudeAGL = -5;
    settings.maxDetourFactor = 0;
    settings.minDetourAllowance = -1;
    settings.climbWeight = qQNaN();
    settings.descentWeight = -3;
    settings.sampleSpacing = 0;
    settings.maxSearchWork = 0;
    const GeoZoneRouteSearch::Settings valid = settings.validated();
    QCOMPARE(valid.margin, 40.0);
    QCOMPARE(valid.verticalMargin, defaults.verticalMargin);
    QCOMPARE(valid.minGroundClearance, defaults.minGroundClearance);
    QCOMPARE(valid.maxAltitudeAGL, defaults.maxAltitudeAGL);
    QCOMPARE(valid.maxDetourFactor, defaults.maxDetourFactor);
    QCOMPARE(valid.minDetourAllowance, defaults.minDetourAllowance);
    QCOMPARE(valid.climbWeight, defaults.climbWeight);
    QCOMPARE(valid.descentWeight, defaults.descentWeight);
    QCOMPARE(valid.sampleSpacing, defaults.sampleSpacing);
    QCOMPARE(valid.maxSearchWork, defaults.maxSearchWork);
    // Climbs without a limit above the terrain stay without one
    settings.maxAltitudeAGL = qQNaN();
    QVERIFY(qIsNaN(settings.validated().maxAltitudeAGL));

    // A search validates its settings
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    GeoZoneRouteSearch search(zones.catalog(), GeoZonePolicy::routing(QDateTime(), QDateTime()));
    settings.margin = 0;
    search.setSettings(settings);
    QCOMPARE(search.settings().margin, GeoZoneRouteSearch::Settings::minMargin);
}

void GeoZoneRouteTest::_testMaze()
{
    // A leg of 20 km through a row of a maze of 1200 zones, whose detour took a minute of a debug build when A* tested the
    // edges from each node to all others, with 4 million visibility tests
    const QGeoCoordinate southWest(46.5, 7.5);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("maze.json"), _mazeFeatures(southWest, 40, 30, 420, 7)), errorString), qPrintable(errorString));
    QCOMPARE(zones.catalog().zones().count(), 1200);
    const GeoZoneGeometry::LocalProjection projection(southWest);
    const GeoZoneRouteSearch::Leg leg{ GeoZone::Position{ projection.toGeo(QPointF(-1000, 6090)), 50 },
                                       GeoZone::Position{ projection.toGeo(QPointF(19000, 6090)), 50 } };
    const GeoZonePolicy policy = GeoZonePolicy::routing(QDateTime(), QDateTime());
    GeoZoneRouteSearch search(zones.catalog(), policy);
    search.setTerrainLookup([](const QGeoCoordinate &) { return 0.0; });

    QElapsedTimer timer;
    timer.start();
    const GeoZoneRouteSearch::Route detour = search.detour(leg);
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(detour.isValid(), qPrintable(QString::number(int(detour.failure))));
    // Generous for debug builds with sanitizers on a busy machine
    QVERIFY2(elapsed < 30000, qPrintable(QString::number(elapsed)));
    QVERIFY2(detour.statistics.visibilityTests < 1000000, qPrintable(QString::number(detour.statistics.visibilityTests)));
    QVERIFY2(detour.statistics.work < search.settings().maxSearchWork, qPrintable(QString::number(detour.statistics.work)));
    // Through the maze, within 2% of the shortest detour, 24672 m, which a search of all edges finds
    QVERIFY2(detour.length < (24672 * 1.02), qPrintable(QString::number(detour.length)));
    QVERIFY(detour.zones.count() > 10);
    const QList<QGeoCoordinate> path = _routePath(leg.from.coordinate, detour, leg.to.coordinate);
    QList<GeoZone::Position> positions;
    for (const QGeoCoordinate &coordinate : path) {
        positions.append(GeoZone::Position{ coordinate, 50 });
    }
    GeoZonePathEvaluator evaluator(zones.catalog(), policy);
    QVERIFY(evaluator.evaluate({ GeoZonePathEvaluator::Path::fromPositions(positions) }).activeEnteredZones().isEmpty());

    // Searches stop at the limit of their work
    GeoZoneRouteSearch::Settings settings;
    settings.maxSearchWork = 100000;
    search.setSettings(settings);
    const GeoZoneRouteSearch::Route limited = search.detour(leg);
    QCOMPARE(limited.failure, GeoZoneRouteSearch::Failure::SearchLimit);
    QCOMPARE(limited.zones, detour.zones);
    QVERIFY(limited.waypoints.isEmpty());
    QVERIFY2(limited.statistics.work < (2 * (settings.maxSearchWork + 100000)), qPrintable(QString::number(limited.statistics.work)));
}

GEOZONE_TEST(GeoZoneRouteTest)
