/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMonitorTest.h"
#include "GeoZoneTestHelpers.h"

using namespace GeoZoneTestHelpers;

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

    // The list model updates distances in place
    QSignalSpy resetSpy(monitor.nearbyModel(), &QAbstractItemModel::modelReset);
    QSignalSpy dataChangedSpy(monitor.nearbyModel(), &QAbstractItemModel::dataChanged);
    QCOMPARE(monitor.nearbyModel()->count(), 1);

    // Jitter across the boundary and the approach distance does not alert again
    monitor.evaluate({ center.atDistanceAndAzimuth(310, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(290, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(410, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(390, 0) });
    QCOMPARE(resetSpy.count(), 0);
    QVERIFY(dataChangedSpy.count() >= 3);
    QCOMPARE(monitor.nearbyModel()->data(monitor.nearbyModel()->index(0), GeoZoneNearbyModel::DistanceRole).toInt(), 90);
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
    QCOMPARE(monitor.predictedEntryTime(), -1);
    QVERIFY(monitor.predictedPath().isEmpty());

    // At 25 m/s the boundary 700 m ahead is reached in 28 s, within the approach time
    QSignalSpy predictionSpy(&monitor, &GeoZoneMonitor::predictionChanged);
    monitor.evaluate(start, GeoZoneMonitor::noVehicle, north);
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Approaching geozone P (Prohibited), entry in about 28 s"));
    // The map shows the path to the entry with the time left
    QVERIFY(predictionSpy.count() > 0);
    QCOMPARE(monitor.predictedEntryTime(), 28);
    QVERIFY(qAbs(monitor.predictedEntry().distanceTo(center.atDistanceAndAzimuth(300, 180)) - 0) < 2);
    QCOMPARE(monitor.predictedPath().count(), 2);
    QCOMPARE(monitor.predictedPath().first().value<QGeoCoordinate>(), start.coordinate);
    QCOMPARE(GeoZoneInfo::fromVariant(monitor.predictedEntryZone())->identifier(), QStringLiteral("P"));
    QCOMPARE(monitor.nearbyZones().count(), 1);
    QCOMPARE(monitor.nearbyZones()[0].toMap()[QStringLiteral("timeToEntry")].toInt(), 28);
    QCOMPARE(monitor.nearbyZones()[0].toMap()[QStringLiteral("distance")].toInt(), 700);

    // Too slow to arrive within the approach time, and not repeated while it stays near
    alertSpy.clear();
    monitor.evaluate(start, GeoZoneMonitor::noVehicle, GeoZoneMonitor::Velocity{ 10, 0, 0, true });
    QVERIFY(monitor.nearbyZones().isEmpty());
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
    QSignalSpy bandsSpy(&climbMonitor, &GeoZoneMonitor::verticalBandsChanged);
    climbMonitor.evaluate({ above, 400, 300 });
    QCOMPARE(climbMonitor.altitudeAMSL(), 400.0);
    QCOMPARE(climbMonitor.terrainAMSL(), 300.0);
    QCOMPARE(climbMonitor.verticalBands().count(), 1);
    const QVariantMap band = climbMonitor.verticalBands().first().toMap();
    QCOMPARE(band[QStringLiteral("name")].toString(), QStringLiteral("HIGH"));
    QCOMPARE(band[QStringLiteral("floor")].toDouble(), 500.0);
    QVERIFY(std::isnan(band[QStringLiteral("ceiling")].toDouble()));
    const qsizetype bandSignals = bandsSpy.count();
    climbMonitor.evaluate({ above, 400.2, 300 });
    QCOMPARE(bandsSpy.count(), bandSignals);
    climbMonitor.evaluate({ center.atDistanceAndAzimuth(5000, 270), 400, 300 });
    QVERIFY(climbMonitor.verticalBands().isEmpty());

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

    // Paths get the terrain where it is known, positions without terrain keep the previous one elsewhere
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

    // Without terrain tiles the ground below the vehicle is used, which puts the zone below the vehicle
    environment.setTerrain(qQNaN());
    GeoZoneMonitor noTerrain(&manager);
    QSignalSpy noTerrainSpy(&noTerrain, &GeoZoneMonitor::alert);
    noTerrain.evaluate({ valley, 450, 100 }, GeoZoneMonitor::noVehicle, GeoZoneMonitor::Velocity{ 40, 0, 0, true });
    QCOMPARE(noTerrainSpy.count(), 0);
}

void GeoZoneMonitorTest::_testVehicleTelemetry()
{
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    GeoZoneMonitor monitor(&manager);
    const int vehicleId = 1;
    const QGeoCoordinate start(47, 8);
    const auto globalPosition = [](int16_t vx, int16_t vy, int16_t vz) {
        mavlink_message_t message{};
        (void) mavlink_msg_global_position_int_pack(vehicleId, MAV_COMP_ID_AUTOPILOT1, &message, 0, 470000000, 80000000, 500000, 100000, vx, vy, vz, UINT16_MAX);
        return message;
    };

    // The velocity sent with the position is used while recent, north east up from north east down
    monitor.handleVehicleMessage(vehicleId, globalPosition(2500, -500, 300), 1000);
    GeoZoneMonitor::Velocity velocity = monitor.vehicleVelocity(vehicleId, 1500, { start, 500 });
    QVERIFY(velocity.valid);
    QCOMPARE(velocity.north, 25.0);
    QCOMPARE(velocity.east, -5.0);
    QCOMPARE(velocity.up, -3.0);

    // Old velocities give way to the positions, which haven't moved
    velocity = monitor.vehicleVelocity(vehicleId, 1000 + GeoZoneMonitor::reportedVelocityMaxAgeMSecs + 1500, { start, 500 });
    QVERIFY(velocity.valid);
    QCOMPARE(velocity.groundSpeed(), 0.0);

    // Vehicles sending zero velocities while their positions move are estimated
    GeoZoneMonitor estimating(&manager);
    estimating.handleVehicleMessage(vehicleId, globalPosition(0, 0, 0), 0);
    (void) estimating.vehicleVelocity(vehicleId, 0, { start, 500 });
    estimating.handleVehicleMessage(vehicleId, globalPosition(0, 0, 0), 1000);
    velocity = estimating.vehicleVelocity(vehicleId, 1000, { start.atDistanceAndAzimuth(20, 90), 500 });
    QVERIFY(qAbs(velocity.east - 20) < 0.1);

    // Positions are stale after the timeout, or three of their usual intervals on slow links
    QVERIFY(!monitor.positionStale(vehicleId, 1000 + GeoZoneMonitor::positionTimeoutMSecs));
    QVERIFY(monitor.positionStale(vehicleId, 1000 + GeoZoneMonitor::positionTimeoutMSecs + 1));
    monitor.handleVehicleMessage(vehicleId, globalPosition(0, 0, 0), 21000);
    QVERIFY(!monitor.positionStale(vehicleId, 21000 + 50000));
    QVERIFY(monitor.positionStale(vehicleId, 21000 + 60001));
    QVERIFY(!monitor.positionStale(2, 100000));
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
    QCOMPARE(monitor.nearbyModel()->count(), 1);
    monitor.setActiveVehicleId(2);
    QCOMPARE(nearbySpy.count(), 1);
    QVERIFY(monitor.nearbyZones().isEmpty());
    QCOMPARE(monitor.nearbyModel()->count(), 0);
    QVERIFY(!monitor.inside());
    monitor.setActiveVehicleId(1);
    QCOMPARE(monitor.nearbyModel()->count(), 1);
    QVERIFY(monitor.inside());
    monitor.setActiveVehicleId(2);

    // A removed vehicle alerts again when it reconnects, the others keep their state
    monitor.removeVehicle(1);
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
    const int vehicleId = 1;
    monitor.setActiveVehicleId(vehicleId);
    const auto fenceStatus = [](uint8_t breachStatus, uint16_t breachCount, uint8_t breachType) {
        mavlink_message_t message{};
        (void) mavlink_msg_fence_status_pack(vehicleId, MAV_COMP_ID_AUTOPILOT1, &message, breachStatus, breachCount, breachType, 0, FENCE_MITIGATE_NONE);
        return message;
    };
    QSignalSpy breachSpy(&monitor, &GeoZoneMonitor::fenceBreach);
    QSignalSpy statusSpy(&monitor, &GeoZoneMonitor::fenceStatusChanged);

    // Not breached
    QVERIFY(!monitor.fenceStatusKnown());
    monitor.handleVehicleMessage(vehicleId, fenceStatus(0, 0, FENCE_BREACH_NONE), 0);
    QVERIFY(monitor.fenceStatusKnown());
    QVERIFY(!monitor.fenceBreached());
    QCOMPARE(statusSpy.count(), 1);

    // A breach is announced once while it lasts
    monitor.handleVehicleMessage(vehicleId, fenceStatus(1, 1, FENCE_BREACH_BOUNDARY), 1000);
    monitor.handleVehicleMessage(vehicleId, fenceStatus(1, 1, FENCE_BREACH_BOUNDARY), 2000);
    QVERIFY(monitor.fenceBreached());
    QCOMPARE(monitor.fenceBreachText(), QStringLiteral("Boundary fence breached"));
    QCOMPARE(breachSpy.count(), 1);
    QCOMPARE(breachSpy.first().first().toString(), QStringLiteral("Boundary fence breached"));
    QCOMPARE(statusSpy.count(), 2);

    // Cleared, then breached again at the maximum altitude
    monitor.handleVehicleMessage(vehicleId, fenceStatus(0, 1, FENCE_BREACH_NONE), 3000);
    QVERIFY(!monitor.fenceBreached());
    QVERIFY(monitor.fenceBreachText().isEmpty());
    monitor.handleVehicleMessage(vehicleId, fenceStatus(1, 2, FENCE_BREACH_MAXALT), 4000);
    QCOMPARE(breachSpy.count(), 2);
    QCOMPARE(monitor.fenceBreachText(), QStringLiteral("Maximum altitude fence breached (2 time(s))"));

    // Other vehicles don't change the status shown
    monitor.handleVehicleMessage(vehicleId + 1, fenceStatus(0, 0, FENCE_BREACH_NONE), 5000);
    QVERIFY(monitor.fenceBreached());
    monitor.setActiveVehicleId(vehicleId + 1);
    QVERIFY(!monitor.fenceBreached());
    monitor.removeVehicle(vehicleId + 1);
    QVERIFY(!monitor.fenceStatusKnown());
}
