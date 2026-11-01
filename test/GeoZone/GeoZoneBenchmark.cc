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
#include "GeoZoneTestHelpers.h"

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

QTEST_GUILESS_MAIN(GeoZoneBenchmark)
