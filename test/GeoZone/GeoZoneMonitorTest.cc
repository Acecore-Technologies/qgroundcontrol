/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMonitorTest.h"
#include "GeoZoneActiveVehicleView.h"
#include "GeoZoneNearbyModel.h"
#include "GeoZoneTestHelpers.h"
#include "Vehicle.h"

#include <memory>

using namespace GeoZoneTestHelpers;

namespace {

mavlink_message_t globalPositionMessage(int vehicleId, int16_t vx, int16_t vy, int16_t vz)
{
    mavlink_message_t message{};
    (void) mavlink_msg_global_position_int_pack(vehicleId, MAV_COMP_ID_AUTOPILOT1, &message, 0, 470000000, 80000000, 500000, 100000, vx, vy, vz, UINT16_MAX);
    return message;
}

/// Keeps what would be spoken
class RecordingAudioSink : public GeoZoneMonitor::AudioSink
{
public:
    void say(const QString &text) override { spoken.append(text); }

    QStringList spoken;
};

GeoZoneAlertArbiter::Alert arbiterAlert(int id, int vehicleKey, const QString &zone, GeoZone::Restriction restriction, bool inside)
{
    GeoZoneAlertArbiter::Alert alert;
    alert.id = id;
    alert.vehicleKey = vehicleKey;
    alert.zoneKey = zone;
    alert.severity = GeoZone::restrictionSeverity(restriction);
    alert.inside = inside;
    alert.breach = inside && (restriction == GeoZone::Prohibited);
    alert.message = QStringLiteral("%1 %2").arg(inside ? QStringLiteral("Entered") : QStringLiteral("Approaching"), zone);
    alert.repeatMessage = QStringLiteral("Still inside %1").arg(zone);
    return alert;
}

} // namespace

void GeoZoneMonitorTest::_testMonitorAlerts()
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
    QCOMPARE(monitor.activeView()->nearbyZones().count(), 1);
    QVERIFY(!monitor.activeView()->inside());
    QCOMPARE(monitor.activeView()->nearbyZones()[0].toMap()[QStringLiteral("distance")].toInt(), 80);
    QCOMPARE(monitor.activeView()->alertRestriction(), static_cast<int>(GeoZone::Prohibited));
    monitor.evaluate({ center.atDistanceAndAzimuth(350, 0) });
    QCOMPARE(alertSpy.count(), 1);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) });
    QCOMPARE(alertSpy.count(), 2);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Entered geozone P")));
    QVERIFY(monitor.activeView()->inside());

    // The list model updates distances in place
    QSignalSpy resetSpy(monitor.activeView()->nearbyModel(), &QAbstractItemModel::modelReset);
    QSignalSpy dataChangedSpy(monitor.activeView()->nearbyModel(), &QAbstractItemModel::dataChanged);
    QCOMPARE(monitor.activeView()->nearbyModel()->count(), 1);

    // Jitter across the boundary and the approach distance does not alert again
    monitor.evaluate({ center.atDistanceAndAzimuth(310, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(290, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(410, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(390, 0) });
    QCOMPARE(resetSpy.count(), 0);
    QVERIFY(dataChangedSpy.count() >= 3);
    QCOMPARE(monitor.activeView()->nearbyModel()->data(monitor.activeView()->nearbyModel()->index(0), GeoZoneNearbyModel::DistanceRole).toInt(), 90);
    QCOMPARE(alertSpy.count(), 2);

    // Reloading changed zones doesn't repeat the approach alert of an unchanged zone
    (void) _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(3000, 270), 300),
        expired,
        _circleFeature(QStringLiteral("NEW"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(9000, 0), 300),
    });
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));
    QVERIFY(monitor.activeView()->nearbyZones().isEmpty());
    monitor.evaluate({ center.atDistanceAndAzimuth(390, 0) });
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(monitor.activeView()->nearbyZones().count(), 1);

    // Leaving the area resets the zone
    monitor.evaluate({ center.atDistanceAndAzimuth(1000, 0) });
    QVERIFY(monitor.activeView()->nearbyZones().isEmpty());
    QCOMPARE(monitor.activeView()->alertRestriction(), -1);
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
    QCOMPARE(monitor.activeView()->alertRestriction(), static_cast<int>(GeoZone::USpace));
    monitor.evaluate({ center.atDistanceAndAzimuth(6000, 0) });
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(monitor.activeView()->alertRestriction(), static_cast<int>(GeoZone::Prohibited));
    QVERIFY(monitor.activeView()->inside());
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

void GeoZoneMonitorTest::_testMonitorAlertOrder()
{
    // Simultaneous alerts are emitted most severe first, entries before approaches
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("C"), QStringLiteral("CONDITIONAL"), center, 500),
        _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(100, 0), 50),
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(100, 180), 50),
    });
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 3);
    QVERIFY(alertSpy[0][0].toString().startsWith(QStringLiteral("Approaching geozone P")));
    QCOMPARE(alertSpy[0][1].toInt(), GeoZone::restrictionSeverity(GeoZone::Prohibited));
    QVERIFY(alertSpy[1][0].toString().startsWith(QStringLiteral("Approaching geozone A")));
    QVERIFY(alertSpy[2][0].toString().startsWith(QStringLiteral("Entered geozone C")));
}

