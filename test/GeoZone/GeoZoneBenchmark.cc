/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneBenchmark.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZonePolicy.h"
#include "GeoZoneRouteSearch.h"
#include "GeoZoneTestHelpers.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtTest/QTest>

using namespace GeoZoneTestHelpers;

namespace {

constexpr int kZoneCount = 10000;
constexpr int kQueryCount = 1000;   ///< Positions and areas queried in each iteration
constexpr int kMissionWaypoints = 500;
constexpr int kRouteLegs = 50;

// Zones are spread over 10 degrees of latitude and longitude
constexpr double kSouth = 45;
constexpr double kWest = 5;
constexpr double kSize = 10;

const QDateTime kStart(QDate(2026, 6, 15), QTime(10, 0), QTimeZone::UTC);

QGeoCoordinate randomCoordinate(QRandomGenerator &random)
{
    return QGeoCoordinate(kSouth + (random.generateDouble() * kSize), kWest + (random.generateDouble() * kSize));
}

QJsonObject withLimits(QJsonObject feature, double lower, const QString &lowerReference, double upper, const QString &upperReference)
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

/// Circles and irregular polygons of a few hundred meters to a few kilometers, and some large ones crossing many
/// cells of the index, with the usual limits above ground or sea level, or none
QByteArray zonesDocument()
{
    QRandomGenerator random(20261101);
    const QStringList restrictions = { QStringLiteral("PROHIBITED"), QStringLiteral("REQ_AUTHORISATION"), QStringLiteral("CONDITIONAL"),
                                       QStringLiteral("NO_RESTRICTION") };
    QJsonArray features;
    for (int i = 0; i < kZoneCount; i++) {
        const QString identifier = QStringLiteral("Z%1").arg(i);
        const QString restriction = restrictions[random.bounded(static_cast<int>(restrictions.count()))];
        const QGeoCoordinate center = randomCoordinate(random);
        const double radius = (i % 500 == 0) ? (30000 + random.bounded(30000)) : (200 + random.bounded(4800));
        QJsonObject feature;
        if (random.bounded(2) == 0) {
            feature = _circleFeature(identifier, restriction, center, radius);
        } else {
            QList<QGeoCoordinate> ring;
            const int vertexCount = 6 + random.bounded(19);
            for (int vertex = 0; vertex < vertexCount; vertex++) {
                ring.append(center.atDistanceAndAzimuth(radius * (0.6 + (0.4 * random.generateDouble())), vertex * 360.0 / vertexCount));
            }
            feature = _polygonFeature(identifier, restriction, ring);
        }
        switch (random.bounded(4)) {
        case 0:
        case 1:
            feature = withLimits(feature, 0, QStringLiteral("AGL"), 120, QStringLiteral("AGL"));
            break;
        case 2:
            feature = withLimits(feature, 150, QStringLiteral("AGL"), 2000, QStringLiteral("AMSL"));
            break;
        default:
            break;
        }
        feature[QStringLiteral("name")] = QStringLiteral("Zone %1").arg(i);
        features.append(feature);
    }
    return QJsonDocument(QJsonObject{ { QStringLiteral("features"), features } }).toJson(QJsonDocument::Compact);
}

} // namespace

void GeoZoneBenchmark::initTestCase()
{
    _document = zonesDocument();
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(_document, _result, errorString), qPrintable(errorString));
    QCOMPARE(_result.zones.count(), kZoneCount);
    _source = std::make_unique<GeoZoneSource>(QStringLiteral("benchmark.json"), QString());
    _source->setResult(_result, kStart, kStart);
    _catalog.rebuild({ _source.get() });
    QCOMPARE(_catalog.zones().count(), kZoneCount);

    QRandomGenerator random(1);
    for (int i = 0; i < kQueryCount; i++) {
        _points.append(randomCoordinate(random));
    }
    qInfo() << "Document of" << (_document.size() / 1024) << "KiB," << _catalog.shapes().count() << "shapes";

    QVERIFY(_storageDir.isValid());
    QVERIFY(GeoZoneStore(_storageDir.path()).writeFile(QStringLiteral("zones.json"), _document, errorString));
}