void GeoZoneMonitorTest::_testMonitorTimeToEntry()
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    const auto restoreDefault = qScopeGuard([settings]() { settings->approachTime()->setRawValue(settings->approachTime()->rawDefaultValue()); });
    settings->approachTime()->setRawValue(30);

    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate above(47.1, 8);
    QJsonObject overhead = _circleFeature(QStringLiteral("HIGH"), QStringLiteral("PROHIBITED"), above, 500);
    QJsonArray geometry = overhead[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = 500;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
    geometry[0] = layer;
    overhead[QStringLiteral("geometry")] = geometry;
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"),
        QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300), overhead }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    const GeoZoneMonitor::Velocity north{ 25, 0, 0, true };

    // Without a velocity only the distance counts
    const GeoZone::Position start{ center.atDistanceAndAzimuth(1000, 180) };
    monitor.evaluate(start);
    QCOMPARE(alertSpy.count(), 0);

    // Flying away the zone is never entered
    monitor.evaluate(start, GeoZoneMonitor::noVehicle, GeoZoneMonitor::Velocity{ -25, 0, 0, true });
    QCOMPARE(alertSpy.count(), 0);
    QCOMPARE(monitor.activeView()->predictedEntryTime(), -1);
    QVERIFY(monitor.activeView()->predictedPath().isEmpty());

    // At 25 m/s the boundary 700 m ahead is reached in 28 s, within the approach time
    QSignalSpy predictionSpy(monitor.activeView(), &GeoZoneActiveVehicleView::predictionChanged);
    monitor.evaluate(start, GeoZoneMonitor::noVehicle, north);
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Approaching geozone P (Prohibited), entry in about 28 s"));
    // The map shows the path to the entry with the time left
    QVERIFY(predictionSpy.count() > 0);
    QCOMPARE(monitor.activeView()->predictedEntryTime(), 28);
    QVERIFY(qAbs(monitor.activeView()->predictedEntry().distanceTo(center.atDistanceAndAzimuth(300, 180)) - 0) < 2);
    QCOMPARE(monitor.activeView()->predictedPath().count(), 2);
    QCOMPARE(monitor.activeView()->predictedPath().first().value<QGeoCoordinate>(), start.coordinate);
    QCOMPARE(GeoZoneInfo::fromVariant(monitor.activeView()->predictedEntryZone())->identifier(), QStringLiteral("P"));
    QCOMPARE(monitor.activeView()->nearbyZones().count(), 1);
    QCOMPARE(monitor.activeView()->nearbyZones()[0].toMap()[QStringLiteral("timeToEntry")].toInt(), 28);
    QCOMPARE(monitor.activeView()->nearbyZones()[0].toMap()[QStringLiteral("distance")].toInt(), 700);

    // Too slow to arrive within the approach time, and not repeated while it stays near
    alertSpy.clear();
    monitor.evaluate(start, GeoZoneMonitor::noVehicle, GeoZoneMonitor::Velocity{ 10, 0, 0, true });
    QVERIFY(monitor.activeView()->nearbyZones().isEmpty());
    monitor.evaluate({ center.atDistanceAndAzimuth(900, 180) }, GeoZoneMonitor::noVehicle, north);
    monitor.evaluate({ center.atDistanceAndAzimuth(800, 180) }, GeoZoneMonitor::noVehicle, north);
    QCOMPARE(alertSpy.count(), 1);
    alertSpy.clear();
    monitor.evaluate({ center.atDistanceAndAzimuth(700, 180) }, GeoZoneMonitor::noVehicle, north);
    QCOMPARE(alertSpy.count(), 0);

    // Climbing into the floor of a zone overhead
    GeoZoneMonitor climbMonitor(&manager);
    QSignalSpy climbSpy(&climbMonitor, &GeoZoneMonitor::alert);
    climbMonitor.evaluate({ above, 400, 300 }, GeoZoneMonitor::noVehicle, GeoZoneMonitor::Velocity{ 0, 0, 0, true });
    QCOMPARE(climbSpy.count(), 0);
    climbMonitor.evaluate({ above, 400, 300 }, GeoZoneMonitor::noVehicle, GeoZoneMonitor::Velocity{ 0, 0, 5, true });
    QCOMPARE(climbSpy.count(), 1);
    QVERIFY(climbSpy.last()[0].toString().startsWith(QStringLiteral("Approaching geozone HIGH (Prohibited), entry in about 2")));

    // Zones above and below, with their limits at the position
    QSignalSpy bandsSpy(climbMonitor.activeView(), &GeoZoneActiveVehicleView::verticalBandsChanged);
    climbMonitor.evaluate({ above, 400, 300 });
    QCOMPARE(climbMonitor.activeView()->altitudeAMSL(), 400.0);
    QCOMPARE(climbMonitor.activeView()->terrainAMSL(), 300.0);
    QCOMPARE(climbMonitor.activeView()->verticalBands().count(), 1);
    const QVariantMap band = climbMonitor.activeView()->verticalBands().first().toMap();
    QCOMPARE(band[QStringLiteral("name")].toString(), QStringLiteral("HIGH"));
    QCOMPARE(band[QStringLiteral("floor")].toDouble(), 500.0);
    QVERIFY(std::isnan(band[QStringLiteral("ceiling")].toDouble()));
    const qsizetype bandSignals = bandsSpy.count();
    climbMonitor.evaluate({ above, 400.2, 300 });
    QCOMPARE(bandsSpy.count(), bandSignals);
    climbMonitor.evaluate({ center.atDistanceAndAzimuth(5000, 270), 400, 300 });
    QVERIFY(climbMonitor.activeView()->verticalBands().isEmpty());

    // Turned off
    settings->approachTime()->setRawValue(0);
    GeoZoneMonitor offMonitor(&manager);
    QSignalSpy offSpy(&offMonitor, &GeoZoneMonitor::alert);
    offMonitor.evaluate(start, GeoZoneMonitor::noVehicle, north);
    QCOMPARE(offSpy.count(), 0);
}

void GeoZoneMonitorTest::_testTerrainAhead()
{
    ScopedEnvironment environment;
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    const auto restore = qScopeGuard([settings]() { settings->approachTime()->setRawValue(settings->approachTime()->rawDefaultValue()); });
    settings->approachTime()->setRawValue(60);

    // A plateau 300 m above the valley from 47.01 north, with a zone up to 120 m above the ground
    const QGeoCoordinate valley(47, 8);
    const QGeoCoordinate plateauZone(47.02, 8);
    environment.setTerrain([](const QGeoCoordinate &coordinate) { return (coordinate.latitude() >= 47.01) ? 400.0 : 100.0; });
    QJsonObject zone = _circleFeature(QStringLiteral("PLATEAU"), QStringLiteral("PROHIBITED"), plateauZone, 500);
    QJsonArray geometry = zone[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = 0;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AGL");
    layer[QStringLiteral("upperLimit")] = 120;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AGL");
    geometry[0] = layer;
    zone[QStringLiteral("geometry")] = geometry;
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ zone }), errorString), qPrintable(errorString));

    // Paths get the terrain where it is known
    const QList<GeoZone::Position> path = GeoZoneTerrain::withTerrain({ { valley, 450, 100 }, { plateauZone, 450 } });
    QCOMPARE(path.count(), 23);
    QCOMPARE(path.constLast().terrainAMSL, 400.0);
    QCOMPARE(path[5].altitudeAMSL, 450.0);
    QCOMPARE(path[5].terrainAMSL, 100.0);
    QCOMPARE(path[15].terrainAMSL, 400.0);

    // 350 m above the valley, the vehicle flies into the zone over the plateau at 40 m/s in about 43 s
    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.evaluate({ valley, 450, 100 }, GeoZoneMonitor::noVehicle, GeoZoneMonitor::Velocity{ 40, 0, 0, true });
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Approaching geozone PLATEAU (Prohibited), entry in about 43 s"));

    // Without terrain tiles the ground ahead is unknown, rather than that below the vehicle, which would put the zone
    // below it, so the zone counts as reaching the vehicle's altitude
    environment.setTerrain(qQNaN());
    const QList<GeoZone::Position> unknownPath = GeoZoneTerrain::withTerrain({ { valley, 450, 100 }, { plateauZone, 450 } });
    QCOMPARE(unknownPath.count(), 23);
    QCOMPARE(unknownPath.constFirst().terrainAMSL, 100.0);
    QVERIFY(qIsNaN(unknownPath[15].terrainAMSL));
    QVERIFY(qIsNaN(unknownPath.constLast().terrainAMSL));
    GeoZoneMonitor noTerrain(&manager);
    QSignalSpy noTerrainSpy(&noTerrain, &GeoZoneMonitor::alert);
    noTerrain.evaluate({ valley, 450, 100 }, GeoZoneMonitor::noVehicle, GeoZoneMonitor::Velocity{ 40, 0, 0, true });
    QCOMPARE(noTerrainSpy.count(), 1);
    QVERIFY(noTerrainSpy.last()[0].toString().startsWith(QStringLiteral("Approaching geozone PLATEAU (Prohibited)")));
}

void GeoZoneMonitorTest::_testVehicleTelemetry()
{
    const int vehicleId = 1;
    const QGeoCoordinate start(47, 8);
    const GeoZoneVehicleMonitor::Clock clock = []() { return qint64{ 0 }; };
    GeoZoneVehicleMonitor monitor(nullptr, vehicleId, 1, clock);

    // The velocity sent with the position is used while recent, north east up from north east down
    monitor.handleMessage(globalPositionMessage(vehicleId, 2500, -500, 300), 1000);
    GeoZoneVehicleMonitor::Velocity velocity = monitor.velocity(1500, { start, 500 });
    QVERIFY(velocity.valid);
    QCOMPARE(velocity.north, 25.0);
    QCOMPARE(velocity.east, -5.0);
    QCOMPARE(velocity.up, -3.0);

    // Old velocities give way to the positions, which haven't moved
    velocity = monitor.velocity(1000 + GeoZoneVehicleMonitor::reportedVelocityMaxAgeMSecs + 1500, { start, 500 });
    QVERIFY(velocity.valid);
    QCOMPARE(velocity.groundSpeed(), 0.0);

    // Vehicles sending zero velocities while their positions move are estimated
    GeoZoneVehicleMonitor estimating(nullptr, vehicleId, 2, clock);
    estimating.handleMessage(globalPositionMessage(vehicleId, 0, 0, 0), 0);
    (void) estimating.velocity(0, { start, 500 });
    estimating.handleMessage(globalPositionMessage(vehicleId, 0, 0, 0), 1000);
    velocity = estimating.velocity(1000, { start.atDistanceAndAzimuth(20, 90), 500 });
    QVERIFY(qAbs(velocity.east - 20) < 0.1);

    // Positions are stale after the timeout, or three of their usual intervals on slow links
    QVERIFY(!monitor.positionStale(1000 + GeoZoneVehicleMonitor::positionTimeoutMSecs));
    QVERIFY(monitor.positionStale(1000 + GeoZoneVehicleMonitor::positionTimeoutMSecs + 1));
    monitor.handleMessage(globalPositionMessage(vehicleId, 0, 0, 0), 21000);
    QVERIFY(!monitor.positionStale(21000 + 50000));
    QVERIFY(monitor.positionStale(21000 + 60001));
    QVERIFY(!GeoZoneVehicleMonitor(nullptr, 2, 3, clock).positionStale(100000));

    // The ground below the vehicle comes from the height above terrain it sends, is where a disarmed vehicle is, or
    // else comes from the terrain data of QGroundControl, which degrades the height above ground
    ScopedEnvironment environment;
    environment.setTerrain(380);
    using TerrainSource = GeoZoneVehicleMonitor::TerrainSource;
    TerrainSource source = TerrainSource::Unknown;
    GeoZone::Position position = GeoZoneVehicleMonitor::vehiclePosition(start, 500, 90, true, &source);
    QCOMPARE(position.terrainAMSL, 410.0);
    QVERIFY(source == TerrainSource::Vehicle);
    position = GeoZoneVehicleMonitor::vehiclePosition(start, 500, qQNaN(), false, &source);
    QCOMPARE(position.terrainAMSL, 500.0);
    QVERIFY(source == TerrainSource::Ground);
    position = GeoZoneVehicleMonitor::vehiclePosition(start, 500, qQNaN(), true, &source);
    QCOMPARE(position.altitudeAMSL, 500.0);
    QCOMPARE(position.terrainAMSL, 380.0);
    QVERIFY(source == TerrainSource::Database);
    environment.setTerrain(qQNaN());
    position = GeoZoneVehicleMonitor::vehiclePosition(start, 500, qQNaN(), true, &source);
    QVERIFY(std::isnan(position.terrainAMSL));
    QVERIFY(source == TerrainSource::Unknown);
    position = GeoZoneVehicleMonitor::vehiclePosition(start, qQNaN(), 90, true, &source);
    QVERIFY(std::isnan(position.altitudeAMSL) && std::isnan(position.terrainAMSL));
    QVERIFY(source == TerrainSource::Unknown);
    QVERIFY(!monitor.altitudeDegraded());
}

void GeoZoneMonitorTest::_testMonitorMultipleVehicles()
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
    QCOMPARE(monitor.activeView()->nearbyZones().count(), 1);

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
    QVERIFY(monitor.activeView()->inside());
    QSignalSpy nearbySpy(monitor.activeView(), &GeoZoneActiveVehicleView::nearbyZonesChanged);
    QCOMPARE(monitor.activeView()->nearbyModel()->count(), 1);
    monitor.setActiveVehicleId(2);
    QCOMPARE(nearbySpy.count(), 1);
    QVERIFY(monitor.activeView()->nearbyZones().isEmpty());
    QCOMPARE(monitor.activeView()->nearbyModel()->count(), 0);
    QVERIFY(!monitor.activeView()->inside());
    monitor.setActiveVehicleId(1);
    QCOMPARE(monitor.activeView()->nearbyModel()->count(), 1);
    QVERIFY(monitor.activeView()->inside());
    monitor.setActiveVehicleId(2);

    // A removed vehicle alerts again when it reconnects, the others keep their state
    monitor.removePositionMonitor(1);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) }, 1);
    QCOMPARE(alertSpy.count(), 4);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Vehicle 1: Entered geozone P")));
}

void GeoZoneMonitorTest::_testAlertLog()
{
    // The alerts are logged on the same day whenever the test runs
    const ScopedEnvironment environment;
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
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

    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
    monitor.evaluate({ center.atDistanceAndAzimuth(380, 0), 520, 400 }, 3);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) }, 3);
    QCOMPARE(eventSpy.count(), 2);
    const QDate logDate = environment.now().date();
    QCOMPARE(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().time, environment.now());

    QFile file(log.filePath(logDate));
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

    // Feed text starting like a spreadsheet formula stays text
    GeoZoneMonitor::AlertEvent event;
    event.time = environment.now();
    event.zoneName = QStringLiteral("=1+1");
    event.zoneIdentifier = QStringLiteral("@A1");
    QVERIFY(GeoZoneAlertLog::line(event).contains(QStringLiteral(",'@A1,'=1+1,")));
}

void GeoZoneMonitorTest::_testAlertHistory()
{
    const ScopedEnvironment environment;
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    const auto restoreDefault = qScopeGuard([settings]() { settings->logAlerts()->setRawValue(settings->logAlerts()->rawDefaultValue()); });
    settings->logAlerts()->setRawValue(true);

    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QJsonObject feature = _circleFeature(QStringLiteral("P1"), QStringLiteral("PROHIBITED"), center, 300);
    feature[QStringLiteral("name")] = QStringLiteral("Prison \"West\", Zurich");
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ feature }), errorString), qPrintable(errorString));

    // The alerts of the session, newest first
    GeoZoneMonitor monitor(&manager);
    GeoZoneAlertHistory *const history = monitor.alertHistory();
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
    QSignalSpy showSpy(&manager, &GeoZoneManager::showZoneRequested);
    const QGeoCoordinate approach = center.atDistanceAndAzimuth(380, 0);
    monitor.evaluate({ approach, 520, 400 }, 3);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) }, 3);
    QCOMPARE(history->count(), 2);
    QCOMPARE(history->unacknowledgedCount(), 2);
    QVERIFY(history->data(history->index(0), GeoZoneAlertHistory::InsideRole).toBool());
    QVERIFY(history->data(history->index(0), GeoZoneAlertHistory::MessageRole).toString().startsWith(QStringLiteral("Entered geozone Prison \"West\", Zurich")));
    QVERIFY(!history->data(history->index(1), GeoZoneAlertHistory::InsideRole).toBool());

    // Showing an alert acknowledges it and shows its zone and the vehicle position
    history->show(1);
    QCOMPARE(history->unacknowledgedCount(), 1);
    QVERIFY(history->data(history->index(1), GeoZoneAlertHistory::AcknowledgedRole).toBool());
    QCOMPARE(showSpy.count(), 1);
    QCOMPARE(GeoZoneInfo::fromVariant(showSpy.first().first())->identifier(), QStringLiteral("P1"));
    QVERIFY(history->shownPosition().distanceTo(approach) < 0.01);
    history->clearShownPosition();
    QVERIFY(!history->shownPosition().isValid());
    history->acknowledgeAll();
    QCOMPARE(history->unacknowledgedCount(), 0);

    // Alerts an earlier session logged today are read back acknowledged, after those of the session
    GeoZoneAlertLog log;
    log.setDirectory(dir.filePath(QStringLiteral("logs")));
    for (const QList<QVariant> &arguments : std::as_const(eventSpy)) {
        log.append(arguments.first().value<GeoZoneMonitor::AlertEvent>());
    }
    GeoZoneMonitor::AlertEvent formula;
    formula.time = environment.now();
    formula.zoneIdentifier = QStringLiteral("F1");
    formula.zoneName = QStringLiteral("=1+1");
    formula.restriction = GeoZone::Conditional;
    formula.distance = 50;
    formula.timeToEntry = 12;
    log.append(formula);
    const QDate logDate = formula.time.date();
    GeoZoneAlertHistory later(&manager);
    later.append(formula);
    later.loadLog(log.filePath(logDate));
    QCOMPARE(later.count(), 4);
    QCOMPARE(later.unacknowledgedCount(), 1);
    QVERIFY(!later.data(later.index(0), GeoZoneAlertHistory::EarlierRole).toBool());
    QVERIFY(later.data(later.index(1), GeoZoneAlertHistory::EarlierRole).toBool());
    QVERIFY(later.data(later.index(1), GeoZoneAlertHistory::AcknowledgedRole).toBool());
    QCOMPARE(later.data(later.index(1), GeoZoneAlertHistory::ZoneNameRole).toString(), QStringLiteral("=1+1"));
    QVERIFY(later.data(later.index(1), GeoZoneAlertHistory::MessageRole).toString().endsWith(QStringLiteral("entry in about 12 s")));
    QCOMPARE(later.data(later.index(2), GeoZoneAlertHistory::MessageRole).toString(), history->data(history->index(0), GeoZoneAlertHistory::MessageRole).toString());
    QCOMPARE(later.data(later.index(3), GeoZoneAlertHistory::AltitudeRole).toDouble(), 520.0);
    QVERIFY(!GeoZoneAlertHistory::parseLogLine(GeoZoneAlertLog::header()));

    // Earlier alerts are shown by their zone identifier and name
    later.show(2);
    QCOMPARE(showSpy.count(), 2);
}