namespace {

/// Job restoring the stored copy of the benchmark source at startup
GeoZoneLoader::Job restoreJob(const GeoZoneStore &store)
{
    GeoZoneLoader::Job job;
    job.key = QStringLiteral("benchmark.json");
    job.sourcePath = store.path(QStringLiteral("zones.json"));
    job.fromStore = true;
    return job;
}

} // namespace

void GeoZoneBenchmark::_benchmarkRestoreParsing()
{
    // The first start after loading or updating, which parses and writes the parse cache
    const GeoZoneStore store(_storageDir.path());
    const GeoZoneLoader::Job job = restoreJob(store);
    QBENCHMARK {
        GeoZoneParseCache::remove(job.sourcePath);
        const GeoZoneLoader::Result result = GeoZoneLoader::run(job, store);
        QVERIFY(!result.fromCache);
        QCOMPARE(result.result.zones.count(), kZoneCount);
    }
    qInfo() << "Parse cache of" << (QFileInfo(GeoZoneParseCache::path(job.sourcePath)).size() / 1024) << "KiB";
}

void GeoZoneBenchmark::_benchmarkRestoreFromCache()
{
    const GeoZoneStore store(_storageDir.path());
    const GeoZoneLoader::Job job = restoreJob(store);
    (void) GeoZoneLoader::run(job, store);
    QBENCHMARK {
        const GeoZoneLoader::Result result = GeoZoneLoader::run(job, store);
        QVERIFY(result.fromCache);
        QCOMPARE(result.result.zones.count(), kZoneCount);
    }
}

void GeoZoneBenchmark::_benchmarkRestoreFromCacheHashing()
{
    // The stored copy has another modification time, like after copying the settings, so it is hashed first
    const GeoZoneStore store(_storageDir.path());
    const GeoZoneLoader::Job job = restoreJob(store);
    (void) GeoZoneLoader::run(job, store);
    QFile file(job.sourcePath);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QDateTime modified = file.fileTime(QFileDevice::FileModificationTime);
    QBENCHMARK {
        modified = modified.addSecs(1);
        QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
        const GeoZoneLoader::Result result = GeoZoneLoader::run(job, store);
        QVERIFY(result.fromCache);
    }
}

void GeoZoneBenchmark::_benchmarkParse()
{
    QBENCHMARK {
        GeoZoneParser::Result result;
        QString errorString;
        QVERIFY(GeoZoneParser::parse(_document, result, errorString));
    }
}

void GeoZoneBenchmark::_benchmarkLoadSource()
{
    QBENCHMARK {
        GeoZoneSource source(QStringLiteral("benchmark.json"), QString());
        source.setResult(_result, kStart, kStart);
    }
}

void GeoZoneBenchmark::_benchmarkCatalogRebuild()
{
    QBENCHMARK {
        GeoZoneCatalog catalog;
        catalog.rebuild({ _source.get() });
    }
}

void GeoZoneBenchmark::_benchmarkZonesAt()
{
    qsizetype found = 0;
    QBENCHMARK {
        found = 0;
        for (const QGeoCoordinate &point : std::as_const(_points)) {
            found += _catalog.zonesAt(point).count();
        }
    }
    QVERIFY(found > 0);
}

void GeoZoneBenchmark::_benchmarkZonesInArea()
{
    // Areas of about 10 km like those around a vehicle or a mission leg
    const GeoZonePolicy policy = GeoZonePolicy::alerts();
    qsizetype found = 0;
    QBENCHMARK {
        found = 0;
        for (const QGeoCoordinate &point : std::as_const(_points)) {
            const QGeoRectangle area(point.atDistanceAndAzimuth(7000, 315), point.atDistanceAndAzimuth(7000, 135));
            found += _catalog.zonesInArea(area, policy).count();
        }
    }
    QVERIFY(found > 0);
}