void GeoZoneMonitorTest::_testFenceStatus()
{
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    GeoZoneMonitor monitor(&manager);
    Vehicle vehicle(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    (void) monitor.addVehicle(&vehicle);
    monitor.setActiveVehicle(&vehicle);
    const auto fenceStatus = [](uint8_t breachStatus, uint16_t breachCount, uint8_t breachType) {
        mavlink_message_t message{};
        (void) mavlink_msg_fence_status_pack(1, MAV_COMP_ID_AUTOPILOT1, &message, breachStatus, breachCount, breachType, 0, FENCE_MITIGATE_NONE);
        return message;
    };
    QSignalSpy breachSpy(&monitor, &GeoZoneMonitor::fenceBreach);
    QSignalSpy statusSpy(monitor.activeView(), &GeoZoneActiveVehicleView::fenceStatusChanged);

    // Not breached, as received from the vehicle
    QVERIFY(!monitor.activeView()->fenceStatusKnown());
    emit vehicle.mavlinkMessageReceived(fenceStatus(0, 0, FENCE_BREACH_NONE));
    QVERIFY(monitor.activeView()->fenceStatusKnown());
    QVERIFY(!monitor.activeView()->fenceBreached());
    QCOMPARE(statusSpy.count(), 1);

    // A breach is announced once while it lasts
    emit vehicle.mavlinkMessageReceived(fenceStatus(1, 1, FENCE_BREACH_BOUNDARY));
    emit vehicle.mavlinkMessageReceived(fenceStatus(1, 1, FENCE_BREACH_BOUNDARY));
    QVERIFY(monitor.activeView()->fenceBreached());
    QCOMPARE(monitor.activeView()->fenceBreachText(), QStringLiteral("Boundary fence breached"));
    QCOMPARE(breachSpy.count(), 1);
    QCOMPARE(breachSpy.first().first().toString(), QStringLiteral("Boundary fence breached"));
    QCOMPARE(statusSpy.count(), 2);

    // Cleared, then breached again at the maximum altitude
    emit vehicle.mavlinkMessageReceived(fenceStatus(0, 1, FENCE_BREACH_NONE));
    QVERIFY(!monitor.activeView()->fenceBreached());
    QVERIFY(monitor.activeView()->fenceBreachText().isEmpty());
    emit vehicle.mavlinkMessageReceived(fenceStatus(1, 2, FENCE_BREACH_MAXALT));
    QCOMPARE(breachSpy.count(), 2);
    QCOMPARE(monitor.activeView()->fenceBreachText(), QStringLiteral("Maximum altitude fence breached (2 time(s))"));

    // Other vehicles don't change the status shown, their breaches are announced naming them
    Vehicle other(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    (void) monitor.addVehicle(&other);
    emit other.mavlinkMessageReceived(fenceStatus(0, 0, FENCE_BREACH_NONE));
    QVERIFY(monitor.activeView()->fenceBreached());
    emit other.mavlinkMessageReceived(fenceStatus(1, 1, FENCE_BREACH_MINALT));
    QCOMPARE(breachSpy.count(), 3);
    QCOMPARE(breachSpy.last().first().toString(), QStringLiteral("Vehicle %1: Minimum altitude fence breached").arg(other.id()));
    monitor.setActiveVehicle(&other);
    QCOMPARE(monitor.activeView()->fenceBreachText(), QStringLiteral("Minimum altitude fence breached"));
    monitor.removeVehicle(&other);
    QVERIFY(!monitor.activeView()->fenceStatusKnown());
}

void GeoZoneMonitorTest::_testVehicleMonitors()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300) }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    qint64 now = 0;
    monitor.setClock([&now]() { return now; });
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    QVERIFY(!monitor.monitoring());

    // Offline vehicles, which all have the same system id
    auto first = std::make_unique<Vehicle>(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    Vehicle second(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    QCOMPARE(first->id(), second.id());
    GeoZoneVehicleMonitor *const firstMonitor = monitor.addVehicle(first.get());
    GeoZoneVehicleMonitor *const secondMonitor = monitor.addVehicle(&second);
    QVERIFY(firstMonitor && secondMonitor && (firstMonitor != secondMonitor));
    QCOMPARE(monitor.addVehicle(&second), secondMonitor);
    QCOMPARE(monitor.vehicleMonitor(&second), secondMonitor);
    QVERIFY(monitor.monitoring());
    monitor.setActiveVehicle(first.get());

    // Each vehicle has its own alert states, nearby zones and vertical bands, and is named in the alerts
    const GeoZone::Position inside{ center.atDistanceAndAzimuth(250, 0), 450, 400 };
    const GeoZone::Position away{ center.atDistanceAndAzimuth(5000, 0), 600, 400 };
    monitor.evaluate(first.get(), inside);
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Vehicle %1: Entered geozone P (Prohibited)").arg(first->id()));
    QVERIFY(monitor.activeView()->inside());
    QCOMPARE(monitor.activeView()->verticalBands().count(), 1);
    QCOMPARE(monitor.activeView()->altitudeAMSL(), 450.0);
    monitor.evaluate(&second, away);
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(secondMonitor->nearby().isEmpty());
    QVERIFY(monitor.activeView()->inside());
    monitor.evaluate(&second, inside);
    QCOMPARE(alertSpy.count(), 2);
    monitor.evaluate(first.get(), inside);
    QCOMPARE(alertSpy.count(), 2);

    // The published state follows the active vehicle
    monitor.evaluate(&second, away);
    QSignalSpy nearbySpy(monitor.activeView(), &GeoZoneActiveVehicleView::nearbyZonesChanged);
    monitor.setActiveVehicle(&second);
    QCOMPARE(nearbySpy.count(), 1);
    QVERIFY(!monitor.activeView()->inside());
    QVERIFY(monitor.activeView()->verticalBands().isEmpty());
    QCOMPARE(monitor.activeView()->altitudeAMSL(), 600.0);
    monitor.setActiveVehicle(first.get());
    QVERIFY(monitor.activeView()->inside());

    // The messages of each vehicle reach its monitor, timed by the monitor's clock
    now = 1000;
    emit second.mavlinkMessageReceived(globalPositionMessage(second.id(), 0, 0, 0));
    now += GeoZoneVehicleMonitor::positionTimeoutMSecs + 1;
    QVERIFY(secondMonitor->positionStale(now));
    QVERIFY(!firstMonitor->positionStale(now));

    // A vehicle reconnecting with the same system id starts clean, even when it is added before the old one is
    // removed. The old one is forgotten when it is destroyed.
    Vehicle reconnected(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    GeoZoneVehicleMonitor *const reconnectedMonitor = monitor.addVehicle(&reconnected);
    QVERIFY((reconnectedMonitor != firstMonitor) && (reconnectedMonitor != secondMonitor));
    QVERIFY(reconnectedMonitor->nearby().isEmpty());
    nearbySpy.clear();
    first.reset();
    QCOMPARE(nearbySpy.count(), 1);
    QVERIFY(!monitor.activeView()->inside());
    monitor.setActiveVehicle(&reconnected);
    monitor.evaluate(&reconnected, inside);
    QCOMPARE(alertSpy.count(), 3);
    QVERIFY(monitor.activeView()->inside());

    // Removed vehicles are no longer evaluated, nor named once a single vehicle is left
    monitor.removeVehicle(&second);
    QVERIFY(!monitor.vehicleMonitor(&second));
    monitor.evaluate(&second, { center });
    QCOMPARE(alertSpy.count(), 3);
    monitor.evaluate(&reconnected, away);
    monitor.evaluate(&reconnected, inside);
    QCOMPARE(alertSpy.count(), 4);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Entered geozone P (Prohibited)"));
    monitor.removeVehicle(&reconnected);
    QVERIFY(!monitor.monitoring());
    QVERIFY(!monitor.activeView()->inside());
}

void GeoZoneMonitorTest::_testAlertArbiter()
{
    qint64 now = 0;
    GeoZoneAlertArbiter arbiter([&now]() { return now; });

    // The alerts of a tick are one message, the most severe first, naming two and counting the others
    arbiter.submit(arbiterAlert(1, 1, QStringLiteral("U"), GeoZone::USpace, true));
    arbiter.submit(arbiterAlert(2, 1, QStringLiteral("A"), GeoZone::ReqAuthorisation, false));
    arbiter.submit(arbiterAlert(3, 2, QStringLiteral("P"), GeoZone::Prohibited, false));
    arbiter.submit(arbiterAlert(4, 1, QStringLiteral("C"), GeoZone::Conditional, true));
    const GeoZoneAlertArbiter::Announcement announcement = arbiter.tick();
    QCOMPARE(announcement.text, QStringLiteral("Approaching P. Approaching A, and 2 more"));
    QCOMPARE(announcement.alerts.count(), 4);
    QCOMPARE(announcement.alerts[2].id, 4);
    QCOMPARE(announcement.alerts[3].id, 1);
    QVERIFY(arbiter.tick().text.isEmpty());

    // Sooner than the minimum interval, alerts as urgent or less wait and are combined
    now += 1000;
    arbiter.submit(arbiterAlert(5, 1, QStringLiteral("C2"), GeoZone::Conditional, false));
    QVERIFY(arbiter.tick().text.isEmpty());
    now += 1000;
    arbiter.submit(arbiterAlert(6, 2, QStringLiteral("U2"), GeoZone::USpace, false));
    arbiter.submit(arbiterAlert(7, 2, QStringLiteral("P2"), GeoZone::Prohibited, false));
    QVERIFY(arbiter.tick().text.isEmpty());
    QCOMPARE(arbiter.pendingCount(), 3);
    arbiter.acknowledge(7);
    now = GeoZoneAlertArbiter::minIntervalMSecs;
    QCOMPARE(arbiter.tick().text, QStringLiteral("Approaching C2. Approaching U2"));

    // A more urgent alert preempts the interval
    now += 1000;
    arbiter.submit(arbiterAlert(8, 1, QStringLiteral("A2"), GeoZone::ReqAuthorisation, true));
    QCOMPARE(arbiter.tick().text, QStringLiteral("Entered A2"));
    now += 1000;
    arbiter.submit(arbiterAlert(9, 1, QStringLiteral("A3"), GeoZone::ReqAuthorisation, true));
    QVERIFY(arbiter.tick().text.isEmpty());
    arbiter.acknowledge(9);
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    QVERIFY(arbiter.tick().text.isEmpty());

    // Entering a prohibited zone is repeated until it is acknowledged
    arbiter.submit(arbiterAlert(10, 1, QStringLiteral("P"), GeoZone::Prohibited, true));
    QCOMPARE(arbiter.tick().text, QStringLiteral("Entered P"));
    QCOMPARE(arbiter.breachCount(), 1);
    arbiter.setInsideZones(1, { QStringLiteral("P") });
    now += GeoZoneAlertArbiter::repeatIntervalMSecs - 1;
    QVERIFY(arbiter.tick().text.isEmpty());
    now += 1;
    const GeoZoneAlertArbiter::Announcement repeat = arbiter.tick();
    QCOMPARE(repeat.text, QStringLiteral("Still inside P"));
    QCOMPARE(repeat.repeated.count(), 1);
    QCOMPARE(repeat.repeated.first().id, 10);
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    QCOMPARE(arbiter.tick().text, QStringLiteral("Still inside P"));
    arbiter.acknowledge(10);
    QCOMPARE(arbiter.breachCount(), 0);
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    QVERIFY(arbiter.tick().text.isEmpty());

    // ... or the vehicle leaves the zone
    arbiter.submit(arbiterAlert(11, 2, QStringLiteral("P"), GeoZone::Prohibited, true));
    QCOMPARE(arbiter.tick().text, QStringLiteral("Entered P"));
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    arbiter.setInsideZones(2, { QStringLiteral("P"), QStringLiteral("U") });
    QCOMPARE(arbiter.tick().text, QStringLiteral("Still inside P"));
    arbiter.setInsideZones(2, { QStringLiteral("U") });
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    QVERIFY(arbiter.tick().text.isEmpty());
    QCOMPARE(arbiter.breachCount(), 0);

    // Muted, alerts are dropped, but the first announcement of a new breach is made
    arbiter.mute();
    QVERIFY(arbiter.muted());
    QCOMPARE(arbiter.mutedUntil(), now + GeoZoneAlertArbiter::defaultMuteMSecs);
    const qint64 mutedUntil = arbiter.mutedUntil();
    arbiter.submit(arbiterAlert(12, 1, QStringLiteral("A4"), GeoZone::ReqAuthorisation, true));
    QVERIFY(arbiter.tick().text.isEmpty());
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    arbiter.submit(arbiterAlert(13, 1, QStringLiteral("C3"), GeoZone::Conditional, true));
    arbiter.submit(arbiterAlert(14, 1, QStringLiteral("P3"), GeoZone::Prohibited, true));
    const GeoZoneAlertArbiter::Announcement mutedBreach = arbiter.tick();
    QCOMPARE(mutedBreach.text, QStringLiteral("Entered P3"));
    QCOMPARE(mutedBreach.alerts.count(), 1);
    arbiter.setInsideZones(1, { QStringLiteral("P3") });

    // Its repeats wait for the end of the mute, the alerts dropped meanwhile aren't announced
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    QVERIFY(arbiter.tick().text.isEmpty());
    now = mutedUntil;
    QVERIFY(!arbiter.muted());
    QCOMPARE(arbiter.tick().text, QStringLiteral("Still inside P3"));

    // A vehicle no longer monitored is forgotten
    arbiter.removeVehicle(1);
    QCOMPARE(arbiter.breachCount(), 0);
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    QVERIFY(arbiter.tick().text.isEmpty());

    // The breaches of a vehicle whose position is lost are kept but not repeated until it is back
    arbiter.submit(arbiterAlert(15, 3, QStringLiteral("P4"), GeoZone::Prohibited, true));
    QCOMPARE(arbiter.tick().text, QStringLiteral("Entered P4"));
    arbiter.setSuspended(3, true);
    QVERIFY(arbiter.suspended(3));
    now += GeoZoneAlertArbiter::repeatIntervalMSecs * 2;
    QVERIFY(arbiter.tick().text.isEmpty());
    QCOMPARE(arbiter.breachCount(), 1);
    arbiter.setSuspended(3, false);
    QCOMPARE(arbiter.tick().text, QStringLiteral("Still inside P4"));
    arbiter.removeVehicle(3);

    // A zone muted for a vehicle drops its alerts and breach until the vehicle leaves it
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    arbiter.submit(arbiterAlert(16, 4, QStringLiteral("P5"), GeoZone::Prohibited, true));
    arbiter.submit(arbiterAlert(17, 5, QStringLiteral("P5"), GeoZone::Prohibited, true));
    arbiter.muteZone(4, QStringLiteral("P5"));
    QVERIFY(arbiter.zoneMuted(4, QStringLiteral("P5")));
    QVERIFY(!arbiter.zoneMuted(5, QStringLiteral("P5")));
    QCOMPARE(arbiter.pendingCount(), 1);
    QCOMPARE(arbiter.breachCount(), 1);
    arbiter.submit(arbiterAlert(18, 4, QStringLiteral("P5"), GeoZone::Prohibited, true));
    QCOMPARE(arbiter.pendingCount(), 1);
    arbiter.setInsideZones(4, { QStringLiteral("P5") });
    QCOMPARE(arbiter.mutedZones(4), QSet<QString>{ QStringLiteral("P5") });
    QCOMPARE(arbiter.zoneMuteCount(), 1);
    arbiter.setInsideZones(4, {});
    QVERIFY(!arbiter.zoneMuted(4, QStringLiteral("P5")));
    QCOMPARE(arbiter.zoneMuteCount(), 0);
    arbiter.submit(arbiterAlert(19, 4, QStringLiteral("P5"), GeoZone::Prohibited, true));
    QCOMPARE(arbiter.pendingCount(), 2);
}

void GeoZoneMonitorTest::_testSpokenAlerts()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("C"), QStringLiteral("CONDITIONAL"), center, 500),
        _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(100, 0), 50),
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(100, 180), 50),
    }), errorString), qPrintable(errorString));

    // The monitor speaks with the sink and clock given, using the settings given rather than the application's
    GeoZoneMonitor monitor(&manager);
    qint64 now = 0;
    monitor.setClock([&now]() { return now; });
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);
    GeoZoneMonitor::Settings settings;
    settings.limits.distance = 100;
    settings.limits.verticalDistance = 30;
    monitor.setSettings(settings);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    QSignalSpy repeatSpy(&monitor, &GeoZoneMonitor::alertRepeated);
    QSignalSpy mutedSpy(&monitor, &GeoZoneMonitor::alertsMutedChanged);

    // The alerts of an evaluation are shown one by one, but spoken as one message, the most severe first
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 3);
    QCOMPARE(sink->spoken, QStringList{ QStringLiteral("Approaching geozone P (Prohibited). Approaching geozone A (Authorisation required), and 1 more") });

    // Entering the prohibited zone right after is more urgent and spoken at once
    const GeoZone::Position inProhibited{ center.atDistanceAndAzimuth(100, 180) };
    const GeoZone::Position away{ center.atDistanceAndAzimuth(5000, 0) };
    now += 1000;
    monitor.evaluate(inProhibited);
    QCOMPARE(alertSpy.count(), 4);
    QCOMPARE(sink->spoken.last(), QStringLiteral("Entered geozone P (Prohibited)"));
    const int breachId = alertSpy.last()[2].toInt();
    QVERIFY(breachId > alertSpy.first()[2].toInt());

    // Repeated and shown again while the vehicle stays inside, until it is acknowledged
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(sink->spoken.count(), 3);
    QCOMPARE(sink->spoken.last(), QStringLiteral("Still inside geozone P (Prohibited)"));
    QCOMPARE(repeatSpy.count(), 1);
    QCOMPARE(repeatSpy.last()[0].toString(), sink->spoken.last());
    QCOMPARE(repeatSpy.last()[2].toInt(), breachId);
    QCOMPARE(alertSpy.count(), 4);
    monitor.acknowledgeAlert(breachId);
    GeoZoneAlertHistory *const history = monitor.alertHistory();
    QVERIFY(history->data(history->index(0), GeoZoneAlertHistory::AcknowledgedRole).toBool());
    QCOMPARE(history->unacknowledgedCount(), 3);
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(sink->spoken.count(), 3);

    // Acknowledging in the history ends the repeats too
    monitor.evaluate(away);
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(sink->spoken.last(), QStringLiteral("Entered geozone P (Prohibited). Entered geozone C (Conditional)"));
    history->acknowledgeAll();
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(sink->spoken.count(), 4);

    // Muted, approaches and entries aren't spoken, but a new entry into a prohibited zone is
    monitor.muteAlerts();
    QVERIFY(monitor.alertsMuted());
    QCOMPARE(mutedSpy.count(), 1);
    monitor.evaluate(away);
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 9);
    QCOMPARE(sink->spoken.count(), 4);
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(alertSpy.count(), 10);
    QCOMPARE(sink->spoken.count(), 5);
    QCOMPARE(sink->spoken.last(), QStringLiteral("Entered geozone P (Prohibited)"));

    // Its repeats wait for the end of the mute
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(sink->spoken.count(), 5);
    now += GeoZoneAlertArbiter::defaultMuteMSecs;
    monitor.evaluate(inProhibited);
    QVERIFY(!monitor.alertsMuted());
    QCOMPARE(mutedSpy.count(), 2);
    QCOMPARE(sink->spoken.last(), QStringLiteral("Still inside geozone P (Prohibited)"));

    // Removing the vehicle ends its repeats
    monitor.removePositionMonitor(GeoZoneMonitor::noVehicle);
    QCOMPARE(monitor.arbiter().breachCount(), 0);

    // With the audio turned off, alerts are shown but not spoken, and a breach is still repeated until acknowledged
    settings.audioAlerts = false;
    monitor.setSettings(settings);
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(alertSpy.count(), 12);
    QCOMPARE(sink->spoken.count(), 6);
    const int silentBreachId = alertSpy[10][2].toInt();
    QCOMPARE(alertSpy[10][0].toString(), QStringLiteral("Entered geozone P (Prohibited)"));
    const qsizetype repeatCount = repeatSpy.count();
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(repeatSpy.count(), repeatCount + 1);
    QCOMPARE(repeatSpy.last()[2].toInt(), silentBreachId);
    QCOMPARE(sink->spoken.count(), 6);
    monitor.acknowledgeAlert(silentBreachId);
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(repeatSpy.count(), repeatCount + 1);
}

void GeoZoneMonitorTest::_testPositionLoss()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("C"), QStringLiteral("CONDITIONAL"), center.atDistanceAndAzimuth(3000, 90), 300),
    }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    qint64 now = 0;
    monitor.setClock([&now]() { return now; });
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    QSignalSpy repeatSpy(&monitor, &GeoZoneMonitor::alertRepeated);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
    GeoZoneActiveVehicleView *const view = monitor.activeView();

    // An offline vehicle has no position of its own, like a vehicle whose position is lost
    Vehicle vehicle(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    GeoZoneVehicleMonitor *const vehicleMonitor = monitor.addVehicle(&vehicle);
    monitor.setActiveVehicle(&vehicle);
    QCOMPARE(view->vehicleId(), vehicle.id());
    const GeoZone::Position inside{ center.atDistanceAndAzimuth(100, 0), 450, 400 };
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Entered geozone P (Prohibited)"));
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(repeatSpy.count(), 1);

    // Losing the position is alerted once with the zone the vehicle was in, and the breach isn't repeated while the
    // vehicle may have left the zone. Its nearby zones and bands are kept as stale.
    QSignalSpy unavailableSpy(view, &GeoZoneActiveVehicleView::positionUnavailableChanged);
    now += 1000;
    monitor.evaluateVehicles();
    QVERIFY(vehicleMonitor->positionUnavailable());
    QVERIFY(view->positionUnavailable());
    QCOMPARE(unavailableSpy.count(), 1);
    QVERIFY(monitor.arbiter().suspended(vehicleMonitor->key()));
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Position lost inside geozone P (Prohibited)"));
    QCOMPARE(alertSpy.last()[1].toInt(), GeoZone::restrictionSeverity(GeoZone::Prohibited));
    QCOMPARE(sink->spoken.last(), alertSpy.last()[0].toString());
    const GeoZoneMonitor::AlertEvent lost = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY(lost.kind == GeoZoneMonitor::AlertEvent::PositionLost);
    QVERIFY(lost.inside);
    QCOMPARE(lost.position.coordinate, inside.coordinate);
    QVERIFY(view->inside());
    QCOMPARE(view->verticalBands().count(), 1);
    QCOMPARE(view->predictedEntryTime(), -1);
    for (int i = 0; i < 3; i++) {
        now += GeoZoneAlertArbiter::repeatIntervalMSecs;
        monitor.evaluateVehicles();
    }
    QCOMPARE(repeatSpy.count(), 1);
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(monitor.arbiter().breachCount(), 1);

    // When the position is back the breach resumes at once if the vehicle is still inside, and ends when it leaves
    monitor.evaluate(&vehicle, inside);
    QVERIFY(!view->positionUnavailable());
    QVERIFY(!monitor.arbiter().suspended(vehicleMonitor->key()));
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(repeatSpy.count(), 2);
    QCOMPARE(sink->spoken.last(), QStringLiteral("Still inside geozone P (Prohibited)"));
    monitor.evaluate(&vehicle, { center.atDistanceAndAzimuth(1000, 0), 450, 400 });
    QCOMPARE(monitor.arbiter().breachCount(), 0);

    // Far from any zone, nothing is alerted
    monitor.evaluate(&vehicle, { center.atDistanceAndAzimuth(10000, 0), 450, 400 });
    QVERIFY(view->nearbyZones().isEmpty());
    const qsizetype alertCount = alertSpy.count();
    monitor.evaluateVehicles();
    QVERIFY(view->positionUnavailable());
    QCOMPARE(alertSpy.count(), alertCount);

    // Positions passed to evaluate() are lost on request, near a zone
    monitor.evaluate({ center.atDistanceAndAzimuth(3350, 90) }, 7);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Vehicle 7: Approaching geozone C (Conditional)"));
    monitor.positionLost(7);
    monitor.positionLost(7);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Vehicle 7: Position lost near geozone C (Conditional)"));
    QCOMPARE(alertSpy.count(), alertCount + 2);
    QVERIFY(monitor.positionMonitor(7)->positionUnavailable());
    monitor.evaluate({ center.atDistanceAndAzimuth(3350, 90) }, 7);
    QVERIFY(!monitor.positionMonitor(7)->positionUnavailable());
    QCOMPARE(alertSpy.count(), alertCount + 2);

    // The alert is logged and read back
    const GeoZoneMonitor::AlertEvent nearLost = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>();
    const QString line = GeoZoneAlertLog::line(nearLost);
    QCOMPARE(line.section(QLatin1Char(','), 6, 6), QStringLiteral("position_lost"));
    const std::optional<GeoZoneMonitor::AlertEvent> read = GeoZoneAlertHistory::parseLogLine(line);
    QVERIFY(read);
    QVERIFY(read->kind == GeoZoneMonitor::AlertEvent::PositionLost);
    QVERIFY(!read->inside);
    QCOMPARE(read->message, QStringLiteral("Position lost near geozone C (Conditional)"));
    QVERIFY(GeoZoneAlertHistory::parseLogLine(GeoZoneAlertLog::line(lost))->inside);
}