void GeoZoneBenchmark::_benchmarkPathEvaluation()
{
    // A mission of 500 waypoints 2 km apart wandering across the zones, 100 m above sea level
    QRandomGenerator random(2);
    QList<GeoZone::Position> positions;
    QGeoCoordinate position(kSouth + (kSize / 2), kWest + (kSize / 2));
    double heading = 0;
    for (int i = 0; i < kMissionWaypoints; i++) {
        positions.append(GeoZone::Position{ position, 100 });
        heading += random.bounded(90) - 45;
        position = position.atDistanceAndAzimuth(2000, heading);
    }
    GeoZonePathEvaluator::Path path = GeoZonePathEvaluator::Path::fromPositions(positions);
    const QDateTime end = kStart.addSecs(3 * 3600);
    path.setTimes(kStart, end);
    const GeoZonePathEvaluator evaluator(_catalog, GeoZonePolicy::mission(kStart, end), kStart);

    GeoZonePathEvaluator::Result result;
    QBENCHMARK {
        result = evaluator.evaluate({ path });
    }
    QVERIFY(result.activeEnteredZones().count() > 10);
}

void GeoZoneBenchmark::_benchmarkRouteSearch()
{
    // Legs of 20 km in all directions between waypoints outside the zones, 100 m above the terrain, each with a detour,
    // a climb and a descent searched when it enters restricted zones
    QRandomGenerator random(3);
    QList<GeoZoneRouteSearch::Leg> legs;
    while (legs.count() < kRouteLegs) {
        const QGeoCoordinate from = randomCoordinate(random);
        const QGeoCoordinate to = from.atDistanceAndAzimuth(20000, random.bounded(360.0));
        if (_catalog.zonesAt(from).isEmpty() && _catalog.zonesAt(to).isEmpty()) {
            legs.append(GeoZoneRouteSearch::Leg{ GeoZone::Position{ from, 400 }, GeoZone::Position{ to, 400 } });
        }
    }
    GeoZoneRouteSearch search(_catalog, GeoZonePolicy::routing(QDateTime(), QDateTime()));
    search.setTerrainLookup([](const QGeoCoordinate &) { return 300.0; });

    int crossing = 0;
    int routed = 0;
    QBENCHMARK {
        crossing = 0;
        routed = 0;
        for (const GeoZoneRouteSearch::Leg &leg : std::as_const(legs)) {
            const GeoZoneRouteSearch::Result result = search.plan(leg);
            crossing += result.crossedZones.isEmpty() ? 0 : 1;
            routed += result.best() ? 1 : 0;
        }
    }
    qInfo() << routed << "of" << crossing << "legs entering zones routed";
    QVERIFY(crossing > 10);
    QVERIFY(routed > (crossing / 2));
}

void GeoZoneBenchmark::_benchmarkRouteSearchMaze()
{
    // A leg of 20 km through a row of a maze of 1200 zones 420 m apart, see GeoZonePlanningTest::_testRouteMaze
    const QGeoCoordinate southWest(46.5, 7.5);
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(QJsonDocument(QJsonObject{ { QStringLiteral("features"), _mazeFeatures(southWest, 40, 30, 420, 7) } }).toJson(), result, errorString),
             qPrintable(errorString));
    GeoZoneSource source(QStringLiteral("maze.json"), QString());
    source.setResult(result, kStart, kStart);
    GeoZoneCatalog catalog;
    catalog.rebuild({ &source });
    const GeoZoneGeometry::LocalProjection projection(southWest);
    const GeoZoneRouteSearch::Leg leg{ GeoZone::Position{ projection.toGeo(QPointF(-1000, 6090)), 400 },
                                       GeoZone::Position{ projection.toGeo(QPointF(19000, 6090)), 400 } };
    GeoZoneRouteSearch search(catalog, GeoZonePolicy::routing(QDateTime(), QDateTime()));
    search.setTerrainLookup([](const QGeoCoordinate &) { return 300.0; });

    GeoZoneRouteSearch::Route detour;
    QBENCHMARK_ONCE {
        detour = search.detour(leg);
    }
    const GeoZoneRouteSearch::Statistics &statistics = detour.statistics;
    qInfo() << "Length" << qRound(detour.length) << "waypoints" << detour.waypoints.count() << "rounds" << statistics.rounds << "obstacles"
            << statistics.includedObstacles << "of" << statistics.obstacles << "nodes" << statistics.nodes << "visibility tests" << statistics.visibilityTests
            << "cached" << statistics.cachedVisibility << "work" << statistics.work;
    QVERIFY(detour.isValid());
}

QTEST_GUILESS_MAIN(GeoZoneBenchmark)