void GeoZoneMonitorTest::_testAlertActions()
{
    // A prohibited zone up to 120 m above the ground, and U-space airspace without a ceiling
    const QGeoCoordinate center(47, 8);
    QJsonObject prohibited = _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300);
    QJsonArray geometry = prohibited[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = 0;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AGL");
    layer[QStringLiteral("upperLimit")] = 120;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AGL");
    geometry[0] = layer;
    prohibited[QStringLiteral("geometry")] = geometry;
    const QGeoCoordinate uSpaceCenter = center.atDistanceAndAzimuth(5000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        prohibited,
        _circleFeature(QStringLiteral("U"), QStringLiteral("USPACE"), uSpaceCenter, 300),
    }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    qint64 now = 0;
    monitor.setClock([&now]() { return now; });
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    QSignalSpy repeatSpy(&monitor, &GeoZoneMonitor::alertRepeated);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
    QSignalSpy mutesSpy(&monitor, &GeoZoneMonitor::zoneMutesChanged);
    Vehicle vehicle(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    (void) monitor.addVehicle(&vehicle);
    monitor.setActiveVehicle(&vehicle);
    QVERIFY(monitor.alertActions(1).isEmpty());

    // 50 m above the ground inside the zone, the active vehicle climbs 100 m to be 30 m above its ceiling
    const GeoZone::Position inside{ center.atDistanceAndAzimuth(100, 0), 450, 400 };
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(alertSpy.count(), 1);
    const int entryId = alertSpy.last()[2].toInt();
    QVariantMap actions = monitor.alertActions(entryId);
    QCOMPARE(actions.value(QStringLiteral("vehicleId")).toInt(), vehicle.id());
    QCOMPARE(actions.value(QStringLiteral("zoneKey")).toString(), eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().zoneKey);
    QVERIFY(actions.value(QStringLiteral("activeVehicle")).toBool());
    QVERIFY(actions.value(QStringLiteral("inside")).toBool());
    QVERIFY(!actions.value(QStringLiteral("zoneMuted")).toBool());
    QCOMPARE(actions.value(QStringLiteral("climbHeight")).toDouble(), 120 + GeoZoneMonitor::climbClearance - 50);
    monitor.setActiveVehicle(nullptr);
    QVERIFY(!monitor.alertActions(entryId).value(QStringLiteral("activeVehicle")).toBool());
    monitor.setActiveVehicle(&vehicle);

    // Muting the zone ends its repeats until the vehicle leaves it
    mutesSpy.clear();
    monitor.muteZone(entryId);
    QCOMPARE(mutesSpy.count(), 1);
    QCOMPARE(monitor.mutedZoneKeys(), QStringList{ eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().zoneKey });
    QVERIFY(monitor.alertActions(entryId).value(QStringLiteral("zoneMuted")).toBool());
    QCOMPARE(monitor.arbiter().breachCount(), 0);
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(repeatSpy.count(), 0);
    monitor.evaluate(&vehicle, { center.atDistanceAndAzimuth(1000, 0), 450, 400 });
    QVERIFY(monitor.mutedZoneKeys().isEmpty());
    QCOMPARE(mutesSpy.count(), 2);
    QVERIFY(!monitor.alertActions(entryId).value(QStringLiteral("inside")).toBool());
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(monitor.arbiter().breachCount(), 1);

    // Without a current position the vehicle isn't known to be inside nor able to climb out
    monitor.evaluateVehicles();
    actions = monitor.alertActions(alertSpy.last()[2].toInt());
    QVERIFY(!actions.value(QStringLiteral("inside")).toBool());
    QVERIFY(std::isnan(actions.value(QStringLiteral("climbHeight")).toDouble()));

    // Nor above zones without a ceiling
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    monitor.evaluate(&vehicle, { uSpaceCenter, 450, 400 });
    QCOMPARE(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().zoneIdentifier, QStringLiteral("U"));
    actions = monitor.alertActions(alertSpy.last()[2].toInt());
    QVERIFY(actions.value(QStringLiteral("inside")).toBool());
    QVERIFY(std::isnan(actions.value(QStringLiteral("climbHeight")).toDouble()));

    // A vehicle no longer monitored has no actions
    monitor.removeVehicle(&vehicle);
    QVERIFY(monitor.alertActions(entryId).isEmpty());
}

void GeoZoneMonitorTest::_testTraffic()
{
    // No terrain data, a prohibited zone at any height and one up to 120 m above the ground
    const ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate lowCenter = center.atDistanceAndAzimuth(3000, 270);
    QJsonObject low = _circleFeature(QStringLiteral("LOW"), QStringLiteral("REQ_AUTHORISATION"), lowCenter, 300);
    QJsonArray geometry = low[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = 0;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AGL");
    layer[QStringLiteral("upperLimit")] = 120;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AGL");
    geometry[0] = layer;
    low[QStringLiteral("geometry")] = geometry;
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300), low,
    }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);
    GeoZoneMonitor::Settings settings;
    settings.limits.distance = 100;
    settings.limits.verticalDistance = 30;
    settings.traffic.enabled = true;
    monitor.setSettings(settings);
    GeoZoneTrafficMonitor *const traffic = monitor.traffic();
    QVERIFY(traffic->enabled());
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);

    const auto aircraft = [](quint32 address, const QString &callsign, const QGeoCoordinate &coordinate, double altitude) {
        GeoZoneTrafficMonitor::Aircraft plane;
        plane.address = address;
        plane.callsign = callsign;
        plane.coordinate = QGeoCoordinate(coordinate.latitude(), coordinate.longitude(), altitude);
        return plane;
    };
    const GeoZoneTrafficMonitor::Aircraft inP = aircraft(0x4B1234, QStringLiteral("SWR123 ") + QChar(0), center.atDistanceAndAzimuth(100, 90), 600);
    const GeoZoneTrafficMonitor::Aircraft nearP = aircraft(0x4B5678, QString(), center.atDistanceAndAzimuth(360, 90), 700);
    const GeoZoneTrafficMonitor::Aircraft far = aircraft(0x4B9999, QStringLiteral("FAR"), center.atDistanceAndAzimuth(20000, 0), 600);

    // Traffic is checked around the active vehicle, here 5 km north of the prohibited zone, within the radius
    monitor.evaluateTraffic({ inP });
    QCOMPARE(traffic->count(), 0);
    monitor.evaluate({ center.atDistanceAndAzimuth(5000, 0), 500, 400 });
    QCOMPARE(alertSpy.count(), 0);
    QSignalSpy trafficSpy(traffic, &GeoZoneTrafficMonitor::aircraftChanged);
    monitor.evaluateTraffic({ far, nearP, inP });
    QCOMPARE(trafficSpy.count(), 1);
    QCOMPARE(traffic->count(), 2);
    const QVariantList listed = traffic->aircraft();
    QCOMPARE(listed[0].toMap().value(QStringLiteral("callsign")).toString(), QStringLiteral("SWR123"));
    QVERIFY(listed[0].toMap().value(QStringLiteral("inside")).toBool());
    QCOMPARE(listed[0].toMap().value(QStringLiteral("altitude")).toDouble(), 600.0);
    QCOMPARE(GeoZoneInfo::fromVariant(listed[0].toMap().value(QStringLiteral("zone")))->identifier(), QStringLiteral("P"));
    QCOMPARE(listed[1].toMap().value(QStringLiteral("callsign")).toString(), QStringLiteral("4B5678"));
    QVERIFY(!listed[1].toMap().value(QStringLiteral("inside")).toBool());
    QCOMPARE(listed[1].toMap().value(QStringLiteral("distance")).toInt(), 60);

    // The aircraft entering the zone is shown, less urgent than the vehicles, but not spoken by default
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Traffic SWR123 entered geozone P (Prohibited)"));
    QCOMPARE(alertSpy.last()[1].toInt(), GeoZoneMonitor::trafficSeverity + GeoZone::restrictionSeverity(GeoZone::Prohibited));
    QVERIFY(sink->spoken.isEmpty());
    const GeoZoneMonitor::AlertEvent event = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY(event.kind == GeoZoneMonitor::AlertEvent::Traffic);
    QCOMPARE(event.traffic, QStringLiteral("SWR123"));
    QCOMPARE(event.vehicleId, GeoZoneMonitor::noVehicle);
    QVERIFY(monitor.alertActions(event.id).isEmpty());
    const std::optional<GeoZoneMonitor::AlertEvent> read = GeoZoneAlertHistory::parseLogLine(GeoZoneAlertLog::line(event));
    QVERIFY(read && (read->kind == GeoZoneMonitor::AlertEvent::Traffic));
    QCOMPARE(read->traffic, QStringLiteral("SWR123"));
    QCOMPARE(read->message, event.message);

    // Once per entry, and aircraft gone are forgotten
    monitor.evaluateTraffic({ inP });
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(traffic->count(), 1);

    // Spoken when asked, an aircraft coming back enters again
    settings.traffic.alerts = GeoZoneTrafficMonitor::SpeakAlerts;
    monitor.setSettings(settings);
    monitor.evaluateTraffic({ inP, aircraft(0x4B5678, QString(), center, 700) });
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(sink->spoken, QStringList{ QStringLiteral("Traffic 4B5678 entered geozone P (Prohibited)") });

    // Without terrain data below an aircraft, the ground below the vehicle is used for limits above the ground
    settings.traffic.alerts = GeoZoneTrafficMonitor::NoAlerts;
    monitor.setSettings(settings);
    monitor.evaluateTraffic({ aircraft(1, QStringLiteral("HIGH"), lowCenter, 600), aircraft(2, QStringLiteral("LOW"), lowCenter, 450) });
    QCOMPARE(traffic->count(), 1);
    QCOMPARE(traffic->aircraft().first().toMap().value(QStringLiteral("callsign")).toString(), QStringLiteral("LOW"));
    QCOMPARE(alertSpy.count(), 2);

    // Turned off, the traffic is forgotten
    settings.traffic.enabled = false;
    monitor.setSettings(settings);
    QVERIFY(!traffic->enabled());
    QCOMPARE(traffic->count(), 0);
    monitor.evaluateTraffic({ inP });
    QCOMPARE(traffic->count(), 0);
}
