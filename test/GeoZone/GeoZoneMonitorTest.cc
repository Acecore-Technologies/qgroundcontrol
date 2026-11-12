/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMonitorTest.h"
#include "GeoZoneAlertHistory.h"
#include "GeoZoneAlertLevel.h"
#include "GeoZoneFenceWarning.h"
#include "GeoZoneFences.h"
#include "GeoZoneNearbyModel.h"
#include "GeoZonePermits.h"
#include "GeoZonePreflightCheck.h"
#include "GeoZoneTestBase.h"
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

/// @return GPS_RAW_INT with the fix, satellites, HDOP and horizontal accuracy in meters (0 when unknown)
mavlink_message_t gpsMessage(int vehicleId, uint8_t fixType, uint8_t satellites, double hdop, double horizontalAccuracy)
{
    mavlink_message_t message{};
    (void) mavlink_msg_gps_raw_int_pack(vehicleId, MAV_COMP_ID_AUTOPILOT1, &message, 0, fixType, 470000000, 80000000, 500000, static_cast<uint16_t>(hdop * 100),
                                        UINT16_MAX, UINT16_MAX, UINT16_MAX, satellites, 0, static_cast<uint32_t>(horizontalAccuracy * 1000), 0, 0, 0, 0);
    return message;
}

mavlink_message_t gpsRawIntMessage(int vehicleId, uint16_t hdop, uint32_t horizontalAccuracyMM, uint32_t verticalAccuracyMM)
{
    mavlink_message_t message{};
    (void) mavlink_msg_gps_raw_int_pack(vehicleId, MAV_COMP_ID_AUTOPILOT1, &message, 0, GPS_FIX_TYPE_3D_FIX, 470000000, 80000000, 500000, hdop, UINT16_MAX,
                                        UINT16_MAX, UINT16_MAX, 10, 0, horizontalAccuracyMM, verticalAccuracyMM, 0, 0, 0);
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
    alert.level = GeoZoneMonitor::zoneLevel(restriction, inside, false);
    alert.severity = GeoZoneAlertLevel::severity(alert.level, GeoZone::restrictionSeverity(restriction));
    alert.inside = inside;
    alert.breach = inside && (restriction == GeoZone::Prohibited);
    alert.message = QStringLiteral("%1 %2").arg(inside ? QStringLiteral("Entered") : QStringLiteral("Approaching"), zone);
    alert.repeatMessage = QStringLiteral("Still inside %1").arg(zone);
    return alert;
}

/// @return Alert with the id shown by the alert popup, nothing when it isn't shown
std::optional<GeoZoneAlertModel::Alert> shownAlert(const GeoZoneMonitor &monitor, int alertId)
{
    for (const GeoZoneAlertModel::Alert &alert : monitor.alertModel()->alerts()) {
        if (alert.alertId == alertId) {
            return alert;
        }
    }
    return std::nullopt;
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
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);

    // Far away, then approaching, then inside, each alerting once
    monitor.evaluate({ center.atDistanceAndAzimuth(1000, 0) });
    QCOMPARE(alertSpy.count(), 0);
    monitor.evaluate({ center.atDistanceAndAzimuth(380, 0) });
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Approaching geozone P")));
    QCOMPARE(monitor.activeMonitor()->nearby().count(), 1);
    QVERIFY(!monitor.activeMonitor()->inside());
    QCOMPARE(monitor.activeMonitor()->nearby()[0].distance, 80);
    QCOMPARE(monitor.activeMonitor()->alertRestriction(), static_cast<int>(GeoZone::Prohibited));
    monitor.evaluate({ center.atDistanceAndAzimuth(350, 0) });
    QCOMPARE(alertSpy.count(), 1);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) });
    QCOMPARE(alertSpy.count(), 2);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Entered geozone P")));
    QVERIFY(monitor.activeMonitor()->inside());

    // The list model updates distances in place
    QSignalSpy resetSpy(monitor.activeMonitor()->nearbyModel(), &QAbstractItemModel::modelReset);
    QSignalSpy dataChangedSpy(monitor.activeMonitor()->nearbyModel(), &QAbstractItemModel::dataChanged);
    QCOMPARE(monitor.activeMonitor()->nearbyModel()->count(), 1);

    // Jitter across the boundary and the approach distance does not alert again
    monitor.evaluate({ center.atDistanceAndAzimuth(310, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(290, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(410, 0) });
    monitor.evaluate({ center.atDistanceAndAzimuth(390, 0) });
    QCOMPARE(resetSpy.count(), 0);
    QVERIFY(dataChangedSpy.count() >= 3);
    QCOMPARE(monitor.activeMonitor()->nearbyModel()->data(monitor.activeMonitor()->nearbyModel()->index(0), GeoZoneNearbyModel::DistanceRole).toInt(), 90);
    QCOMPARE(alertSpy.count(), 2);

    // Reloading changed zones doesn't repeat the approach alert of an unchanged zone
    (void) _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(3000, 270), 300),
        expired,
        _circleFeature(QStringLiteral("NEW"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(9000, 0), 300),
    });
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));
    QVERIFY(monitor.activeMonitor()->nearby().isEmpty());
    monitor.evaluate({ center.atDistanceAndAzimuth(390, 0) });
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(monitor.activeMonitor()->nearby().count(), 1);

    // Leaving the area resets the zone
    monitor.evaluate({ center.atDistanceAndAzimuth(1000, 0) });
    QVERIFY(monitor.activeMonitor()->nearby().isEmpty());
    QCOMPARE(monitor.activeMonitor()->alertRestriction(), -1);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) });
    QCOMPARE(alertSpy.count(), 3);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Entered geozone P")));

    // Unrestricted and inactive zones don't alert
    monitor.evaluate({ center.atDistanceAndAzimuth(3000, 270) });
    monitor.evaluate({ center.atDistanceAndAzimuth(3000, 90) });
    QCOMPARE(alertSpy.count(), 3);

    // Zones are forgotten before the manager deletes them
    manager.repository()->clear();
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 3);

    // U-space airspace alerts too, but a prohibited zone ahead decides the indicator color
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("uspace.json"),
        QJsonArray{ _circleFeature(QStringLiteral("U"), QStringLiteral("USPACE"), center.atDistanceAndAzimuth(6000, 0), 3000),
                    _circleFeature(QStringLiteral("P2"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(6380, 0), 300) }), errorString), qPrintable(errorString));
    alertSpy.clear();
    monitor.evaluate({ center.atDistanceAndAzimuth(5000, 0) });
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Entered geozone U (U-space airspace)")));
    QCOMPARE(monitor.activeMonitor()->alertRestriction(), static_cast<int>(GeoZone::USpace));
    monitor.evaluate({ center.atDistanceAndAzimuth(6000, 0) });
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(monitor.activeMonitor()->alertRestriction(), static_cast<int>(GeoZone::Prohibited));
    QVERIFY(monitor.activeMonitor()->inside());
    manager.repository()->clear();

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
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("elevated.json"), QJsonArray{ elevated }), errorString), qPrintable(errorString));
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
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 3);
    QVERIFY(alertSpy[0][0].toString().startsWith(QStringLiteral("Approaching geozone P")));
    QCOMPARE(alertSpy[0][1].toInt(), GeoZoneAlertLevel::severity(GeoZoneAlertLevel::Caution, GeoZone::restrictionSeverity(GeoZone::Prohibited)));
    QVERIFY(alertSpy[1][0].toString().startsWith(QStringLiteral("Approaching geozone A")));
    QVERIFY(alertSpy[2][0].toString().startsWith(QStringLiteral("Entered geozone C")));
}

void GeoZoneMonitorTest::_testMonitorTimeToEntry()
{
    // The thresholds of the default profile at 25 m/s warn 27 s ahead, the pilot adds 5 s
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    const auto restoreDefault = qScopeGuard([settings]() { settings->approachExtraTime()->setRawValue(settings->approachExtraTime()->rawDefaultValue()); });
    settings->approachExtraTime()->setRawValue(5);

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
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"),
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
    QCOMPARE(monitor.activeMonitor()->predictedEntryTime(), -1);
    QVERIFY(monitor.activeMonitor()->predictedPath().isEmpty());

    // At 25 m/s the boundary 700 m ahead is reached in 28 s, within the warning time of 32 s
    QSignalSpy predictionSpy(monitor.activeMonitor(), &GeoZoneVehicleMonitor::predictionChanged);
    monitor.evaluate(start, GeoZoneMonitor::noVehicle, north);
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Approaching geozone P (Prohibited), entry in about 28 s"));
    // The map shows the path to the entry with the time left
    QVERIFY(predictionSpy.count() > 0);
    QCOMPARE(monitor.activeMonitor()->predictedEntryTime(), 28);
    QVERIFY(qAbs(monitor.activeMonitor()->predictedEntry().distanceTo(center.atDistanceAndAzimuth(300, 180)) - 0) < 2);
    QCOMPARE(monitor.activeMonitor()->predictedPath().count(), 2);
    QCOMPARE(monitor.activeMonitor()->predictedPath().first().value<QGeoCoordinate>(), start.coordinate);
    QCOMPARE(GeoZoneInfo::fromVariant(monitor.activeMonitor()->predictedEntryZone())->identifier(), QStringLiteral("P"));
    QCOMPARE(monitor.activeMonitor()->nearby().count(), 1);
    QCOMPARE(monitor.activeMonitor()->nearby()[0].timeToEntry, 28);
    QCOMPARE(monitor.activeMonitor()->nearby()[0].distance, 700);

    // Too slow to arrive within the approach time, and not repeated while it stays near
    alertSpy.clear();
    monitor.evaluate(start, GeoZoneMonitor::noVehicle, GeoZoneMonitor::Velocity{ 10, 0, 0, true });
    QVERIFY(monitor.activeMonitor()->nearby().isEmpty());
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
    QSignalSpy bandsSpy(climbMonitor.activeMonitor(), &GeoZoneVehicleMonitor::verticalBandsChanged);
    climbMonitor.evaluate({ above, 400, 300 });
    QCOMPARE(climbMonitor.activeMonitor()->altitudeAMSL(), 400.0);
    QCOMPARE(climbMonitor.activeMonitor()->terrainAMSL(), 300.0);
    QCOMPARE(climbMonitor.activeMonitor()->verticalBands().count(), 1);
    const QVariantMap band = climbMonitor.activeMonitor()->verticalBands().first().toMap();
    QCOMPARE(band[QStringLiteral("name")].toString(), QStringLiteral("HIGH"));
    QCOMPARE(band[QStringLiteral("floor")].toDouble(), 500.0);
    QVERIFY(std::isnan(band[QStringLiteral("ceiling")].toDouble()));
    const qsizetype bandSignals = bandsSpy.count();
    climbMonitor.evaluate({ above, 400.2, 300 });
    QCOMPARE(bandsSpy.count(), bandSignals);
    climbMonitor.evaluate({ center.atDistanceAndAzimuth(5000, 270), 400, 300 });
    QVERIFY(climbMonitor.activeMonitor()->verticalBands().isEmpty());

    // Without the extra time, the entry in 28 s is beyond the manufacturer's warning time, which the vehicle monitor
    // publishes with the profile
    settings->approachExtraTime()->setRawValue(0);
    GeoZoneMonitor shortMonitor(&manager);
    QSignalSpy shortSpy(&shortMonitor, &GeoZoneMonitor::alert);
    shortMonitor.evaluate(start, GeoZoneMonitor::noVehicle, north);
    QCOMPARE(shortSpy.count(), 0);
    const QVariantMap thresholds = shortMonitor.activeMonitor()->alertThresholds();
    QVERIFY2(_near(thresholds[QStringLiteral("time")].toDouble(), 27.1, 0.1), qPrintable(thresholds[QStringLiteral("time")].toString()));
    QVERIFY(_near(thresholds[QStringLiteral("distance")].toDouble(), 521, 1));
    QCOMPARE(thresholds[QStringLiteral("groundSpeed")].toDouble(), 25.0);
    QCOMPARE(thresholds[QStringLiteral("profileName")].toString(), QStringLiteral("Generic vehicle"));
}

void GeoZoneMonitorTest::_testMinimumWarningTime()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"),
        QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300) }), errorString), qPrintable(errorString));

    // At 50 m/s straight towards the zone, evaluated every second, the vehicle is alerted at least 5 s and 250 m before
    // it enters the zone: by a fixed approach distance alone, without predicting entries, and with the thresholds of the
    // vehicle
    const GeoZoneMonitor::Velocity north{ 50, 0, 0, true };
    for (const bool predicted : { false, true }) {
        for (const double offset : { 5.0, 20.0, 35.0, 49.0 }) {
            GeoZoneMonitor monitor(&manager);
            if (!predicted) {
                GeoZoneMonitor::Settings distanceOnly;
                distanceOnly.limits = GeoZoneAlertTracker::Limits{ 100, 30, 0 };
                monitor.setSettings(distanceOnly);
            }
            QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
            int approachTick = -1;
            int entryTick = -1;
            for (int tick = 0; (tick < 60) && (entryTick < 0); tick++) {
                const double toZone = 1000 + offset - (50.0 * tick);
                monitor.evaluate({ center.atDistanceAndAzimuth(300 + toZone, 180) }, GeoZoneMonitor::noVehicle, north);
                if ((approachTick < 0) && !eventSpy.isEmpty()) {
                    const GeoZoneMonitor::AlertEvent approach = eventSpy.first().first().value<GeoZoneMonitor::AlertEvent>();
                    QVERIFY(approach.kind == GeoZoneMonitor::AlertEvent::Approach);
                    QVERIFY2(approach.distance >= 250, qPrintable(QStringLiteral("%1 m").arg(approach.distance)));
                    approachTick = tick;
                }
                if (monitor.activeMonitor()->inside()) {
                    entryTick = tick;
                }
            }
            QVERIFY(approachTick >= 0);
            QVERIFY(entryTick >= 0);
            QVERIFY2((entryTick - approachTick) >= 5, qPrintable(QStringLiteral("%1 s, predicted %2").arg(entryTick - approachTick).arg(predicted)));
        }
    }
}

void GeoZoneMonitorTest::_testTerrainAhead()
{
    // The pilot's extra time warns 48 s ahead at 40 m/s
    ScopedEnvironment environment;
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    const auto restore = qScopeGuard([settings]() { settings->approachExtraTime()->setRawValue(settings->approachExtraTime()->rawDefaultValue()); });
    settings->approachExtraTime()->setRawValue(15);

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
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ zone }), errorString), qPrintable(errorString));

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

    // The vehicle type picks the manufacturer's profile of the alert thresholds
    QCOMPARE(GeoZoneVehicleThresholds::vehicleClass(MAV_TYPE_QUADROTOR), QStringLiteral("multirotor"));
    QCOMPARE(GeoZoneVehicleThresholds::vehicleClass(MAV_TYPE_HEXAROTOR), QStringLiteral("multirotor"));
    QCOMPARE(GeoZoneVehicleThresholds::vehicleClass(MAV_TYPE_FIXED_WING), QStringLiteral("fixedwing"));
    QCOMPARE(GeoZoneVehicleThresholds::vehicleClass(MAV_TYPE_VTOL_TILTROTOR), QStringLiteral("vtol"));
    QCOMPARE(GeoZoneVehicleThresholds::vehicleClass(MAV_TYPE_GROUND_ROVER), QStringLiteral("default"));

    // The GNSS accuracy the vehicle reports widens them, else its HDOP, until it is older than the maximum age
    GeoZoneVehicleThresholds thresholds;
    thresholds.setVehicleClass(QStringLiteral("multirotor"));
    const GeoZoneAlertTracker::Velocity hover{ 0, 0, 0, true };
    QCOMPARE(thresholds.update(0, hover, {}).positionError, 20.0);
    thresholds.handleMessage(gpsRawIntMessage(vehicleId, 120, 1500, 3000), 1000);
    QCOMPARE(thresholds.update(1000, hover, {}).positionError, 3.0);
    QCOMPARE(thresholds.result().verticalPositionError, 6.0);
    QCOMPARE(thresholds.result().profileName, QStringLiteral("Multirotor"));
    thresholds.handleMessage(gpsRawIntMessage(vehicleId, 120, 0, 0), 2000);
    QCOMPARE(thresholds.update(2000, hover, {}).positionError, 12.0);
    QCOMPARE(thresholds.result().verticalPositionError, 18.0);
    QCOMPARE(thresholds.update(2000 + GeoZoneVehicleThresholds::accuracyMaxAgeMSecs + 1, hover, {}).positionError, 20.0);
    QCOMPARE(thresholds.toVariantMap()[QStringLiteral("distance")].toDouble(), 100.0);
}

void GeoZoneMonitorTest::_testMonitorMultipleVehicles()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300) }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.setActiveVehicleId(1);

    // A single vehicle isn't named
    monitor.evaluate({ center.atDistanceAndAzimuth(380, 0) }, 1);
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Approaching geozone P")));
    QCOMPARE(monitor.activeMonitor()->nearby().count(), 1);

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
    QVERIFY(monitor.activeMonitor()->inside());
    QSignalSpy activeSpy(&monitor, &GeoZoneMonitor::activeMonitorChanged);
    QCOMPARE(monitor.activeMonitor()->nearbyModel()->count(), 1);
    monitor.setActiveVehicleId(2);
    QCOMPARE(activeSpy.count(), 1);
    QVERIFY(monitor.activeMonitor()->nearby().isEmpty());
    QCOMPARE(monitor.activeMonitor()->nearbyModel()->count(), 0);
    QVERIFY(!monitor.activeMonitor()->inside());
    monitor.setActiveVehicleId(1);
    QCOMPARE(monitor.activeMonitor()->nearbyModel()->count(), 1);
    QVERIFY(monitor.activeMonitor()->inside());
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
    ScopedEnvironment environment;
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    const auto restoreDefault = qScopeGuard([settings]() {
        settings->recordRetention()->setRawValue(settings->recordRetention()->rawDefaultValue());
    });

    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QJsonObject feature = _circleFeature(QStringLiteral("P1"), QStringLiteral("PROHIBITED"), center, 300);
    feature[QStringLiteral("name")] = QStringLiteral("Prison \"West\", Zurich");
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ feature }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    GeoZoneAlertHistory &log = *monitor.alertHistory();
    log.startLog(dir.filePath(QStringLiteral("logs")));

    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
    monitor.evaluate({ center.atDistanceAndAzimuth(380, 0), 520, 400 }, 3);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) }, 3);
    QCOMPARE(eventSpy.count(), 2);
    const QDate logDate = environment.now().date();
    QCOMPARE(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().time, environment.now());

    // The alerts are buffered and written to the open file a little later
    QFile file(log.logFilePath(logDate));
    QVERIFY(!file.exists());
    QTRY_VERIFY_WITH_TIMEOUT(file.exists() && (file.size() > 0), GeoZoneAlertHistory::logFlushIntervalMSecs * 3);
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(file.fileName()));
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    file.close();
    // Each alert is followed by the popup showing it
    QCOMPARE(lines.count(), 5);
    QCOMPARE(lines[0], GeoZoneAlertHistory::logHeader());
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
    // With the alert thresholds of the vehicle: those of the default profile without velocity
    const QStringList approachFields = GeoZoneAlertHistory::logFields(lines[1]);
    QCOMPARE(approachFields.mid(16), QStringList({ QStringLiteral("100"), QStringLiteral("45"), QStringLiteral("96"), QStringLiteral("0.0"), QStringLiteral("0.0"), QStringLiteral("caution") }));
    const std::optional<GeoZoneMonitor::AlertEvent> parsed = GeoZoneAlertHistory::parseLogLine(lines[1]);
    QVERIFY(parsed);
    QCOMPARE(parsed->warningDistance, 100.0);
    QCOMPARE(parsed->warningTime, 96.0);
    QCOMPARE(parsed->level, GeoZoneAlertLevel::Caution);
    QCOMPARE(lines[2].section(QLatin1Char(','), 6, 6), QStringLiteral("notified"));
    QCOMPARE(GeoZoneAlertHistory::logFields(lines[2]).mid(16).join(QString()), QStringLiteral("caution"));
    QCOMPARE(GeoZoneAlertHistory::logFields(lines[3]).value(21), QStringLiteral("warning"));
    fields = lines[3].split(QLatin1Char(','));
    QCOMPARE(fields[4], QString());
    QCOMPARE(fields[6], QStringLiteral("entered"));
    QCOMPARE(fields[7], QStringLiteral("0"));

    // The files of the days past the retention are deleted when a file is started, others are kept
    settings->recordRetention()->setRawValue(30);
    const QDir logDir(dir.filePath(QStringLiteral("logs")));
    const QDate today = logDate.addDays(1);
    const QStringList kept = { log.logFilePath(today.addDays(-29)), log.logFilePath(logDate), dir.filePath(QStringLiteral("logs/GeoZoneAlerts-notes.csv")) };
    const QStringList old = { log.logFilePath(today.addDays(-30)), log.logFilePath(today.addDays(-400)) };
    for (const QString &path : kept + old) {
        QFile touched(path);
        QVERIFY(touched.open(QIODevice::Append));
    }
    environment.addSecs(24 * 60 * 60);
    monitor.evaluate({ center.atDistanceAndAzimuth(5000, 0) }, 3);
    monitor.evaluate({ center.atDistanceAndAzimuth(250, 0) }, 3);
    log.flushLog();
    QVERIFY(QFile::exists(log.logFilePath(today)));
    for (const QString &path : kept) {
        QVERIFY2(QFile::exists(path), qPrintable(path));
    }
    for (const QString &path : old) {
        QVERIFY2(!QFile::exists(path), qPrintable(path));
    }
    QCOMPARE(log.removeOldLogs(today, 0), 0);
    QCOMPARE(log.removeOldLogs(today.addDays(30), 30), 3);
    QCOMPARE(logDir.entryList({ QStringLiteral("*.csv") }, QDir::Files).count(), 1);

    // Feed text starting like a spreadsheet formula stays text
    GeoZoneMonitor::AlertEvent event;
    event.time = environment.now();
    event.zoneName = QStringLiteral("=1+1");
    event.zoneIdentifier = QStringLiteral("@A1");
    QVERIFY(GeoZoneAlertHistory::logLine(event).contains(QStringLiteral(",'@A1,'=1+1,")));
}

void GeoZoneMonitorTest::_testPilotEventLog()
{
    ScopedEnvironment environment;

    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate uSpaceCenter = center.atDistanceAndAzimuth(5000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P1"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("U1"), QStringLiteral("USPACE"), uSpaceCenter, 300),
    }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    GeoZoneAlertHistory &log = *monitor.alertHistory();
    log.startLog(dir.filePath(QStringLiteral("logs")));

    // Approaching the prohibited zone asks for the permission, which the pilot confirms: the entry is logged only
    monitor.evaluate({ center.atDistanceAndAzimuth(380, 0), 520, 400 }, 3);
    QCOMPARE(monitor.alertModel()->count(), 1);
    const int approachId = monitor.alertModel()->alerts().first().alertId;
    environment.addSecs(4);
    const GeoZone::Position inside{ center.atDistanceAndAzimuth(250, 0), 530, 400 };
    monitor.evaluate({ center.atDistanceAndAzimuth(330, 0), 525, 400 }, 3);
    monitor.approveAlert(approachId);
    monitor.evaluate(inside, 3);

    // U-space airspace is dismissed, shown again and muted, then closed
    monitor.evaluate({ uSpaceCenter, 450, 400 }, 3);
    const int uSpaceId = monitor.alertModel()->alerts().first().alertId;
    monitor.dismissAlert(uSpaceId);
    monitor.evaluate({ uSpaceCenter.atDistanceAndAzimuth(2000, 0), 450, 400 }, 3);
    monitor.evaluate({ uSpaceCenter, 450, 400 }, 3);
    const int uSpaceAgainId = monitor.alertModel()->alerts().first().alertId;
    monitor.muteAlerts(120);
    monitor.closeAlerts();
    log.flushLog();

    QFile file(log.logFilePath(environment.now().date()));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(file.fileName()));
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    file.close();
    QCOMPARE(lines.first(), GeoZoneAlertHistory::logHeader());
    QStringList sequence;
    for (const QString &line : lines.mid(1)) {
        const QStringList fields = line.split(QLatin1Char(','));
        QCOMPARE(fields.count(), 22);
        sequence.append(QStringList{ fields[6], fields[8], fields[14], fields[15] }.join(QLatin1Char(' ')).trimmed());
    }
    const QString approach = QString::number(approachId);
    const QString question = QString::fromLatin1(GeoZoneMonitor::permissionQuestion);
    const QString entry = QString::number(approachId + 1);
    const QString uSpace = QString::number(uSpaceId);
    const QString uSpaceAgain = QString::number(uSpaceAgainId);
    QCOMPARE(sequence, (QStringList{
        QStringLiteral("approaching P1 %1").arg(approach),
        QStringLiteral("notified P1 %1 %2").arg(approach, question),
        QStringLiteral("approved P1 %1 I do").arg(approach),
        QStringLiteral("entered P1 %1").arg(entry),
        QStringLiteral("entered U1 %1").arg(uSpace),
        QStringLiteral("notified U1 %1").arg(uSpace),
        QStringLiteral("dismissed U1 %1 Dismiss").arg(uSpace),
        QStringLiteral("entered U1 %1").arg(uSpaceAgain),
        QStringLiteral("notified U1 %1").arg(uSpaceAgain),
        QStringLiteral("muted U1 %1 Silence speech for 120 s").arg(uSpaceAgain),
        QStringLiteral("dismissed U1 %1 Close").arg(uSpaceAgain),
    }));

    // The answer is logged with the time, vehicle and position when the pilot gave it
    const QStringList approved = lines[3].split(QLatin1Char(','));
    QCOMPARE(QDateTime::fromString(approved[0], Qt::ISODateWithMs), environment.now());
    QCOMPARE(approved[1], QStringLiteral("3"));
    QCOMPARE(approved[4], QStringLiteral("525.0"));
    QCOMPARE(approved[7], QStringLiteral("30"));
    QCOMPARE(approved[10], QStringLiteral("Prohibited"));

    // Only the alerts are listed, also when read back
    QCOMPARE(log.count(), 4);
    GeoZoneAlertHistory later(&manager);
    later.startLog(dir.filePath(QStringLiteral("logs")));
    QCOMPARE(later.count(), 4);
    const std::optional<GeoZoneMonitor::AlertEvent> read = GeoZoneAlertHistory::parseLogLine(lines[3]);
    QVERIFY(read && read->pilotEvent());
    QCOMPARE(read->detail, QStringLiteral("I do"));
}

void GeoZoneMonitorTest::_testAlertHistory()
{
    const ScopedEnvironment environment;

    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QJsonObject feature = _circleFeature(QStringLiteral("P1"), QStringLiteral("PROHIBITED"), center, 300);
    feature[QStringLiteral("name")] = QStringLiteral("Prison \"West\", Zurich");
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ feature }), errorString), qPrintable(errorString));

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
    GeoZoneAlertHistory earlier(&manager);
    earlier.startLog(dir.filePath(QStringLiteral("logs")));
    for (const QList<QVariant> &arguments : std::as_const(eventSpy)) {
        earlier.append(arguments.first().value<GeoZoneMonitor::AlertEvent>());
    }
    GeoZoneMonitor::AlertEvent formula;
    formula.time = environment.now();
    formula.zoneIdentifier = QStringLiteral("F1");
    formula.zoneName = QStringLiteral("=1+1");
    formula.restriction = GeoZone::Conditional;
    formula.distance = 50;
    formula.timeToEntry = 12;
    earlier.append(formula);
    earlier.flushLog();
    GeoZoneAlertHistory later(&manager);
    later.append(formula);
    later.startLog(dir.filePath(QStringLiteral("logs")));
    QCOMPARE(later.count(), 4);
    QCOMPARE(later.unacknowledgedCount(), 1);
    QVERIFY(!later.data(later.index(0), GeoZoneAlertHistory::EarlierRole).toBool());
    QVERIFY(later.data(later.index(1), GeoZoneAlertHistory::EarlierRole).toBool());
    QVERIFY(later.data(later.index(1), GeoZoneAlertHistory::AcknowledgedRole).toBool());
    QCOMPARE(later.data(later.index(1), GeoZoneAlertHistory::ZoneNameRole).toString(), QStringLiteral("=1+1"));
    QVERIFY(later.data(later.index(1), GeoZoneAlertHistory::MessageRole).toString().endsWith(QStringLiteral("entry in about 12 s")));
    QCOMPARE(later.data(later.index(2), GeoZoneAlertHistory::MessageRole).toString(), history->data(history->index(0), GeoZoneAlertHistory::MessageRole).toString());
    QCOMPARE(later.data(later.index(3), GeoZoneAlertHistory::AltitudeRole).toDouble(), 520.0);
    QVERIFY(!GeoZoneAlertHistory::parseLogLine(GeoZoneAlertHistory::logHeader()));

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
    QSignalSpy statusSpy(monitor.activeMonitor(), &GeoZoneVehicleMonitor::fenceStatusChanged);

    // Not breached, as received from the vehicle
    QVERIFY(!monitor.activeMonitor()->fenceStatusKnown());
    emit vehicle.mavlinkMessageReceived(fenceStatus(0, 0, FENCE_BREACH_NONE));
    QVERIFY(monitor.activeMonitor()->fenceStatusKnown());
    QVERIFY(!monitor.activeMonitor()->fenceBreached());
    QCOMPARE(statusSpy.count(), 1);

    // A breach is announced once while it lasts
    emit vehicle.mavlinkMessageReceived(fenceStatus(1, 1, FENCE_BREACH_BOUNDARY));
    emit vehicle.mavlinkMessageReceived(fenceStatus(1, 1, FENCE_BREACH_BOUNDARY));
    QVERIFY(monitor.activeMonitor()->fenceBreached());
    QCOMPARE(monitor.activeMonitor()->fenceBreachText(), QStringLiteral("Boundary fence breached"));
    QCOMPARE(breachSpy.count(), 1);
    QCOMPARE(breachSpy.first().first().toString(), QStringLiteral("Boundary fence breached"));
    QCOMPARE(statusSpy.count(), 2);

    // Cleared, then breached again at the maximum altitude
    emit vehicle.mavlinkMessageReceived(fenceStatus(0, 1, FENCE_BREACH_NONE));
    QVERIFY(!monitor.activeMonitor()->fenceBreached());
    QVERIFY(monitor.activeMonitor()->fenceBreachText().isEmpty());
    emit vehicle.mavlinkMessageReceived(fenceStatus(1, 2, FENCE_BREACH_MAXALT));
    QCOMPARE(breachSpy.count(), 2);
    QCOMPARE(monitor.activeMonitor()->fenceBreachText(), QStringLiteral("Maximum altitude fence breached (2 time(s))"));

    // Other vehicles don't change the status shown, their breaches are announced naming them
    Vehicle other(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    (void) monitor.addVehicle(&other);
    emit other.mavlinkMessageReceived(fenceStatus(0, 0, FENCE_BREACH_NONE));
    QVERIFY(monitor.activeMonitor()->fenceBreached());
    emit other.mavlinkMessageReceived(fenceStatus(1, 1, FENCE_BREACH_MINALT));
    QCOMPARE(breachSpy.count(), 3);
    QCOMPARE(breachSpy.last().first().toString(), QStringLiteral("Vehicle %1: Minimum altitude fence breached").arg(other.id()));
    monitor.setActiveVehicle(&other);
    QCOMPARE(monitor.activeMonitor()->fenceBreachText(), QStringLiteral("Minimum altitude fence breached"));
    monitor.removeVehicle(&other);
    QVERIFY(!monitor.activeMonitor());
}

void GeoZoneMonitorTest::_testFenceWarning()
{
    const ScopedMetricUnits metricUnits;
    const QGeoCoordinate origin(47, 8);
    const auto local = [&origin](double east, double north) {
        return origin.atDistanceAndAzimuth(north, 0).atDistanceAndAzimuth(east, 90);
    };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    GeoZoneMonitor monitor(&manager);
    qint64 now = 0;
    monitor.setClock([&now]() { return now; });
    GeoZoneMonitor::Settings settings;
    monitor.setSettings(settings);
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);

    for (const MAV_AUTOPILOT autopilot : { MAV_AUTOPILOT_PX4, MAV_AUTOPILOT_ARDUPILOTMEGA }) {
        const bool px4 = autopilot == MAV_AUTOPILOT_PX4;
        Vehicle vehicle(autopilot, MAV_TYPE_QUADROTOR);
        GeoZoneVehicleMonitor *const vehicleMonitor = monitor.addVehicle(&vehicle);
        monitor.setActiveVehicle(&vehicle);
        GeoZoneFenceWarning *const warning = vehicleMonitor->fenceWarning();
        QVERIFY(warning);
        // An offline vehicle has no fence
        QVERIFY(!warning->enabled());

        // An exclusion polygon from 450 m east, and the vehicle's fence action: PX4 returns, ArduPilot copters RTL or land
        GeoZoneFenceWarning::Config config;
        config.enabled = true;
        config.firmware = px4 ? GeoZoneFenceWarning::Firmware::PX4 : GeoZoneFenceWarning::Firmware::ArduPilot;
        config.action = px4 ? 3 : 1;
        config.fence.polygons.append({ { local(450, 100), local(550, 100), local(550, -100), local(450, -100) }, false });
        config.fence.inclusionUnion = px4;
        warning->setConfig(config);
        QVERIFY(warning->enabled());
        const QString action = px4 ? QStringLiteral("Return") : QStringLiteral("RTL or land");
        QCOMPARE(warning->actionText(), action);

        QSignalSpy warningSpy(&monitor, &GeoZoneMonitor::fenceWarning);
        QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
        sink->spoken.clear();
        // East at 10 m/s, as GLOBAL_POSITION_INT reports it (vy is east)
        const auto fly = [&](double east, int16_t vy, double relativeAltitude = 30) {
            now += 1000;
            const QGeoCoordinate position = local(east, 0);
            mavlink_message_t message{};
            (void) mavlink_msg_global_position_int_pack(vehicle.id(), MAV_COMP_ID_AUTOPILOT1, &message, 0, std::lround(position.latitude() * 1e7),
                                                        std::lround(position.longitude() * 1e7), 500000, std::lround(relativeAltitude * 1000), 0, vy, 0, UINT16_MAX);
            emit vehicle.mavlinkMessageReceived(message);
        };
        // 35 s away, then within the warning time of 8 s
        fly(100, 1000);
        QVERIFY(!warning->active());
        QCOMPARE(warningSpy.count(), 0);
        fly(390, 1000);
        QVERIFY(warning->active());
        QCOMPARE(warning->timeToFence(), 6);
        QVERIFY(std::abs(warning->distanceToFence() - 60) < 1);
        QVERIFY(warning->intersection().distanceTo(local(450, 0)) < 1);
        QCOMPARE(warning->path().count(), 2);
        const QString text = QStringLiteral("Geofence: %1 in 6 s, 60 m ahead").arg(action);
        QCOMPARE(warning->text(), text);
        // Shown above the geozone alerts, spoken once and logged with the fence, time and distance
        QCOMPARE(warningSpy.count(), 1);
        QCOMPARE(warningSpy.first().first().toString(), text);
        QCOMPARE(monitor.alertModel()->alerts().count(), 1);
        const GeoZoneAlertModel::Alert shown = monitor.alertModel()->alerts().first();
        QCOMPARE(shown.message, text);
        QCOMPARE(shown.severity, -1);
        QCOMPARE(shown.level, GeoZoneAlertLevel::Warning);
        QCOMPARE(sink->spoken, QStringList({ QStringLiteral("Warning: ") + text }));
        QCOMPARE(eventSpy.count(), 1);
        const GeoZoneMonitor::AlertEvent event = eventSpy.first().first().value<GeoZoneMonitor::AlertEvent>();
        QCOMPARE(event.kind, GeoZoneMonitor::AlertEvent::FenceWarning);
        QCOMPARE(event.zoneIdentifier, QStringLiteral("FENCE"));
        QCOMPARE(event.zoneName, QStringLiteral("Exclusion polygon 1"));
        QCOMPARE(event.detail, QStringLiteral("Fence action: %1").arg(action));
        QCOMPARE(std::lround(event.timeToEntry), 6);
        QVERIFY(GeoZoneAlertHistory::logLine(event).contains(QStringLiteral(",fence_warning,60,FENCE,Exclusion polygon 1,")));

        // Counting down in place, without speaking again, and at most every 250 ms
        fly(400, 1000);
        QCOMPARE(warning->timeToFence(), 5);
        QCOMPARE(monitor.alertModel()->alerts().count(), 1);
        QCOMPARE(monitor.alertModel()->alerts().first().alertId, shown.alertId);
        QCOMPARE(monitor.alertModel()->alerts().first().message, QStringLiteral("Geofence: %1 in 5 s, 50 m ahead").arg(action));
        now -= 900;
        fly(420, 1000);
        QCOMPARE(warning->timeToFence(), 5);
        QCOMPARE(warningSpy.count(), 1);
        QCOMPARE(sink->spoken.count(), 1);

        // Turning away ends it
        fly(400, -1000);
        QVERIFY(!warning->active());
        QCOMPARE(monitor.alertModel()->alerts().count(), 0);

        // Back towards the fence, then its breach: the warning ends with it. The breach is raised once, from the
        // position past the fence, and not again when the vehicle reports it too.
        fly(420, 1000);
        QVERIFY(warning->active());
        QCOMPARE(warningSpy.count(), 2);
        const qsizetype spokenCount = sink->spoken.count();
        QSignalSpy breachSpy(&monitor, &GeoZoneMonitor::fenceBreach);
        fly(460, 1000);
        QVERIFY(!warning->active());
        QCOMPARE(breachSpy.count(), 1);
        mavlink_message_t fenceStatus{};
        (void) mavlink_msg_fence_status_pack(vehicle.id(), MAV_COMP_ID_AUTOPILOT1, &fenceStatus, 1, 1, FENCE_BREACH_BOUNDARY, 0, FENCE_MITIGATE_NONE);
        emit vehicle.mavlinkMessageReceived(fenceStatus);
        QCOMPARE(breachSpy.count(), 1);
        QCOMPARE(monitor.alertModel()->alerts().count(), 1);
        QCOMPARE(monitor.alertModel()->alerts().first().message, QStringLiteral("Boundary fence breached"));
        QCOMPARE(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().kind, GeoZoneMonitor::AlertEvent::FenceBreach);
        // The vehicle speaks its breach itself
        QCOMPARE(sink->spoken.count(), spokenCount);

        // Hovering, or with the fence disabled, nothing is predicted
        fly(300, 0);
        QVERIFY(!warning->active());
        config.enabled = false;
        warning->setConfig(config);
        fly(300, 1000);
        fly(400, 1000);
        QVERIFY(!warning->active());

        monitor.closeAlerts();
        monitor.removeVehicle(&vehicle);
    }

    // The manoeuvres of the fence actions, and how the pilot overrides them
    using Warning = GeoZoneFenceWarning;
    QCOMPARE(Warning::actionText(Warning::Firmware::PX4, Warning::VehicleClass::Copter, 2), QStringLiteral("Hold"));
    QCOMPARE(Warning::actionText(Warning::Firmware::PX4, Warning::VehicleClass::Copter, 5), QStringLiteral("Land"));
    QCOMPARE(Warning::actionText(Warning::Firmware::ArduPilot, Warning::VehicleClass::Copter, 4), QStringLiteral("Brake or land"));
    QCOMPARE(Warning::actionText(Warning::Firmware::ArduPilot, Warning::VehicleClass::Copter, 0), QStringLiteral("Report only, no manoeuvre"));
    QCOMPARE(Warning::actionText(Warning::Firmware::ArduPilot, Warning::VehicleClass::Plane, 1), QStringLiteral("RTL"));
    QCOMPARE(Warning::actionText(Warning::Firmware::ArduPilot, Warning::VehicleClass::Rover, 2), QStringLiteral("Hold"));
    QVERIFY(Warning::isManoeuvre(Warning::Firmware::ArduPilot, 1) && !Warning::isManoeuvre(Warning::Firmware::ArduPilot, 0));
    QVERIFY(Warning::isManoeuvre(Warning::Firmware::PX4, 2) && !Warning::isManoeuvre(Warning::Firmware::PX4, 1));
    QCOMPARE(Warning::takeControlMode(Warning::Firmware::ArduPilot, Warning::VehicleClass::Copter, { QStringLiteral("Stabilize"), QStringLiteral("Loiter") }), QStringLiteral("Loiter"));
    QCOMPARE(Warning::takeControlMode(Warning::Firmware::ArduPilot, Warning::VehicleClass::Copter, { QStringLiteral("RTL"), QStringLiteral("POSHOLD"), QStringLiteral("LOITER") }), QStringLiteral("LOITER"));
    QCOMPARE(Warning::takeControlMode(Warning::Firmware::ArduPilot, Warning::VehicleClass::Plane, { QStringLiteral("Manual"), QStringLiteral("FBW A") }), QStringLiteral("FBW A"));
    QCOMPARE(Warning::takeControlMode(Warning::Firmware::PX4, Warning::VehicleClass::Copter, { QStringLiteral("Hold"), QStringLiteral("Position") }), QStringLiteral("Position"));
    QVERIFY(Warning::takeControlMode(Warning::Firmware::PX4, Warning::VehicleClass::Copter, { QStringLiteral("Hold") }).isEmpty());
    Warning::Config arduPilot;
    arduPilot.firmware = Warning::Firmware::ArduPilot;
    arduPilot.action = 1;
    QVERIFY(Warning::overrideText(arduPilot).contains(QStringLiteral("10 s")));
    arduPilot.modeChangeBlocked = true;
    QVERIFY(Warning::overrideText(arduPilot).contains(QStringLiteral("disable the fence")));
    Warning::Config px4;
    px4.firmware = Warning::Firmware::PX4;
    px4.action = 4;
    QVERIFY(Warning::overrideText(px4).contains(QStringLiteral("can't be overridden")));
}

void GeoZoneMonitorTest::_testFenceBreachWithoutStatus()
{
    // A PX4 vehicle, which sends no FENCE_STATUS, flying east at 10 m/s into an exclusion polygon from 450 m to 550 m
    // east with GF_ACTION Hold, and below a ceiling 100 m above home
    const ScopedMetricUnits metricUnits;
    const QGeoCoordinate origin(47, 8);
    const auto local = [&origin](double east, double north) {
        return origin.atDistanceAndAzimuth(north, 0).atDistanceAndAzimuth(east, 90);
    };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    GeoZoneMonitor monitor(&manager);
    qint64 now = 0;
    monitor.setClock([&now]() { return now; });
    monitor.setSettings(GeoZoneMonitor::Settings());
    Vehicle vehicle(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    GeoZoneVehicleMonitor *const vehicleMonitor = monitor.addVehicle(&vehicle);
    monitor.setActiveVehicle(&vehicle);
    GeoZoneFenceWarning *const warning = vehicleMonitor->fenceWarning();
    QVERIFY(warning);
    GeoZoneFenceWarning::Config config;
    config.enabled = true;
    config.firmware = GeoZoneFenceWarning::Firmware::PX4;
    config.action = 2;
    config.fence.polygons.append({ { local(450, 100), local(550, 100), local(550, -100), local(450, -100) }, false });
    config.fence.inclusionUnion = true;
    config.fence.maxAltitude = 100;
    warning->setConfig(config);

    QSignalSpy breachSpy(&monitor, &GeoZoneMonitor::fenceBreach);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
    QSignalSpy statusSpy(vehicleMonitor, &GeoZoneVehicleMonitor::fenceStatusChanged);
    const auto fly = [&](double east, int16_t vy, double relativeAltitude = 30, int msecs = 1000) {
        now += msecs;
        const QGeoCoordinate position = local(east, 0);
        mavlink_message_t message{};
        (void) mavlink_msg_global_position_int_pack(vehicle.id(), MAV_COMP_ID_AUTOPILOT1, &message, 0, std::lround(position.latitude() * 1e7),
                                                    std::lround(position.longitude() * 1e7), 500000, std::lround(relativeAltitude * 1000), 0, vy, 0, UINT16_MAX);
        emit vehicle.mavlinkMessageReceived(message);
    };
    const auto breachEvents = [&eventSpy]() {
        int count = 0;
        for (const QList<QVariant> &arguments : std::as_const(eventSpy)) {
            count += (arguments.first().value<GeoZoneMonitor::AlertEvent>().kind == GeoZoneMonitor::AlertEvent::FenceBreach) ? 1 : 0;
        }
        return count;
    };

    // Warned before the fence, without a breach
    fly(390, 1000);
    QVERIFY(warning->active());
    QVERIFY(!warning->outside());
    QVERIFY(!vehicleMonitor->fenceBreached());
    QCOMPARE(breachSpy.count(), 0);

    // Past the fence the prediction ends, and the breach is raised and logged once from the position
    fly(455, 1000);
    QVERIFY(!warning->active());
    QVERIFY(warning->outside());
    QVERIFY(!vehicleMonitor->fenceStatusKnown());
    QVERIFY(vehicleMonitor->fenceBreached());
    QCOMPARE(vehicleMonitor->fenceBreachText(), QStringLiteral("Boundary fence breached"));
    QCOMPARE(breachSpy.count(), 1);
    QCOMPARE(breachSpy.first().first().toString(), QStringLiteral("Boundary fence breached"));
    QCOMPARE(breachEvents(), 1);
    QCOMPARE(statusSpy.count(), 1);
    QVERIFY(!monitor.alertModel()->alerts().isEmpty());
    QCOMPARE(monitor.alertModel()->alerts().first().message, QStringLiteral("Boundary fence breached"));

    // It lasts while the vehicle holds outside its fence, which keeps the override shown, without another alert
    for (int i = 0; i < 5; i++) {
        fly(460, 0);
    }
    QVERIFY(vehicleMonitor->fenceBreached());
    QCOMPARE(breachSpy.count(), 1);
    QCOMPARE(breachEvents(), 1);

    // Back inside the fence it ends, after a moment, so that a position at the boundary doesn't breach repeatedly
    fly(440, -1000);
    QVERIFY(vehicleMonitor->fenceBreached());
    fly(452, 1000, 30, 500);
    fly(440, -1000, 30, 500);
    QVERIFY(vehicleMonitor->fenceBreached());
    QCOMPARE(breachSpy.count(), 1);
    fly(430, -1000);
    fly(420, -1000);
    QVERIFY(!warning->outside());
    QVERIFY(!vehicleMonitor->fenceBreached());
    QVERIFY(vehicleMonitor->fenceBreachText().isEmpty());

    // Above the ceiling is another breach, of the altitude
    fly(300, 0, 101);
    QVERIFY(vehicleMonitor->fenceBreached());
    QCOMPARE(vehicleMonitor->fenceBreachText(), QStringLiteral("Maximum altitude fence breached"));
    QCOMPARE(breachSpy.count(), 2);
    QCOMPARE(breachEvents(), 2);

    // With the fence disabled the vehicle is outside of nothing
    config.enabled = false;
    warning->setConfig(config);
    QVERIFY(!vehicleMonitor->fenceBreached());
    fly(300, 0, 101);
    QVERIFY(!vehicleMonitor->fenceBreached());
    config.enabled = true;
    warning->setConfig(config);
    fly(300, 0, 101);
    QCOMPARE(breachSpy.count(), 3);

    // A vehicle reporting its fence status is taken at its word, until it stops reporting it
    mavlink_message_t fenceStatus{};
    (void) mavlink_msg_fence_status_pack(vehicle.id(), MAV_COMP_ID_AUTOPILOT1, &fenceStatus, 0, 0, FENCE_BREACH_NONE, 0, FENCE_MITIGATE_NONE);
    emit vehicle.mavlinkMessageReceived(fenceStatus);
    QVERIFY(vehicleMonitor->fenceStatusKnown());
    QVERIFY(!vehicleMonitor->fenceBreached());
    fly(300, 0, 101);
    QVERIFY(!vehicleMonitor->fenceBreached());
    fly(300, 0, 101, GeoZoneVehicleMonitor::fenceStatusTimeoutMSecs);
    QVERIFY(!vehicleMonitor->fenceStatusKnown());
    QVERIFY(vehicleMonitor->fenceBreached());
    QCOMPARE(breachSpy.count(), 4);

    monitor.closeAlerts();
    monitor.removeVehicle(&vehicle);
}

void GeoZoneMonitorTest::_testFenceTakeControl()
{
    using Warning = GeoZoneFenceWarning;
    const QGeoCoordinate center(47, 8);

    // An ArduPilot VTOL runs the plane firmware: hovering it has the fence actions of planes, and the pilot takes
    // control in its hover modes only, since its Loiter circles and FBW A and Manual fly as a fixed wing
    const QStringList planeModes = { QStringLiteral("Manual"), QStringLiteral("FBW A"), QStringLiteral("Loiter"), QStringLiteral("QuadPlane Hover"),
                                     QStringLiteral("QuadPlane Loiter") };
    QCOMPARE(Warning::takeControlMode(Warning::Firmware::ArduPilot, Warning::VehicleClass::Copter, planeModes, true), QStringLiteral("QuadPlane Loiter"));
    QCOMPARE(Warning::takeControlMode(Warning::Firmware::ArduPilot, Warning::VehicleClass::Copter, { QStringLiteral("QHOVER"), QStringLiteral("MANUAL") }, true), QStringLiteral("QHOVER"));
    QVERIFY(Warning::takeControlMode(Warning::Firmware::ArduPilot, Warning::VehicleClass::Copter, { QStringLiteral("Manual"), QStringLiteral("Loiter") }, true).isEmpty());
    QCOMPARE(Warning::takeControlMode(Warning::Firmware::ArduPilot, Warning::VehicleClass::Plane, planeModes, false), QStringLiteral("FBW A"));
    QCOMPARE(Warning::takeControlMode(Warning::Firmware::PX4, Warning::VehicleClass::Copter, { QStringLiteral("Hold"), QStringLiteral("Position") }, true), QStringLiteral("Position"));
    Vehicle vtol(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_VTOL_TILTROTOR);
    Warning warning(&vtol);
    Warning::Config config;
    config.enabled = true;
    config.firmware = Warning::Firmware::ArduPilot;
    config.vehicleClass = Warning::VehicleClass::Copter;
    config.vtolHover = true;
    config.action = 6;
    config.fence.circles.append({ QGeoCircle(center, 100), false });
    config.takeControlMode = QStringLiteral("QuadPlane Loiter");
    warning.setConfig(config);
    QCOMPARE(warning.actionText(), QStringLiteral("Guided to the return point"));
    config.vtolHover = false;
    warning.setConfig(config);
    QCOMPARE(warning.actionText(), QStringLiteral("Fence action 6"));

    // Taking control is offered while the vehicle would switch: not during a breach with FENCE_OPTIONS bit 0 ...
    QSignalSpy canSpy(&warning, &Warning::canTakeControlChanged);
    QVERIFY(warning.canTakeControl());
    config.modeChangeBlocked = true;
    warning.setConfig(config);
    QVERIFY(warning.canTakeControl());
    canSpy.clear();
    warning.setBreachReported(true);
    QCOMPARE(canSpy.count(), 1);
    QVERIFY(warning.breached());
    QVERIFY(!warning.canTakeControl());
    warning.takeControl();
    QVERIFY2(warning.commandText().contains(QStringLiteral("refuses flight mode changes")), qPrintable(warning.commandText()));
    // ... unless the fence only reports
    config.action = 0;
    warning.setConfig(config);
    QVERIFY(warning.canTakeControl());
    warning.setBreachReported(false);
    QVERIFY(!warning.breached());

    // ... and not once PX4 terminates the flight, which the position outside the fence tells
    config = Warning::Config();
    config.enabled = true;
    config.firmware = Warning::Firmware::PX4;
    config.action = 4;
    config.fence.circles.append({ QGeoCircle(center, 100), false });
    config.takeControlMode = QStringLiteral("Position");
    warning.setConfig(config);
    QVERIFY(warning.canTakeControl());
    warning.evaluate(center, 30, Warning::Velocity());
    QVERIFY(warning.breached());
    QVERIFY(!warning.canTakeControl());
    config.action = 3;
    warning.setConfig(config);
    QVERIFY(warning.canTakeControl());
    config.takeControlMode.clear();
    warning.setConfig(config);
    QVERIFY(!warning.canTakeControl());

    // With GF_PREDICT PX4 acts before the fence, which the warning tells
    config.predictive = true;
    config.takeControlMode = QStringLiteral("Position");
    warning.setConfig(config);
    QVERIFY(warning.timingText().contains(QStringLiteral("GF_PREDICT")));
    warning.evaluate(center.atDistanceAndAzimuth(160, 270), 30, Warning::Velocity{ 0, 10, 0, true });
    QVERIFY(warning.active());
    QVERIFY2(warning.text().startsWith(QStringLiteral("Geofence: Return within 6 s, ")), qPrintable(warning.text()));
    config.predictive = false;
    warning.setConfig(config);
    QVERIFY(warning.timingText().isEmpty());
    QVERIFY2(warning.text().startsWith(QStringLiteral("Geofence: Return in 6 s, ")), qPrintable(warning.text()));
    // ... but not for an action which only warns
    config.predictive = true;
    config.action = 1;
    warning.setConfig(config);
    QVERIFY(warning.timingText().isEmpty());
    config.action = 3;

    // A warning time which isn't positive, like the one of a profile with both of its times at 0, isn't taken
    warning.setWarningTime(12);
    QCOMPARE(warning.warningTime(), 12.0);
    for (const double seconds : { 0.0, -3.0, qQNaN() }) {
        warning.setWarningTime(seconds);
        QCOMPARE(warning.warningTime(), Warning::defaultWarningTime);
        warning.setWarningTime(12);
    }

    // The pilot disabling the fence shows in the checklist until it is enabled again
    GeoZonePreflightCheck::FenceInput input;
    QCOMPARE(GeoZonePreflightCheck::fenceItem(input).state, GeoZonePreflightCheck::NotApplicable);
    input.fenceDisabled = true;
    const GeoZonePreflightCheck::Item item = GeoZonePreflightCheck::fenceItem(input);
    QCOMPARE(item.state, GeoZonePreflightCheck::Warning);
    QVERIFY2(item.text.contains(QStringLiteral("Disable Fence")), qPrintable(item.text));
}

void GeoZoneMonitorTest::_testVerticalBandSeverity()
{
    // The bands of the altitude strip tell how severe their restriction is, which isn't the order of the restrictions:
    // U-space airspace ranks above no restriction
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("U"), QStringLiteral("USPACE"), center, 300),
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
    }), errorString), qPrintable(errorString));
    GeoZoneMonitor monitor(&manager);
    monitor.evaluate({ center, 400, 300 });
    QVERIFY(monitor.activeMonitor());
    QMap<QString, int> severities;
    for (const QVariant &band : monitor.activeMonitor()->verticalBands()) {
        const QVariantMap map = band.toMap();
        QVERIFY(map.contains(QStringLiteral("severity")));
        severities.insert(map[QStringLiteral("name")].toString(), map[QStringLiteral("severity")].toInt());
        QCOMPARE(map[QStringLiteral("severity")].toInt(), GeoZone::restrictionSeverity(static_cast<GeoZone::Restriction>(map[QStringLiteral("restriction")].toInt())));
    }
    QCOMPARE(severities.value(QStringLiteral("P"), -1), 0);
    QCOMPARE(severities.value(QStringLiteral("U"), -1), 3);
    QVERIFY(GeoZone::restrictionSeverity(GeoZone::USpace) < GeoZone::restrictionSeverity(GeoZone::NoRestriction));
}

void GeoZoneMonitorTest::_testFenceAlertSequence()
{
    // An ArduCopter flying east at 10 m/s towards a prohibited zone whose exclusion fence it stores, with FENCE_ACTION
    // RTL or land and a 2 m margin
    const ScopedMetricUnits metricUnits;
    const QGeoCoordinate origin(47, 8);
    const auto local = [&origin](double east) { return origin.atDistanceAndAzimuth(east, 90); };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), local(700), 100) }), errorString), qPrintable(errorString));
    QCOMPARE(manager.zoneCount(), 1);
    GeoZoneMonitor monitor(&manager);
    qint64 now = 0;
    monitor.setClock([&now]() { return now; });
    GeoZoneMonitor::Settings settings;
    settings.limits = GeoZoneAlertTracker::Limits{ 100, 30, 30 };
    monitor.setSettings(settings);
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);

    Vehicle vehicle(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_QUADROTOR);
    GeoZoneVehicleMonitor *const vehicleMonitor = monitor.addVehicle(&vehicle);
    monitor.setActiveVehicle(&vehicle);
    GeoZoneFenceWarning::Config config;
    config.enabled = true;
    config.firmware = GeoZoneFenceWarning::Firmware::ArduPilot;
    config.action = 1;
    config.fence.horizontalMargin = 2;
    const GeoZone *const zone = manager.catalog().zones().constFirst();
    for (const QList<QGeoCoordinate> &polygon : GeoZoneFences::zoneFences(zone, 0, true).polygons) {
        config.fence.polygons.append({ polygon, false });
    }
    vehicleMonitor->fenceWarning()->setConfig(config);

    // Each second the zones are evaluated and the vehicle reports its position
    const auto fly = [&](double east) {
        now += 1000;
        const GeoZone::Position position{ local(east), 600, 500 };
        monitor.evaluate(&vehicle, position, GeoZoneMonitor::Velocity{ 0, 10, 0, true });
        mavlink_message_t message{};
        (void) mavlink_msg_global_position_int_pack(vehicle.id(), MAV_COMP_ID_AUTOPILOT1, &message, 0, std::lround(position.coordinate.latitude() * 1e7),
                                                    std::lround(position.coordinate.longitude() * 1e7), 600000, 100000, 0, 1000, 0, UINT16_MAX);
        emit vehicle.mavlinkMessageReceived(message);
    };
    const auto kinds = [&eventSpy]() {
        QList<GeoZoneMonitor::AlertEvent::Kind> list;
        for (const QList<QVariant> &arguments : std::as_const(eventSpy)) {
            list.append(arguments.first().value<GeoZoneMonitor::AlertEvent>().kind);
        }
        return list;
    };

    // The zone is approached first, about 30 s before its entry
    for (double east = 0; east <= 330; east += 10) {
        fly(east);
    }
    QCOMPARE(kinds(), (QList<GeoZoneMonitor::AlertEvent::Kind>{ GeoZoneMonitor::AlertEvent::Approach }));
    QCOMPARE(sink->spoken.count(), 1);
    QVERIFY(sink->spoken.first().startsWith(QStringLiteral("Caution: Approaching geozone P")));
    QVERIFY(!vehicleMonitor->fenceWarning()->active());

    // Then the fence warning, above the zone alert and spoken at once, more urgent than the last announcement
    for (double east = 340; east <= 530; east += 10) {
        fly(east);
    }
    QVERIFY(vehicleMonitor->fenceWarning()->active());
    QCOMPARE(kinds(), (QList<GeoZoneMonitor::AlertEvent::Kind>{ GeoZoneMonitor::AlertEvent::Approach, GeoZoneMonitor::AlertEvent::FenceWarning }));
    QCOMPARE(sink->spoken.count(), 2);
    QVERIFY2(sink->spoken.last().startsWith(QStringLiteral("Warning: Geofence: RTL or land in ")), qPrintable(sink->spoken.last()));
    QList<GeoZoneAlertModel::Alert> shown = monitor.alertModel()->alerts();
    QCOMPARE(shown.count(), 2);
    QVERIFY(shown[0].message.startsWith(QStringLiteral("Geofence: RTL or land in ")));
    QVERIFY(shown[1].message.startsWith(QStringLiteral("Approaching geozone P")));

    // The vehicle breaches the fence before the zone and reports it: the warning gives way to the breach, which the
    // vehicle announces itself, and the vehicle turns back without entering the zone
    fly(598);
    mavlink_message_t fenceStatus{};
    (void) mavlink_msg_fence_status_pack(vehicle.id(), MAV_COMP_ID_AUTOPILOT1, &fenceStatus, 1, 1, FENCE_BREACH_BOUNDARY, 0, FENCE_MITIGATE_NONE);
    emit vehicle.mavlinkMessageReceived(fenceStatus);
    QVERIFY(!vehicleMonitor->fenceWarning()->active());
    shown = monitor.alertModel()->alerts();
    QCOMPARE(shown.count(), 2);
    QCOMPARE(shown[0].message, QStringLiteral("Boundary fence breached"));
    QVERIFY(shown[1].message.startsWith(QStringLiteral("Approaching geozone P")));
    QCOMPARE(kinds(), (QList<GeoZoneMonitor::AlertEvent::Kind>{ GeoZoneMonitor::AlertEvent::Approach, GeoZoneMonitor::AlertEvent::FenceWarning,
                                                                GeoZoneMonitor::AlertEvent::FenceBreach }));
    QCOMPARE(sink->spoken.count(), 2);

    // An action without manoeuvre, like ArduPilot's report only, is named after the time and distance
    config.action = 0;
    vehicleMonitor->fenceWarning()->setConfig(config);
    QCOMPARE(GeoZoneFenceWarning::warningText(vehicleMonitor->fenceWarning()->actionText(), false, 6, 42), QStringLiteral("Geofence in 6 s, 42 m ahead: report only, no manoeuvre"));
}

void GeoZoneMonitorTest::_testVehicleMonitors()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300) }), errorString), qPrintable(errorString));

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
    QVERIFY(monitor.activeMonitor()->inside());
    QCOMPARE(monitor.activeMonitor()->verticalBands().count(), 1);
    QCOMPARE(monitor.activeMonitor()->altitudeAMSL(), 450.0);
    monitor.evaluate(&second, away);
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(secondMonitor->nearby().isEmpty());
    QVERIFY(monitor.activeMonitor()->inside());
    monitor.evaluate(&second, inside);
    QCOMPARE(alertSpy.count(), 2);
    monitor.evaluate(first.get(), inside);
    QCOMPARE(alertSpy.count(), 2);

    // The published state follows the active vehicle
    monitor.evaluate(&second, away);
    QSignalSpy activeSpy(&monitor, &GeoZoneMonitor::activeMonitorChanged);
    monitor.setActiveVehicle(&second);
    QCOMPARE(activeSpy.count(), 1);
    QVERIFY(!monitor.activeMonitor()->inside());
    QVERIFY(monitor.activeMonitor()->verticalBands().isEmpty());
    QCOMPARE(monitor.activeMonitor()->altitudeAMSL(), 600.0);
    monitor.setActiveVehicle(first.get());
    QVERIFY(monitor.activeMonitor()->inside());

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
    activeSpy.clear();
    first.reset();
    QCOMPARE(activeSpy.count(), 1);
    QVERIFY(!monitor.activeMonitor());
    monitor.setActiveVehicle(&reconnected);
    monitor.evaluate(&reconnected, inside);
    QCOMPARE(alertSpy.count(), 3);
    QVERIFY(monitor.activeMonitor()->inside());

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
    QVERIFY(!monitor.activeMonitor());
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
    QCOMPARE(announcement.text, QStringLiteral("Caution: Approaching P. Approaching A, and 2 more"));
    QCOMPARE(announcement.alerts.count(), 4);
    QCOMPARE(announcement.alerts[2].id, 1);
    QCOMPARE(announcement.alerts[3].id, 4);
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
    QCOMPARE(arbiter.tick().text, QStringLiteral("Caution: Approaching U2. Approaching C2"));

    // A more urgent alert preempts the interval
    now += 1000;
    arbiter.submit(arbiterAlert(8, 1, QStringLiteral("A2"), GeoZone::ReqAuthorisation, true));
    QCOMPARE(arbiter.tick().text, QStringLiteral("Warning: Entered A2"));
    now += 1000;
    arbiter.submit(arbiterAlert(9, 1, QStringLiteral("A3"), GeoZone::ReqAuthorisation, true));
    QVERIFY(arbiter.tick().text.isEmpty());
    arbiter.acknowledge(9);
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    QVERIFY(arbiter.tick().text.isEmpty());

    // Entering a prohibited zone is repeated until it is acknowledged
    arbiter.submit(arbiterAlert(10, 1, QStringLiteral("P"), GeoZone::Prohibited, true));
    QCOMPARE(arbiter.tick().text, QStringLiteral("Warning: Entered P"));
    QCOMPARE(arbiter.breachCount(), 1);
    arbiter.setZones(1, { QStringLiteral("P") }, { QStringLiteral("P") });
    now += GeoZoneAlertArbiter::repeatIntervalMSecs - 1;
    QVERIFY(arbiter.tick().text.isEmpty());
    now += 1;
    const GeoZoneAlertArbiter::Announcement repeat = arbiter.tick();
    QCOMPARE(repeat.text, QStringLiteral("Warning: Still inside P"));
    QCOMPARE(repeat.repeated.count(), 1);
    QCOMPARE(repeat.repeated.first().id, 10);
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    QCOMPARE(arbiter.tick().text, QStringLiteral("Warning: Still inside P"));
    arbiter.acknowledge(10);
    QCOMPARE(arbiter.breachCount(), 0);
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    QVERIFY(arbiter.tick().text.isEmpty());

    // ... or the vehicle leaves the zone
    arbiter.submit(arbiterAlert(11, 2, QStringLiteral("P"), GeoZone::Prohibited, true));
    QCOMPARE(arbiter.tick().text, QStringLiteral("Warning: Entered P"));
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    arbiter.setZones(2, { QStringLiteral("P"), QStringLiteral("U") }, { QStringLiteral("P"), QStringLiteral("U") });
    QCOMPARE(arbiter.tick().text, QStringLiteral("Warning: Still inside P"));
    arbiter.setZones(2, { QStringLiteral("U") }, { QStringLiteral("P"), QStringLiteral("U") });
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    QVERIFY(arbiter.tick().text.isEmpty());
    QCOMPARE(arbiter.breachCount(), 0);

    // Muted, alerts aren't spoken, but the first announcement of a new breach is made
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
    QCOMPARE(mutedBreach.text, QStringLiteral("Warning: Entered P3"));
    QCOMPARE(mutedBreach.alerts.count(), 1);
    arbiter.setZones(1, { QStringLiteral("P3") }, { QStringLiteral("P3") });

    // Its repeats are shown but not spoken until the end of the mute, the alerts dropped meanwhile aren't announced
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    const GeoZoneAlertArbiter::Announcement mutedRepeat = arbiter.tick();
    QVERIFY(mutedRepeat.text.isEmpty());
    QCOMPARE(mutedRepeat.repeated.count(), 1);
    QCOMPARE(mutedRepeat.repeated.first().id, 14);
    now = mutedUntil;
    QVERIFY(!arbiter.muted());
    QVERIFY(arbiter.tick().text.isEmpty());
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    QCOMPARE(arbiter.tick().text, QStringLiteral("Warning: Still inside P3"));

    // A vehicle no longer monitored is forgotten
    arbiter.removeVehicle(1);
    QCOMPARE(arbiter.breachCount(), 0);
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    QVERIFY(arbiter.tick().text.isEmpty());

    // The breaches of a vehicle whose position is lost are kept but not repeated until it is back
    arbiter.submit(arbiterAlert(15, 3, QStringLiteral("P4"), GeoZone::Prohibited, true));
    QCOMPARE(arbiter.tick().text, QStringLiteral("Warning: Entered P4"));
    arbiter.setSuspended(3, true);
    QVERIFY(arbiter.suspended(3));
    now += GeoZoneAlertArbiter::repeatIntervalMSecs * 2;
    QVERIFY(arbiter.tick().text.isEmpty());
    QCOMPARE(arbiter.breachCount(), 1);
    arbiter.setSuspended(3, false);
    QCOMPARE(arbiter.tick().text, QStringLiteral("Warning: Still inside P4"));
    arbiter.removeVehicle(3);

    // A zone approved for a vehicle drops its alerts and breach until the vehicle leaves the zone and its approach range
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    arbiter.submit(arbiterAlert(16, 4, QStringLiteral("P5"), GeoZone::Prohibited, true));
    arbiter.submit(arbiterAlert(17, 5, QStringLiteral("P5"), GeoZone::Prohibited, true));
    arbiter.approveZone(4, QStringLiteral("P5"));
    QVERIFY(arbiter.zoneApproved(4, QStringLiteral("P5")));
    QVERIFY(!arbiter.zoneApproved(5, QStringLiteral("P5")));
    QCOMPARE(arbiter.pendingCount(), 1);
    QCOMPARE(arbiter.breachCount(), 1);
    arbiter.submit(arbiterAlert(18, 4, QStringLiteral("P5"), GeoZone::Prohibited, true));
    QCOMPARE(arbiter.pendingCount(), 1);
    arbiter.setZones(4, { QStringLiteral("P5") }, { QStringLiteral("P5") });
    QCOMPARE(arbiter.approvedZones(4), QSet<QString>{ QStringLiteral("P5") });
    QCOMPARE(arbiter.approvalCount(), 1);
    arbiter.setZones(4, {}, { QStringLiteral("P5") });
    QVERIFY(arbiter.zoneApproved(4, QStringLiteral("P5")));
    arbiter.setZones(4, {}, {});
    QVERIFY(!arbiter.zoneApproved(4, QStringLiteral("P5")));
    QCOMPARE(arbiter.approvalCount(), 0);
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
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
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
    settings.limits = GeoZoneAlertTracker::Limits{ 100, 30, 0 };
    monitor.setSettings(settings);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    QSignalSpy repeatSpy(&monitor, &GeoZoneMonitor::alertRepeated);
    QSignalSpy mutedSpy(&monitor, &GeoZoneMonitor::alertsMutedChanged);

    // The alerts of an evaluation are shown one by one, but spoken as one message, the most severe first
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 3);
    QCOMPARE(sink->spoken, QStringList{ QStringLiteral("Caution: Approaching geozone P (Prohibited). Approaching geozone A (Authorisation required), and 1 more") });

    // Entering the prohibited zone right after is more urgent and spoken at once
    const GeoZone::Position inProhibited{ center.atDistanceAndAzimuth(100, 180) };
    const GeoZone::Position away{ center.atDistanceAndAzimuth(5000, 0) };
    now += 1000;
    monitor.evaluate(inProhibited);
    QCOMPARE(alertSpy.count(), 4);
    QCOMPARE(sink->spoken.last(), QStringLiteral("Warning: Entered geozone P (Prohibited)"));
    const int breachId = alertSpy.last()[2].toInt();
    QVERIFY(breachId > alertSpy.first()[2].toInt());

    // Repeated and shown again while the vehicle stays inside, until it is acknowledged
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(sink->spoken.count(), 3);
    QCOMPARE(sink->spoken.last(), QStringLiteral("Warning: Still inside geozone P (Prohibited)"));
    QCOMPARE(repeatSpy.count(), 1);
    QCOMPARE(QStringLiteral("Warning: ") + repeatSpy.last()[0].toString(), sink->spoken.last());
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
    QCOMPARE(sink->spoken.last(), QStringLiteral("Warning: Entered geozone P (Prohibited). Entered geozone C (Conditional)"));
    history->acknowledgeAll();
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(sink->spoken.count(), 4);

    // Muted, approaches and entries are shown but not spoken, and a new entry into a prohibited zone is spoken
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
    QCOMPARE(sink->spoken.last(), QStringLiteral("Warning: Entered geozone P (Prohibited)"));

    // Its repeats are shown, but only spoken after the end of the mute
    const qsizetype repeats = repeatSpy.count();
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(inProhibited);
    QCOMPARE(sink->spoken.count(), 5);
    QCOMPARE(repeatSpy.count(), repeats + 1);
    QVERIFY(shownAlert(monitor, repeatSpy.last()[2].toInt()));
    now += GeoZoneAlertArbiter::defaultMuteMSecs;
    monitor.evaluate(inProhibited);
    QVERIFY(!monitor.alertsMuted());
    QCOMPARE(mutedSpy.count(), 2);
    QCOMPARE(sink->spoken.last(), QStringLiteral("Warning: Still inside geozone P (Prohibited)"));

    // Mutes are limited to a day
    monitor.muteAlerts(std::numeric_limits<int>::max());
    QCOMPARE(monitor.arbiter().mutedUntil(), now + (qint64{ GeoZoneMonitor::maxMuteSecs } * 1000));
    monitor.unmuteAlerts();
    QVERIFY(!monitor.alertsMuted());

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
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
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

    // An offline vehicle has no position of its own, like a vehicle whose position is lost
    Vehicle vehicle(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    GeoZoneVehicleMonitor *const vehicleMonitor = monitor.addVehicle(&vehicle);
    monitor.setActiveVehicle(&vehicle);
    QCOMPARE(monitor.activeMonitor(), vehicleMonitor);
    const GeoZone::Position inside{ center.atDistanceAndAzimuth(100, 0), 450, 400 };
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Entered geozone P (Prohibited)"));
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(repeatSpy.count(), 1);

    // Losing the position is alerted once with the zone the vehicle was in, and the breach isn't repeated while the
    // vehicle may have left the zone. Its nearby zones and bands are kept as stale.
    QSignalSpy unavailableSpy(vehicleMonitor, &GeoZoneVehicleMonitor::positionUnavailableChanged);
    now += 1000;
    monitor.evaluateVehicles();
    QVERIFY(vehicleMonitor->positionUnavailable());
    QCOMPARE(unavailableSpy.count(), 1);
    QVERIFY(monitor.arbiter().suspended(vehicleMonitor->key()));
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Position lost inside geozone P (Prohibited)"));
    QCOMPARE(alertSpy.last()[1].toInt(), GeoZoneAlertLevel::severity(GeoZoneAlertLevel::Caution, GeoZone::restrictionSeverity(GeoZone::Prohibited)));
    // A caution, spoken after the warning repeated a second ago
    QCOMPARE(sink->spoken.last(), QStringLiteral("Warning: Still inside geozone P (Prohibited)"));
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    monitor.evaluateVehicles();
    QCOMPARE(sink->spoken.last(), QStringLiteral("Caution: ") + alertSpy.last()[0].toString());
    const GeoZoneMonitor::AlertEvent lost = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY(lost.kind == GeoZoneMonitor::AlertEvent::PositionLost);
    QVERIFY(lost.inside);
    QCOMPARE(lost.position.coordinate, inside.coordinate);
    QVERIFY(vehicleMonitor->inside());
    QCOMPARE(vehicleMonitor->verticalBands().count(), 1);
    QCOMPARE(vehicleMonitor->predictedEntryTime(), -1);
    for (int i = 0; i < 3; i++) {
        now += GeoZoneAlertArbiter::repeatIntervalMSecs;
        monitor.evaluateVehicles();
    }
    QCOMPARE(repeatSpy.count(), 1);
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(monitor.arbiter().breachCount(), 1);

    // When the position is back the breach resumes at once if the vehicle is still inside, and ends when it leaves
    monitor.evaluate(&vehicle, inside);
    QVERIFY(!vehicleMonitor->positionUnavailable());
    QVERIFY(!monitor.arbiter().suspended(vehicleMonitor->key()));
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(repeatSpy.count(), 2);
    QCOMPARE(sink->spoken.last(), QStringLiteral("Warning: Still inside geozone P (Prohibited)"));
    monitor.evaluate(&vehicle, { center.atDistanceAndAzimuth(1000, 0), 450, 400 });
    QCOMPARE(monitor.arbiter().breachCount(), 0);

    // Far from any zone, nothing is alerted
    monitor.evaluate(&vehicle, { center.atDistanceAndAzimuth(10000, 0), 450, 400 });
    QVERIFY(vehicleMonitor->nearby().isEmpty());
    const qsizetype alertCount = alertSpy.count();
    monitor.evaluateVehicles();
    QVERIFY(vehicleMonitor->positionUnavailable());
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
    const QString line = GeoZoneAlertHistory::logLine(nearLost);
    QCOMPARE(line.section(QLatin1Char(','), 6, 6), QStringLiteral("position_lost"));
    const std::optional<GeoZoneMonitor::AlertEvent> read = GeoZoneAlertHistory::parseLogLine(line);
    QVERIFY(read);
    QVERIFY(read->kind == GeoZoneMonitor::AlertEvent::PositionLost);
    QVERIFY(!read->inside);
    QCOMPARE(read->message, QStringLiteral("Position lost near geozone C (Conditional)"));
    QVERIFY(GeoZoneAlertHistory::parseLogLine(GeoZoneAlertHistory::logLine(lost))->inside);
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
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        prohibited,
        _circleFeature(QStringLiteral("U"), QStringLiteral("USPACE"), uSpaceCenter, 300),
    }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    qint64 now = 0;
    monitor.setClock([&now]() { return now; });
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    QSignalSpy repeatSpy(&monitor, &GeoZoneMonitor::alertRepeated);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
    QSignalSpy approvalsSpy(&monitor, &GeoZoneMonitor::approvalsChanged);
    GeoZoneAlertModel *const shown = monitor.alertModel();
    GeoZoneAlertHistory *const history = monitor.alertHistory();
    Vehicle vehicle(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    (void) monitor.addVehicle(&vehicle);
    monitor.setActiveVehicle(&vehicle);
    QCOMPARE(shown->count(), 0);

    // 50 m above the ground inside the restricted zone, the pilot is asked for the permission to fly there, and the
    // active vehicle climbs 100 m to be 30 m above its ceiling
    const GeoZone::Position inside{ center.atDistanceAndAzimuth(100, 0), 450, 400 };
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(alertSpy.count(), 1);
    const int entryId = alertSpy.last()[2].toInt();
    const QString zoneKey = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().zoneKey;
    std::optional<GeoZoneAlertModel::Alert> alert = shownAlert(monitor, entryId);
    QVERIFY(alert);
    QCOMPARE(alert->message, alertSpy.last()[0].toString());
    QCOMPARE(alert->zoneKey, zoneKey);
    QVERIFY(alert->question);
    QVERIFY(alert->activeVehicle);
    QVERIFY(alert->inside);
    QCOMPARE(alert->climbHeight, 120 + GeoZoneMonitor::climbClearance - 50);
    QCOMPARE(shown->questionCount(), 1);
    QVERIFY(shown->activeVehicleAlerts());
    QCOMPARE(shown->climbHeight(), alert->climbHeight);

    // The rows are notified only when their actions change
    QSignalSpy dataSpy(shown, &QAbstractItemModel::dataChanged);
    monitor.setActiveVehicle(nullptr);
    QVERIFY(!shownAlert(monitor, entryId)->activeVehicle);
    QVERIFY(!shown->activeVehicleAlerts());
    QVERIFY(std::isnan(shown->climbHeight()));
    monitor.setActiveVehicle(&vehicle);
    QCOMPARE(dataSpy.count(), 2);
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(dataSpy.count(), 2);

    // Confirming the permission acknowledges the alert and ends the alerts and repeats about the zone
    approvalsSpy.clear();
    monitor.approveAlert(entryId);
    QCOMPARE(approvalsSpy.count(), 1);
    QCOMPARE(monitor.approvedZoneKeys(), QStringList{ zoneKey });
    QCOMPARE(shown->count(), 0);
    QCOMPARE(monitor.arbiter().breachCount(), 0);
    QCOMPARE(history->unacknowledgedCount(), 0);
    now += GeoZoneAlertArbiter::repeatIntervalMSecs;
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(repeatSpy.count(), 0);

    // ... until the vehicle leaves the zone and its approach range: entering again from near it is only logged
    monitor.evaluate(&vehicle, { center.atDistanceAndAzimuth(350, 0), 450, 400 });
    QCOMPARE(monitor.approvedZoneKeys(), QStringList{ zoneKey });
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(eventSpy.count(), 2);
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(shown->count(), 0);
    QCOMPARE(monitor.arbiter().breachCount(), 0);
    QCOMPARE(history->unacknowledgedCount(), 0);
    monitor.evaluate(&vehicle, { center.atDistanceAndAzimuth(1000, 0), 450, 400 });
    QVERIFY(monitor.approvedZoneKeys().isEmpty());
    QCOMPARE(approvalsSpy.count(), 2);
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    monitor.evaluate(&vehicle, inside);
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(monitor.arbiter().breachCount(), 1);
    const int againId = alertSpy.last()[2].toInt();
    QVERIFY(shownAlert(monitor, againId)->question);

    // Without a current position the vehicle isn't known to be inside nor able to climb out, the question stands
    monitor.evaluateVehicles();
    alert = shownAlert(monitor, againId);
    QVERIFY(!alert->inside);
    QVERIFY(std::isnan(alert->climbHeight));
    QVERIFY(alert->question);
    QVERIFY(!shownAlert(monitor, alertSpy.last()[2].toInt())->question);

    // U-space airspace asks nothing and has no ceiling to climb above. Far from the restricted zone, its question
    // no longer stands.
    now += GeoZoneAlertArbiter::minIntervalMSecs;
    monitor.evaluate(&vehicle, { uSpaceCenter, 450, 400 });
    QCOMPARE(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().zoneIdentifier, QStringLiteral("U"));
    const int uSpaceId = alertSpy.last()[2].toInt();
    alert = shownAlert(monitor, uSpaceId);
    QVERIFY(alert->inside);
    QVERIFY(!alert->question);
    QVERIFY(std::isnan(alert->climbHeight));
    QVERIFY(!shownAlert(monitor, againId)->question);
    QCOMPARE(shown->questionCount(), 0);

    // An alert dismissed is acknowledged, those closed are not
    const int unacknowledged = history->unacknowledgedCount();
    monitor.dismissAlert(uSpaceId);
    QVERIFY(!shownAlert(monitor, uSpaceId));
    QCOMPARE(history->unacknowledgedCount(), unacknowledged - 1);
    monitor.closeAlerts();
    QCOMPARE(shown->count(), 0);
    QCOMPARE(history->unacknowledgedCount(), unacknowledged - 1);

    // The alerts of a vehicle no longer monitored have no actions, approving one only dismisses it
    monitor.evaluate(&vehicle, inside);
    const int lastId = alertSpy.last()[2].toInt();
    QVERIFY(shownAlert(monitor, lastId)->question);
    monitor.removeVehicle(&vehicle);
    alert = shownAlert(monitor, lastId);
    QVERIFY(!alert->question);
    QVERIFY(!alert->activeVehicle);
    monitor.approveAlert(lastId);
    QVERIFY(!shownAlert(monitor, lastId));
    QVERIFY(monitor.approvedZoneKeys().isEmpty());

    // The same zone published with other limits by a second source (identical copies are merged by the catalog) is
    // shown once, entering it replaces its approach, and confirming the permission covers both
    QJsonObject copy = prohibited;
    QJsonArray copyGeometry = copy[QStringLiteral("geometry")].toArray();
    QJsonObject copyLayer = copyGeometry[0].toObject();
    copyLayer[QStringLiteral("upperLimit")] = 150;
    copyGeometry[0] = copyLayer;
    copy[QStringLiteral("geometry")] = copyGeometry;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("copy.json"), QJsonArray{ copy }), errorString), qPrintable(errorString));
    monitor.closeAlerts();
    const int earlierUnacknowledged = history->unacknowledgedCount();
    monitor.evaluate({ center.atDistanceAndAzimuth(380, 0), 450, 400 }, 9);
    QCOMPARE(shown->count(), 1);
    QCOMPARE(shown->alerts().first().zoneKeys.count(), 2);
    monitor.evaluate({ center.atDistanceAndAzimuth(100, 0), 450, 400 }, 9);
    QCOMPARE(shown->count(), 1);
    QVERIFY(shown->alerts().first().message.startsWith(QStringLiteral("Entered geozone P")));
    QCOMPARE(shown->alerts().first().zoneKeys.count(), 2);
    QCOMPARE(monitor.arbiter().breachCount(), 2);
    QCOMPARE(history->unacknowledgedCount(), earlierUnacknowledged + 4);
    monitor.approveAlert(shown->alerts().first().alertId);
    QCOMPARE(shown->count(), 0);
    QCOMPARE(monitor.arbiter().approvedZones(monitor.positionMonitor(9)->key()).count(), 2);
    QCOMPARE(monitor.arbiter().breachCount(), 0);
    QCOMPARE(history->unacknowledgedCount(), earlierUnacknowledged);
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
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300), low,
    }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);
    GeoZoneMonitor::Settings settings;
    settings.limits = GeoZoneAlertTracker::Limits{ 100, 30, 0 };
    settings.trafficEnabled = true;
    monitor.setSettings(settings);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);

    const auto aircraft = [](quint32 address, const QString &callsign, const QGeoCoordinate &coordinate, double altitude) {
        GeoZoneMonitor::Aircraft plane;
        plane.address = address;
        plane.callsign = callsign;
        plane.coordinate = QGeoCoordinate(coordinate.latitude(), coordinate.longitude(), altitude);
        return plane;
    };
    const GeoZoneMonitor::Aircraft inP = aircraft(0x4B1234, QStringLiteral("SWR123 ") + QChar(0), center.atDistanceAndAzimuth(100, 90), 600);
    const GeoZoneMonitor::Aircraft nearP = aircraft(0x4B5678, QString(), center.atDistanceAndAzimuth(360, 90), 700);
    const GeoZoneMonitor::Aircraft far = aircraft(0x4B9999, QStringLiteral("FAR"), center.atDistanceAndAzimuth(20000, 0), 600);

    // Traffic is checked around the active vehicle, here 5 km north of the prohibited zone, within the radius
    monitor.evaluateTraffic({ inP });
    QVERIFY(monitor.traffic().isEmpty());
    monitor.evaluate({ center.atDistanceAndAzimuth(5000, 0), 500, 400 });
    QCOMPARE(alertSpy.count(), 0);
    QSignalSpy trafficSpy(&monitor, &GeoZoneMonitor::trafficChanged);
    monitor.evaluateTraffic({ far, nearP, inP });
    QCOMPARE(trafficSpy.count(), 1);
    const QVariantList listed = monitor.traffic();
    QCOMPARE(listed.count(), 2);
    QCOMPARE(listed[0].toMap().value(QStringLiteral("callsign")).toString(), QStringLiteral("SWR123"));
    QVERIFY(listed[0].toMap().value(QStringLiteral("inside")).toBool());
    QCOMPARE(listed[0].toMap().value(QStringLiteral("altitude")).toDouble(), 600.0);
    QCOMPARE(GeoZoneInfo::fromVariant(listed[0].toMap().value(QStringLiteral("zone")))->identifier(), QStringLiteral("P"));
    QCOMPARE(listed[1].toMap().value(QStringLiteral("callsign")).toString(), QStringLiteral("4B5678"));
    QVERIFY(!listed[1].toMap().value(QStringLiteral("inside")).toBool());
    QCOMPARE(listed[1].toMap().value(QStringLiteral("distance")).toInt(), 60);

    // The aircraft entering the zone is shown, less urgent than the vehicles, but not spoken
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Traffic SWR123 entered geozone P (Prohibited)"));
    QCOMPARE(alertSpy.last()[1].toInt(), GeoZoneMonitor::trafficSeverity + GeoZoneAlertLevel::severity(GeoZoneAlertLevel::Advisory, GeoZone::restrictionSeverity(GeoZone::Prohibited)));
    QVERIFY(sink->spoken.isEmpty());
    const GeoZoneMonitor::AlertEvent event = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY(event.kind == GeoZoneMonitor::AlertEvent::Traffic);
    QCOMPARE(event.traffic, QStringLiteral("SWR123"));
    QCOMPARE(event.vehicleId, GeoZoneMonitor::noVehicle);
    const std::optional<GeoZoneAlertModel::Alert> trafficAlert = shownAlert(monitor, event.id);
    QVERIFY(trafficAlert && !trafficAlert->question && !trafficAlert->activeVehicle);
    const std::optional<GeoZoneMonitor::AlertEvent> read = GeoZoneAlertHistory::parseLogLine(GeoZoneAlertHistory::logLine(event));
    QVERIFY(read && (read->kind == GeoZoneMonitor::AlertEvent::Traffic));
    QCOMPARE(read->traffic, QStringLiteral("SWR123"));
    QCOMPARE(read->message, event.message);

    // Once per entry, and aircraft gone are forgotten
    monitor.evaluateTraffic({ inP });
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(monitor.traffic().count(), 1);

    // An aircraft coming back enters again
    monitor.evaluateTraffic({ inP, aircraft(0x4B5678, QString(), center, 700) });
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Traffic 4B5678 entered geozone P (Prohibited)"));
    QVERIFY(sink->spoken.isEmpty());

    // Without terrain data below an aircraft, the ground below the vehicle is used for limits above the ground
    monitor.evaluateTraffic({ aircraft(1, QStringLiteral("HIGH"), lowCenter, 600), aircraft(2, QStringLiteral("LOW"), lowCenter, 450) });
    QCOMPARE(monitor.traffic().count(), 1);
    QCOMPARE(monitor.traffic().first().toMap().value(QStringLiteral("callsign")).toString(), QStringLiteral("LOW"));
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Traffic LOW entered geozone LOW (Authorisation required)"));

    // Turned off, the traffic is forgotten
    settings.trafficEnabled = false;
    monitor.setSettings(settings);
    QVERIFY(monitor.traffic().isEmpty());
    monitor.evaluateTraffic({ inP });
    QVERIFY(monitor.traffic().isEmpty());
}

void GeoZoneMonitorTest::_testGroundAlerts()
{
    ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate authorisationCenter = center.atDistanceAndAzimuth(3000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), authorisationCenter, 300),
    }), errorString), qPrintable(errorString));

    GeoZoneMonitor monitor(&manager);
    qint64 now = 0;
    monitor.setClock([&now]() { return now; });
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);

    // A vehicle on the ground, known to be disarmed, is told it is inside the prohibited zone like by a pre-arm check,
    // asked for the permission and reminded
    constexpr int vehicleId = 2;
    monitor.evaluate({ center.atDistanceAndAzimuth(10000, 0), 400, 400 }, vehicleId);
    GeoZoneVehicleMonitor *const vehicle = monitor.positionMonitor(vehicleId);
    QVERIFY(vehicle && !vehicle->onGround() && !vehicle->inFlight());
    vehicle->setArmed(false);
    QVERIFY(vehicle->onGround());
    const GeoZone::Position inside{ center, 400, 400 };
    monitor.evaluate(inside, vehicleId);
    const QString onGround = QStringLiteral("On the ground inside prohibited geozone P \u2014 take-off not allowed here");
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(alertSpy.last()[0].toString(), onGround);
    QCOMPARE(alertSpy.last()[1].toInt(), GeoZoneAlertLevel::severity(GeoZoneAlertLevel::Warning, GeoZone::restrictionSeverity(GeoZone::Prohibited)));
    QCOMPARE(sink->spoken.last(), QStringLiteral("Warning: ") + onGround);
    const GeoZoneMonitor::AlertEvent event = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY(event.kind == GeoZoneMonitor::AlertEvent::OnGround);
    QVERIFY(shownAlert(monitor, event.id)->question);
    QCOMPARE(vehicle->groundWarning(), onGround);
    QCOMPARE(vehicle->groundLevel(), static_cast<int>(GeoZoneAlertLevel::Warning));
    QCOMPARE(monitor.arbiter().breachCount(), 1);

    // Logged as such and read back
    const QString line = GeoZoneAlertHistory::logLine(event);
    QCOMPARE(line.section(QLatin1Char(','), 6, 6), QStringLiteral("on_ground"));
    const std::optional<GeoZoneMonitor::AlertEvent> read = GeoZoneAlertHistory::parseLogLine(line);
    QVERIFY(read && read->inside);
    QCOMPARE(read->message, onGround);

    // Trying to arm alerts again, once for the acknowledged command and the arming which follows
    mavlink_message_t ack{};
    (void) mavlink_msg_command_ack_pack(vehicleId, MAV_COMP_ID_AUTOPILOT1, &ack, MAV_CMD_COMPONENT_ARM_DISARM, MAV_RESULT_DENIED, 0, 0, 0, 0);
    now += 1000;
    vehicle->handleMessage(ack, now);
    const QString arming = QStringLiteral("Arming inside prohibited geozone P \u2014 take-off not allowed here");
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(alertSpy.last()[0].toString(), arming);
    QCOMPARE(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().detail, QStringLiteral("arming"));
    QCOMPARE(GeoZoneAlertHistory::parseLogLine(GeoZoneAlertHistory::logLine(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>()))->message, arming);
    QCOMPARE(monitor.alertModel()->count(), 1);
    now += 1000;
    vehicle->handleMessage(ack, now);
    vehicle->setArmed(true);
    QCOMPARE(alertSpy.count(), 2);
    QVERIFY(vehicle->groundWarning().isEmpty());
    // Disarmed and armed again later, like with the remote control, which sends no command
    vehicle->setArmed(false);
    QCOMPARE(vehicle->groundWarning(), onGround);
    now += GeoZoneMonitor::armingAlertIntervalMSecs;
    vehicle->setArmed(true);
    QCOMPARE(alertSpy.count(), 3);
    QCOMPARE(alertSpy.last()[0].toString(), arming);
    // An armed vehicle sends no arming attempt
    now += GeoZoneMonitor::armingAlertIntervalMSecs;
    vehicle->handleMessage(ack, now);
    QCOMPARE(alertSpy.count(), 3);

    // The pilot confirming the permission ends the warning and the alerts, until the vehicle leaves the zone
    vehicle->setArmed(false);
    monitor.approveAlert(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().id);
    QCOMPARE(vehicle->groundLevel(), static_cast<int>(GeoZoneAlertLevel::Advisory));
    QCOMPARE(vehicle->groundWarning(), QStringLiteral("On the ground inside geozone P (Prohibited), permission confirmed"));
    QCOMPARE(monitor.arbiter().breachCount(), 0);
    now += GeoZoneMonitor::armingAlertIntervalMSecs;
    vehicle->setArmed(true);
    QCOMPARE(alertSpy.count(), 3);

    // Inside a zone requiring an authorisation, a caution
    vehicle->setArmed(false);
    monitor.evaluate({ authorisationCenter, 400, 400 }, vehicleId);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("On the ground inside geozone A (Authorisation required) \u2014 take-off only with an authorisation"));
    QCOMPARE(alertSpy.last()[1].toInt(), GeoZoneAlertLevel::severity(GeoZoneAlertLevel::Caution, GeoZone::restrictionSeverity(GeoZone::ReqAuthorisation)));
    QCOMPARE(vehicle->groundLevel(), static_cast<int>(GeoZoneAlertLevel::Caution));
    QCOMPARE(GeoZoneMonitor::groundLevel(GeoZone::ReqAuthorisation, true), static_cast<int>(GeoZoneAlertLevel::Advisory));
    QCOMPARE(GeoZoneMonitor::groundLevel(GeoZone::USpace, false), static_cast<int>(GeoZoneAlertLevel::Advisory));

    // A flying vehicle enters zones, and a vehicle whose armed state isn't known isn't on the ground
    vehicle->setArmed(true);
    monitor.evaluate({ center.atDistanceAndAzimuth(10000, 0), 400, 400 }, vehicleId);
    monitor.evaluate({ center, 450, 400 }, vehicleId);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Entered geozone P (Prohibited)"));
    QVERIFY(vehicle->groundWarning().isEmpty());
    monitor.evaluate({ authorisationCenter, 400, 400 }, vehicleId + 1);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Vehicle 3: Entered geozone A (Authorisation required)"));
}


void GeoZoneMonitorTest::_testAwareness()
{
    using Awareness = GeoZoneAwareness;

    // Each reason, the unavailable ones first
    Awareness::Input input;
    QCOMPARE(Awareness::evaluate(input).state, Awareness::Unavailable);
    QCOMPARE(Awareness::evaluate(input).reasons(), QStringList{ QStringLiteral("no geozone data loaded") });
    input.zoneCount = 10;
    QCOMPARE(Awareness::evaluate(input).state, Awareness::Available);
    QCOMPARE(Awareness::evaluate(input).summary(), QStringLiteral("Geo-awareness available"));
    input.vehicle = true;
    input.covered = true;
    input.gnss = Awareness::Gnss{ true, 3, 10, 0.9, 1.2 };
    QCOMPARE(Awareness::evaluate(input).state, Awareness::Available);
    const auto check = [&input](const std::function<void(Awareness::Input &)> &change, Awareness::State state, const QString &summary) {
        Awareness::Input changed = input;
        change(changed);
        const Awareness::Status status = Awareness::evaluate(changed);
        return (status.state == state) && (status.summary() == summary);
    };
    QVERIFY(check([](Awareness::Input &in) { in.gnss.fixType = 2; }, Awareness::Degraded, QStringLiteral("Geo-awareness degraded: GNSS poor (2D fix)")));
    QVERIFY(check([](Awareness::Input &in) { in.gnss.satellites = 4; }, Awareness::Degraded, QStringLiteral("Geo-awareness degraded: GNSS poor (4 satellite(s))")));
    QVERIFY(check([](Awareness::Input &in) { in.gnss.hdop = 6.5; }, Awareness::Degraded, QStringLiteral("Geo-awareness degraded: GNSS poor (HDOP 6.5)")));
    // The manufacturer's profile sets the limits
    QVERIFY(check([](Awareness::Input &in) { in.gnss.hdop = 6.5; in.gnssLimits.maxHdop = 8; }, Awareness::Available, QStringLiteral("Geo-awareness available")));
    QVERIFY(check([](Awareness::Input &in) { in.gnss.horizontalAccuracy = 8; }, Awareness::Degraded, QStringLiteral("Geo-awareness degraded: GNSS poor (accuracy 8.0 m)")));
    QVERIFY(check([](Awareness::Input &in) { in.gnss.fixType = 1; }, Awareness::Unavailable, QStringLiteral("Geo-awareness unavailable: GNSS lost (no fix)")));
    QVERIFY(check([](Awareness::Input &in) { in.positionUnavailable = true; }, Awareness::Unavailable, QStringLiteral("Geo-awareness unavailable: no current vehicle position")));
    // A lost link explains the rest
    QVERIFY(check([](Awareness::Input &in) { in.linkLost = true; in.positionUnavailable = true; in.gnss.fixType = 1; }, Awareness::Unavailable,
                  QStringLiteral("Geo-awareness unavailable: telemetry link lost")));
    QVERIFY(check([](Awareness::Input &in) { in.validityWarning = QStringLiteral("A: 40 days old"); }, Awareness::Degraded, QStringLiteral("Geo-awareness degraded: geozone data may be outdated")));
    QVERIFY(check([](Awareness::Input &in) { in.validityWarning = QStringLiteral("A: 40 days old"); in.dataInvalid = true; }, Awareness::Degraded,
                  QStringLiteral("Geo-awareness degraded: geozone data expired")));
    QVERIFY(check([](Awareness::Input &in) { in.failedSources = { QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D") }; }, Awareness::Degraded,
                  QStringLiteral("Geo-awareness degraded: update of A, B, C, 1 more failed")));
    QVERIFY(check([](Awareness::Input &in) { in.covered = false; }, Awareness::Degraded, QStringLiteral("Geo-awareness degraded: no geozone data covers the vehicle position")));
    QVERIFY(check([](Awareness::Input &in) { in.covered = false; in.positionUnavailable = true; }, Awareness::Unavailable,
                  QStringLiteral("Geo-awareness unavailable: no current vehicle position; no geozone data covers the vehicle position")));
    // Without a vehicle, only the data counts
    QVERIFY(check([](Awareness::Input &in) { in.vehicle = false; in.positionUnavailable = true; in.gnss.fixType = 1; }, Awareness::Available, QStringLiteral("Geo-awareness available")));

    // Changes of state are alerted once they last, in flight only
    Awareness::Status degraded;
    degraded.state = Awareness::Degraded;
    Awareness::Tracker tracker;
    QVERIFY(!tracker.update(degraded, false, 0));
    QVERIFY(!tracker.update(degraded, false, 10000));
    QVERIFY(!tracker.update(degraded, true, 10000));
    QVERIFY(!tracker.update(degraded, true, 10000 + Awareness::Tracker::worseSettleMSecs - 1));
    std::optional<Awareness::Tracker::Change> change = tracker.update(degraded, true, 10000 + Awareness::Tracker::worseSettleMSecs);
    QVERIFY(change && (change->from == Awareness::Available) && (change->to == Awareness::Degraded));
    QVERIFY(!tracker.update(degraded, true, 20000));
    QVERIFY(!tracker.update(Awareness::Status(), true, 21000));
    QVERIFY(!tracker.update(degraded, true, 22000));
    QVERIFY(!tracker.update(Awareness::Status(), true, 23000));
    QVERIFY(!tracker.update(Awareness::Status(), true, 23000 + Awareness::Tracker::betterSettleMSecs - 1));
    change = tracker.update(Awareness::Status(), true, 23000 + Awareness::Tracker::betterSettleMSecs);
    QVERIFY(change && (change->to == Awareness::Available));
    QCOMPARE(Awareness::alertLevel(Awareness::Unavailable), static_cast<int>(GeoZoneAlertLevel::Warning));
    QCOMPARE(Awareness::alertLevel(Awareness::Degraded), static_cast<int>(GeoZoneAlertLevel::Caution));
    QCOMPARE(Awareness::alertLevel(Awareness::Available), static_cast<int>(GeoZoneAlertLevel::Advisory));

    // A flying vehicle near the zones of a feed, current a day after it was issued
    ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    manager.repository()->addUrl(server.url(QStringLiteral("/ok")));
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    GeoZoneSource *const feed = manager.repository()->source(server.url(QStringLiteral("/ok")));
    QVERIFY(feed && feed->box().isValid());
    environment.setNow(feed->dataDate().addDays(1));
    manager.repository()->updateValidity();
    QVERIFY2(manager.repository()->validityWarning().isEmpty(), qPrintable(manager.repository()->validityWarning()));
    QVERIFY(GeoZoneAwareness::dataInput(&manager, feed->box().center()).covered == true);
    QVERIFY(GeoZoneAwareness::dataInput(&manager, QGeoCoordinate(-30, 100)).covered == false);
    QVERIFY(!GeoZoneAwareness::dataInput(&manager).covered.has_value());

    GeoZoneMonitor monitor(&manager);
    qint64 now = 0;
    monitor.setClock([&now]() { return now; });
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
    constexpr int vehicleId = 1;
    // High above the zones
    const GeoZone::Position position{ feed->box().center(), 20000, 400 };
    monitor.setActiveVehicleId(vehicleId);
    monitor.evaluate(position, vehicleId);
    GeoZoneVehicleMonitor *const vehicle = monitor.positionMonitor(vehicleId);
    QVERIFY(vehicle);
    vehicle->setArmed(true);
    mavlink_message_t gps = gpsMessage(vehicleId, GPS_FIX_TYPE_3D_FIX, 12, 0.8, 1.5);
    const auto step = [&](qint64 msecs) {
        now += msecs;
        vehicle->handleMessage(gps, now);
        monitor.evaluate(position, vehicleId);
    };
    const auto awarenessAlerts = [&alertSpy]() {
        QStringList alerts;
        for (const QList<QVariant> &arguments : alertSpy) {
            if (arguments.constFirst().toString().startsWith(QStringLiteral("Geo-awareness"))) {
                alerts.append(arguments.constFirst().toString());
            }
        }
        return alerts;
    };
    step(1000);
    QVERIFY2(monitor.awareness()->status().state == Awareness::Available, qPrintable(monitor.awareness()->summary()));
    QVERIFY(awarenessAlerts().isEmpty());

    // Losing the GNSS fix is a warning once it lasts, spoken and logged
    gps = gpsMessage(vehicleId, GPS_FIX_TYPE_NO_FIX, 3, 0, 0);
    step(1000);
    QCOMPARE(monitor.awareness()->stateValue(), static_cast<int>(Awareness::Unavailable));
    QCOMPARE(monitor.awareness()->reasons(), QStringList{ QStringLiteral("GNSS lost (no fix)") });
    QVERIFY(awarenessAlerts().isEmpty());
    step(Awareness::Tracker::worseSettleMSecs);
    const QString lost = QStringLiteral("Geo-awareness unavailable: GNSS lost (no fix)");
    QCOMPARE(awarenessAlerts(), QStringList{ lost });
    QCOMPARE(alertSpy.last()[1].toInt(), GeoZoneAlertLevel::severity(GeoZoneAlertLevel::Warning, 0));
    QCOMPARE(sink->spoken.last(), QStringLiteral("Warning: ") + lost);
    const GeoZoneMonitor::AlertEvent event = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY(event.kind == GeoZoneMonitor::AlertEvent::Awareness);
    const QString line = GeoZoneAlertHistory::logLine(event);
    QCOMPARE(line.section(QLatin1Char(','), 6, 6), QStringLiteral("geo_awareness"));
    QCOMPARE(line.section(QLatin1Char(','), 8, 9), QStringLiteral("unavailable,GNSS lost (no fix)"));
    QCOMPARE(GeoZoneAlertHistory::parseLogLine(line)->message, lost);
    QVERIFY(shownAlert(monitor, event.id) && !shownAlert(monitor, event.id)->question);
    step(10000);
    QCOMPARE(awarenessAlerts().count(), 1);

    // Recovering is an advisory
    gps = gpsMessage(vehicleId, GPS_FIX_TYPE_3D_FIX, 12, 0.8, 1.5);
    step(1000);
    step(Awareness::Tracker::betterSettleMSecs);
    QCOMPARE(awarenessAlerts().last(), QStringLiteral("Geo-awareness available again"));
    QCOMPARE(alertSpy.last()[1].toInt(), GeoZoneAlertLevel::severity(GeoZoneAlertLevel::Advisory, 0));

    // A refresh of the feed which fails degrades it, a caution
    server.okPathStatus = "503 Service Unavailable";
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(!feed->errorString().isEmpty());
    step(1000);
    step(Awareness::Tracker::worseSettleMSecs);
    QCOMPARE(awarenessAlerts().last(), QStringLiteral("Geo-awareness degraded: update of %1 failed").arg(feed->name()));
    QCOMPARE(alertSpy.last()[1].toInt(), GeoZoneAlertLevel::severity(GeoZoneAlertLevel::Caution, 0));
    QCOMPARE(monitor.awareness()->stateValue(), static_cast<int>(Awareness::Degraded));

    // On the ground nothing is alerted, the state is still published
    vehicle->setArmed(false);
    const qsizetype count = awarenessAlerts().count();
    monitor.positionLost(vehicleId);
    for (int i = 0; i < 3; i++) {
        now += 5000;
        vehicle->handleMessage(gps, now);
        monitor.evaluateVehicles();
    }
    QCOMPARE(monitor.awareness()->stateValue(), static_cast<int>(Awareness::Unavailable));
    QVERIFY(monitor.awareness()->reasons().contains(QStringLiteral("no current vehicle position")));
    QCOMPARE(awarenessAlerts().count(), count);
}


void GeoZoneMonitorTest::_testPermits()
{
    ScopedEnvironment environment;
    // A zone requiring an authorisation, one requiring a notification and a prohibited zone
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate notificationCenter = center.atDistanceAndAzimuth(3000, 90);
    const QGeoCoordinate prohibitedCenter = center.atDistanceAndAzimuth(3000, 270);
    QJsonObject notification = _circleFeature(QStringLiteral("N"), QStringLiteral("CONDITIONAL"), notificationCenter, 300);
    notification[QStringLiteral("zoneAuthority")] = QJsonArray{ QJsonObject{ { "name", "Tower" }, { "purpose", "NOTIFICATION" }, { "intervalBefore", "P1D" } } };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center, 300),
        notification,
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), prohibitedCenter, 300),
    }), errorString), qPrintable(errorString));
    const auto zone = [&manager](const QString &identifier) -> const GeoZone* {
        for (const GeoZone *candidate : manager.zoneList()) {
            if (candidate->identifier() == identifier) {
                return candidate;
            }
        }
        return nullptr;
    };
    QVERIFY(zone(QStringLiteral("N")) && (zone(QStringLiteral("N"))->data().notificationLeadSecs >= 0));
    QVERIFY(GeoZonePermits::applies(zone(QStringLiteral("A"))));
    QVERIFY(GeoZonePermits::applies(zone(QStringLiteral("N"))));
    QVERIFY(!GeoZonePermits::applies(zone(QStringLiteral("P"))));

    GeoZoneMonitor monitor(&manager);
    qint64 now = 0;
    monitor.setClock([&now]() { return now; });
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);
    GeoZoneAlertHistory &log = *monitor.alertHistory();
    log.startLog(dir.filePath(QStringLiteral("logs")));
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    GeoZonePermits *const permits = monitor.permits();

    // Acknowledged before the flight in the zone details, without vehicle, a permit stops the approach and entry
    // alerts: the approach is logged only, the entry shown as an advisory, neither spoken
    constexpr int vehicleId = 1;
    monitor.setActiveVehicleId(vehicleId);
    QVERIFY(permits->acknowledge(QVariant::fromValue(GeoZoneInfo(zone(QStringLiteral("A")))), GeoZonePermits::ZoneDetails));
    QVERIFY(!permits->acknowledge(zone(QStringLiteral("A")), GeoZonePermits::Checklist, vehicleId));
    QVERIFY(!permits->acknowledge(zone(QStringLiteral("P")), GeoZonePermits::Checklist, vehicleId));
    QCOMPARE(permits->count(), 1);
    QCOMPARE(permits->permits().first().vehicleId, GeoZoneMonitor::noVehicle);
    QVERIFY(permits->permitted(QVariant::fromValue(GeoZoneInfo(zone(QStringLiteral("A"))))));
    monitor.evaluate({ center.atDistanceAndAzimuth(10000, 0), 450, 400 }, vehicleId);
    GeoZoneVehicleMonitor *const vehicle = monitor.positionMonitor(vehicleId);
    vehicle->setArmed(false);
    vehicle->setArmed(true);
    monitor.evaluate({ center.atDistanceAndAzimuth(380, 0), 450, 400 }, vehicleId);
    QCOMPARE(alertSpy.count(), 0);
    monitor.evaluate({ center, 450, 400 }, vehicleId);
    QCOMPARE(alertSpy.count(), 1);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Entered geozone A (Authorisation required) \u2014 permit acknowledged"));
    QCOMPARE(alertSpy.last()[1].toInt(), GeoZoneAlertLevel::severity(GeoZoneAlertLevel::Advisory, GeoZone::restrictionSeverity(GeoZone::ReqAuthorisation)));
    QVERIFY(!shownAlert(monitor, monitor.alertModel()->alerts().first().alertId)->question);
    QVERIFY(sink->spoken.isEmpty());
    QCOMPARE(log.unacknowledgedCount(), 0);

    // In flight, "I do" for a zone requiring a notification is its permit, the prohibited zone keeps the approval
    monitor.evaluate({ notificationCenter, 450, 400 }, vehicleId);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Entered geozone N (Conditional)"));
    const int notificationAlert = alertSpy.last()[2].toInt();
    QVERIFY(shownAlert(monitor, notificationAlert)->question);
    monitor.approveAlert(notificationAlert);
    QCOMPARE(permits->count(), 2);
    QCOMPARE(permits->permits().last().origin, GeoZonePermits::AlertPopup);
    QCOMPARE(permits->permits().last().vehicleId, vehicleId);
    QVERIFY(monitor.approvedZoneKeys().isEmpty());
    monitor.evaluate({ prohibitedCenter, 450, 400 }, vehicleId);
    monitor.approveAlert(monitor.alertModel()->alerts().first().alertId);
    QCOMPARE(permits->count(), 2);
    QCOMPARE(monitor.approvedZoneKeys(), QStringList{ zone(QStringLiteral("P"))->key() });
    // Coming back to the zone needs no new answer
    const qsizetype count = alertSpy.count();
    monitor.evaluate({ notificationCenter.atDistanceAndAzimuth(380, 0), 450, 400 }, vehicleId);
    monitor.evaluate({ notificationCenter, 450, 400 }, vehicleId);
    QCOMPARE(alertSpy.count(), count + 1);
    QVERIFY(alertSpy.last()[0].toString().endsWith(QStringLiteral("permit acknowledged")));

    // Withdrawn, the zone the vehicle is in alerts again
    permits->withdraw(GeoZonePermits::key(zone(QStringLiteral("N"))));
    QCOMPARE(permits->count(), 1);
    monitor.evaluate({ notificationCenter, 450, 400 }, vehicleId);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Entered geozone N (Conditional)"));
    QVERIFY(shownAlert(monitor, alertSpy.last()[2].toInt())->question);

    // The flight ends when the vehicle disarms after having been armed, which ends the permits
    monitor.evaluate({ center, 450, 400 }, vehicleId);
    vehicle->setArmed(false);
    QCOMPARE(permits->count(), 0);
    QCOMPARE(vehicle->groundWarning(), QStringLiteral("On the ground inside geozone A (Authorisation required) \u2014 take-off only with an authorisation"));
    QCOMPARE(vehicle->groundLevel(), static_cast<int>(GeoZoneAlertLevel::Caution));

    // With a permit for the next flight, from the checklist, take-off is allowed
    QVERIFY(permits->acknowledge(zone(QStringLiteral("A")), GeoZonePermits::Checklist, vehicleId));
    QCOMPARE(vehicle->groundWarning(), QStringLiteral("On the ground inside geozone A (Authorisation required) \u2014 permit acknowledged"));
    QCOMPARE(vehicle->groundLevel(), static_cast<int>(GeoZoneAlertLevel::Advisory));
    vehicle->setArmed(true);
    QCOMPARE(permits->count(), 1);

    // A permit ends when its vehicle disconnects
    Vehicle offline(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    QVERIFY(permits->acknowledge(zone(QStringLiteral("N")), GeoZonePermits::Checklist, offline.id()));
    monitor.addVehicle(&offline);
    monitor.removeVehicle(&offline);
    QCOMPARE(permits->count(), 1);

    // Logged with the time, vehicle, zone and where they were acknowledged, or why they ended
    log.flushLog();
    QFile file(log.logFilePath(environment.now().date()));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(file.fileName()));
    QStringList rows;
    for (const QString &line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList fields = GeoZoneAlertHistory::logFields(line);
        if (fields[6].startsWith(QStringLiteral("permit"))) {
            QCOMPARE(QDateTime::fromString(fields[0], Qt::ISODateWithMs), environment.now());
            rows.append(QStringList{ fields[1], fields[6], fields[8], fields[15] }.join(QLatin1Char(' ')).trimmed());
        }
    }
    QCOMPARE(rows, (QStringList{
        QStringLiteral("permit A zone details"),
        QStringLiteral("1 permit N alert popup"),
        QStringLiteral("1 permit_ended N withdrawn"),
        QStringLiteral("1 permit_ended A flight ended"),
        QStringLiteral("1 permit A pre-flight checklist"),
        QStringLiteral("%1 permit N pre-flight checklist").arg(offline.id()),
        QStringLiteral("%1 permit_ended N vehicle disconnected").arg(offline.id()),
    }));
    // The entry with the permit is logged as such
    QCOMPARE(GeoZoneAlertHistory::parseLogLine(QStringLiteral("2026-06-15T10:00:00.000Z,1,47,8,450,400,entered,0,A,,ReqAuthorisation,,,,5,permit"))->message,
             QStringLiteral("Entered geozone A (Authorisation required) \u2014 permit acknowledged"));
}

GEOZONE_TEST(GeoZoneMonitorTest)

void GeoZoneMonitorTest::_testAlertLevels()
{
    // The levels of EN 4709-003 Annex D: warnings inside restricted zones or entering them imminently, cautions near
    // them and in U-space airspace, advisories for conditional and approved zones
    using Level = GeoZoneAlertLevel::Level;
    QCOMPARE(GeoZoneMonitor::zoneLevel(GeoZone::Prohibited, true, false), GeoZoneAlertLevel::Warning);
    QCOMPARE(GeoZoneMonitor::zoneLevel(GeoZone::ReqAuthorisation, false, true), GeoZoneAlertLevel::Warning);
    QCOMPARE(GeoZoneMonitor::zoneLevel(GeoZone::Unknown, true, false), GeoZoneAlertLevel::Warning);
    QCOMPARE(GeoZoneMonitor::zoneLevel(GeoZone::Prohibited, false, false), GeoZoneAlertLevel::Caution);
    QCOMPARE(GeoZoneMonitor::zoneLevel(GeoZone::USpace, true, false), GeoZoneAlertLevel::Caution);
    QCOMPARE(GeoZoneMonitor::zoneLevel(GeoZone::Conditional, true, true), GeoZoneAlertLevel::Advisory);
    QCOMPARE(GeoZoneMonitor::zoneLevel(GeoZone::Prohibited, true, false, true), GeoZoneAlertLevel::Advisory);
    QCOMPARE(GeoZoneAlertLevel::text(GeoZoneAlertLevel::Warning), QStringLiteral("WARNING"));
    QCOMPARE(GeoZoneAlertLevel::fromCode(QStringLiteral("advisory")), GeoZoneAlertLevel::Advisory);

    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("C"), QStringLiteral("CONDITIONAL"), center.atDistanceAndAzimuth(5000, 90), 300),
    }), errorString), qPrintable(errorString));

    // At 15 m/s towards the prohibited zone with the thresholds of the default profile: a caution ahead, a warning once
    // the entry is within the reaction time and the evaluation interval, and a warning when it enters
    GeoZoneMonitor monitor(&manager);
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
    QSignalSpy levelSpy(&monitor, &GeoZoneMonitor::activeLevelChanged);
    const GeoZoneMonitor::Velocity north{ 15, 0, 0, true };
    QList<Level> levels;
    QList<int> activeLevels;
    for (int tick = 0; tick < 80; tick++) {
        const qsizetype events = eventSpy.count();
        monitor.evaluate({ center.atDistanceAndAzimuth(1300 - (15.0 * tick), 180) }, GeoZoneMonitor::noVehicle, north);
        for (qsizetype i = events; i < eventSpy.count(); i++) {
            const GeoZoneMonitor::AlertEvent event = eventSpy[i].first().value<GeoZoneMonitor::AlertEvent>();
            if (!event.pilotEvent()) {
                levels.append(event.level);
                activeLevels.append(monitor.activeLevel());
            }
        }
        if (monitor.activeMonitor()->inside()) {
            break;
        }
    }
    QCOMPARE(levels, (QList<Level>{ GeoZoneAlertLevel::Caution, GeoZoneAlertLevel::Warning, GeoZoneAlertLevel::Warning }));
    QCOMPARE(activeLevels, (QList<int>{ GeoZoneAlertLevel::Caution, GeoZoneAlertLevel::Warning, GeoZoneAlertLevel::Warning }));
    QVERIFY(levelSpy.count() >= 2);
    const GeoZoneMonitor::AlertEvent imminent = eventSpy[eventSpy.count() - 2].first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY(imminent.kind == GeoZoneMonitor::AlertEvent::Approach);
    QVERIFY2(imminent.timeToEntry <= 6, qPrintable(imminent.message));
    QVERIFY(imminent.message.startsWith(QStringLiteral("Approaching geozone P (Prohibited), entry in about")));
    QVERIFY2(sink->spoken.constFirst().startsWith(QStringLiteral("Caution: Approaching geozone P")), qPrintable(sink->spoken.constFirst()));
    QVERIFY2(sink->spoken.constLast().startsWith(QStringLiteral("Warning: ")), qPrintable(sink->spoken.constLast()));

    // The popup shows the levels, framed by the highest, which replace each other for the zone
    GeoZoneAlertModel *const shown = monitor.alertModel();
    QCOMPARE(shown->count(), 1);
    QCOMPARE(shown->level(), static_cast<int>(GeoZoneAlertLevel::Warning));
    QCOMPARE(shown->data(shown->index(0), GeoZoneAlertModel::LevelTextRole).toString(), QStringLiteral("WARNING"));
    QCOMPARE(monitor.alertHistory()->data(monitor.alertHistory()->index(0), GeoZoneAlertHistory::LevelRole).toInt(), static_cast<int>(GeoZoneAlertLevel::Warning));

    // Approved, the zone is an advisory: its alerts are logged as such
    monitor.approveAlert(shown->alerts().constFirst().alertId);
    QCOMPARE(monitor.activeLevel(), static_cast<int>(GeoZoneAlertLevel::Advisory));
    QCOMPARE(shown->level(), -1);
    monitor.removePositionMonitor(GeoZoneMonitor::noVehicle);
    QCOMPARE(monitor.activeLevel(), -1);

    // Conditional zones are advisories, the vehicle's fence breaches warnings
    eventSpy.clear();
    monitor.evaluate({ center.atDistanceAndAzimuth(5000, 90) });
    QCOMPARE(eventSpy.first().first().value<GeoZoneMonitor::AlertEvent>().level, GeoZoneAlertLevel::Advisory);
    QCOMPARE(monitor.activeLevel(), static_cast<int>(GeoZoneAlertLevel::Advisory));
    QCOMPARE(shown->level(), static_cast<int>(GeoZoneAlertLevel::Advisory));
    emit monitor.fenceBreach(QStringLiteral("Boundary fence breached"));
    QCOMPARE(shown->level(), static_cast<int>(GeoZoneAlertLevel::Warning));
    QCOMPARE(shown->alerts().constFirst().message, QStringLiteral("Boundary fence breached"));

    // Vertically alike: climbing at 3 m/s into a prohibited zone from 300 m AMSL above the vehicle is cautioned, then
    // warned imminent and entered, each spoken
    const QGeoCoordinate above = center.atDistanceAndAzimuth(20000, 0);
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("above.json"), QJsonArray{
        _withLimits(_circleFeature(QStringLiteral("HIGH"), QStringLiteral("PROHIBITED"), above, 1000), 300, QStringLiteral("AMSL"), 2000, QStringLiteral("AMSL")),
    }), errorString), qPrintable(errorString));
    GeoZoneMonitor climbing(&manager);
    const auto climbSink = std::make_shared<RecordingAudioSink>();
    climbing.setAudioSink(climbSink);
    qint64 now = 0;
    climbing.setClock([&now]() { return now; });
    QSignalSpy climbSpy(&climbing, &GeoZoneMonitor::alertEvent);
    for (int tick = 0; (tick < 100) && !(climbing.activeMonitor() && climbing.activeMonitor()->inside()); tick++) {
        climbing.evaluate({ above, 100 + (3.0 * tick), 50 }, GeoZoneMonitor::noVehicle, GeoZoneMonitor::Velocity{ 0, 0, 3, true });
        now += 1000;
    }
    QStringList climbSpoken;
    for (const QString &text : std::as_const(climbSink->spoken)) {
        climbSpoken.append(text.section(QLatin1Char(','), 0, 0));
    }
    QCOMPARE(climbSpoken, (QStringList{ QStringLiteral("Caution: Approaching geozone HIGH (Prohibited)"), QStringLiteral("Warning: Approaching geozone HIGH (Prohibited)"),
                                        QStringLiteral("Warning: Entered geozone HIGH (Prohibited)") }));
    const GeoZoneMonitor::AlertEvent climbCaution = climbSpy.first().first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY2(climbCaution.timeToEntry >= 5 + (3 / 2.0) + 1, qPrintable(climbCaution.message));
}

void GeoZoneMonitorTest::_testZoneActivation()
{
    // A prohibited zone from 12:00 to 13:00 UTC, and one which is always active, cautioned 1 min before
    ScopedEnvironment environment;
    const QDate day(2027, 3, 1);
    environment.setNow(_utc(day, QTime(11, 58, 30)));
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    const auto restoreDefault = qScopeGuard([settings]() { settings->activationCaution()->setRawValue(settings->activationCaution()->rawDefaultValue()); });
    settings->activationCaution()->setRawValue(1);
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate always = center.atDistanceAndAzimuth(5000, 90);
    QJsonObject scheduled = _circleFeature(QStringLiteral("S"), QStringLiteral("PROHIBITED"), center, 300);
    scheduled[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-03-01T12:00:00Z" }, { "endDateTime", "2027-03-01T13:00:00Z" } } };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ scheduled, _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), always, 300) });
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));
    const auto scheduledActive = [&manager]() -> std::optional<bool> {
        for (const GeoZone *zone : manager.zoneList()) {
            if (zone->identifier() == QStringLiteral("S")) {
                return zone->active();
            }
        }
        return std::nullopt;
    };
    QCOMPARE(scheduledActive(), std::optional<bool>(false));
    QSignalSpy activeSpy(&manager, &GeoZoneManager::activeZonesChanged);

    GeoZoneMonitor monitor(&manager);
    monitor.setActiveVehicleId(1);
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
    const GeoZone::Position inside{ center.atDistanceAndAzimuth(100, 0), 500, 450 };

    // Hovering inside the zone before it applies, nothing is alerted
    monitor.evaluate(inside, 1);
    monitor.evaluate(inside, 1);
    QVERIFY(eventSpy.isEmpty());

    // A second before it applies, still nothing
    environment.setNow(_utc(day, QTime(11, 59, 59)));
    monitor.evaluate(inside, 1);
    QCOMPARE(eventSpy.count(), 1);
    QCOMPARE(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().kind, GeoZoneMonitor::AlertEvent::ActivationSoon);
    QCOMPARE(scheduledActive(), std::optional<bool>(false));
    QVERIFY(activeSpy.isEmpty());
    eventSpy.clear();

    // Once it applies, a warning tells the pilot to leave or land, without a manoeuvre from QGroundControl: at the
    // evaluation after its start, which doesn't wait for the manager to look at the schedules every minute. The map
    // shows the zone as active from then too.
    environment.setNow(_utc(day, QTime(12, 0, 0)));
    monitor.evaluate(inside, 1);
    QCOMPARE(scheduledActive(), std::optional<bool>(true));
    QCOMPARE(activeSpy.count(), 1);
    QCOMPARE(eventSpy.count(), 1);
    GeoZoneMonitor::AlertEvent event = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY(event.kind == GeoZoneMonitor::AlertEvent::Activated);
    QVERIFY(event.inside);
    QCOMPARE(event.level, GeoZoneAlertLevel::Warning);
    const QString message = QStringLiteral("Geozone S (Prohibited) is now active \u2014 you are inside it. Leave or land as soon as possible.");
    QCOMPARE(event.message, message);
    QCOMPARE(sink->spoken.last(), QStringLiteral("Warning: ") + message);
    QCOMPARE(monitor.activeLevel(), static_cast<int>(GeoZoneAlertLevel::Warning));
    const std::optional<GeoZoneAlertModel::Alert> shown = shownAlert(monitor, event.id);
    QVERIFY(shown && shown->question && shown->inside);
    // Logged as such, and repeated like an entry into a prohibited zone
    const std::optional<GeoZoneMonitor::AlertEvent> logged = GeoZoneAlertHistory::parseLogLine(GeoZoneAlertHistory::logLine(event));
    QVERIFY(logged && (logged->kind == GeoZoneMonitor::AlertEvent::Activated) && logged->inside);
    QCOMPARE(logged->message, message);
    QCOMPARE(monitor.arbiter().breachCount(), 1);

    // Leaving it ends the warning, flying back in is an entry
    monitor.evaluate({ center.atDistanceAndAzimuth(2000, 0), 500, 450 }, 1);
    QCOMPARE(monitor.arbiter().breachCount(), 0);
    monitor.evaluate({ center.atDistanceAndAzimuth(400, 0), 500, 450 }, 1);
    monitor.evaluate(inside, 1);
    QVERIFY(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().kind == GeoZoneMonitor::AlertEvent::Entry);

    // Starting inside an active zone, like taking off in it, is an entry too
    eventSpy.clear();
    monitor.evaluate({ always, 500, 450 }, 2);
    QCOMPARE(eventSpy.count(), 1);
    QVERIFY(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().kind == GeoZoneMonitor::AlertEvent::Entry);

    // A zone loaded around a vehicle in flight is active around it, so it is too
    eventSpy.clear();
    const GeoZone::Position elsewhere{ center.atDistanceAndAzimuth(9000, 180), 500, 450 };
    monitor.evaluate(elsewhere, 3);
    QVERIFY(eventSpy.isEmpty());
    (void) _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ scheduled, _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), always, 300),
                                                                         _circleFeature(QStringLiteral("NEW"), QStringLiteral("REQ_AUTHORISATION"), elsewhere.coordinate, 300) });
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));
    monitor.evaluate(elsewhere, 3);
    QCOMPARE(eventSpy.count(), 1);
    event = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY(event.kind == GeoZoneMonitor::AlertEvent::Activated);
    QCOMPARE(event.message, QStringLiteral("Vehicle 3: Geozone NEW (Authorisation required) is now active \u2014 you are inside it. Leave or land as soon as possible."));
    QCOMPARE(event.level, GeoZoneAlertLevel::Warning);

    // The zone ends: at the next evaluation the vehicle inside it is no longer in a zone
    monitor.evaluate(inside, 1);
    const GeoZoneVehicleMonitor *const first = monitor.positionMonitor(1);
    QVERIFY(first);
    QVERIFY(first->inside());
    activeSpy.clear();
    environment.setNow(_utc(day, QTime(13, 0, 1)));
    monitor.evaluate(inside, 1);
    QCOMPARE(scheduledActive(), std::optional<bool>(false));
    QCOMPARE(activeSpy.count(), 1);
    QVERIFY(!first->inside());
    QVERIFY(first->nearby().isEmpty());

    // Zones away from the vehicles keep the state the manager gave them
    environment.setNow(_utc(day, QTime(12, 30)));
    monitor.evaluate(elsewhere, 3);
    QCOMPARE(scheduledActive(), std::optional<bool>(false));
    QVERIFY(manager.updateActive(GeoZoneBox(QGeoRectangle(center.atDistanceAndAzimuth(100, 315), center.atDistanceAndAzimuth(100, 135)))));
    QCOMPARE(scheduledActive(), std::optional<bool>(true));
    QVERIFY(!manager.updateActive(GeoZoneBox(QGeoRectangle(center.atDistanceAndAzimuth(100, 315), center.atDistanceAndAzimuth(100, 135)))));
}

void GeoZoneMonitorTest::_testActivationCaution()
{
    // A prohibited zone from 12:00 to 13:00 and from 14:00 to 15:00 UTC
    ScopedEnvironment environment;
    const QDate day(2027, 3, 1);
    environment.setNow(_utc(day, QTime(11, 40)));
    const QGeoCoordinate center(47, 8);
    QJsonObject scheduled = _circleFeature(QStringLiteral("S"), QStringLiteral("PROHIBITED"), center, 300);
    scheduled[QStringLiteral("applicability")] = QJsonArray{
        QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-03-01T12:00:00Z" }, { "endDateTime", "2027-03-01T13:00:00Z" } },
        QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-03-01T14:00:00Z" }, { "endDateTime", "2027-03-01T15:00:00Z" } },
    };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ scheduled }), errorString), qPrintable(errorString));
    const GeoZone *const zone = manager.zoneList().constFirst();
    // The evaluations follow the schedule themselves
    const auto at = [&environment](const QTime &time) { environment.setNow(_utc(QDate(2027, 3, 1), time)); };

    // The activation to the second
    QCOMPARE(GeoZoneActivationTracker::nextActivation(zone, _utc(day, QTime(11, 51, 17)), 600), _utc(day, QTime(12, 0)));
    QVERIFY(!GeoZoneActivationTracker::nextActivation(zone, _utc(day, QTime(11, 40)), 600).isValid());
    QVERIFY(!GeoZoneActivationTracker::nextActivation(zone, _utc(day, QTime(12, 10)), 600).isValid());

    GeoZoneMonitor monitor(&manager);
    const auto sink = std::make_shared<RecordingAudioSink>();
    monitor.setAudioSink(sink);
    QSignalSpy eventSpy(&monitor, &GeoZoneMonitor::alertEvent);
    const GeoZone::Position inside{ center.atDistanceAndAzimuth(100, 0), 500, 450 };
    const GeoZone::Position near{ center.atDistanceAndAzimuth(350, 0), 500, 450 };
    const GeoZone::Position far{ center.atDistanceAndAzimuth(2000, 0), 500, 450 };

    // More than the 10 min of the setting before, nothing
    monitor.evaluate(inside, 1);
    QVERIFY(eventSpy.isEmpty());

    // Within them, a caution with the time left, once for the activation
    at(QTime(11, 51, 30));
    monitor.evaluate(inside, 1);
    QCOMPARE(eventSpy.count(), 1);
    GeoZoneMonitor::AlertEvent event = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY(event.kind == GeoZoneMonitor::AlertEvent::ActivationSoon);
    QCOMPARE(event.level, GeoZoneAlertLevel::Caution);
    QCOMPARE(event.timeToEntry, 510.0);
    QCOMPARE(event.message, QStringLiteral("Geozone S (Prohibited) becomes active in 9 min, you are inside it"));
    QCOMPARE(sink->spoken.last(), QStringLiteral("Caution: ") + event.message);
    QVERIFY(shownAlert(monitor, event.id));
    const std::optional<GeoZoneMonitor::AlertEvent> logged = GeoZoneAlertHistory::parseLogLine(GeoZoneAlertHistory::logLine(event));
    QVERIFY(logged && (logged->kind == GeoZoneMonitor::AlertEvent::ActivationSoon) && logged->inside);
    QCOMPARE(logged->message, event.message);
    QCOMPARE(GeoZoneAlertHistory::logFields(GeoZoneAlertHistory::logLine(event))[6], QStringLiteral("activation_soon"));
    at(QTime(11, 55));
    monitor.evaluate(inside, 1);
    monitor.evaluate(far, 1);
    monitor.evaluate(inside, 1);
    QCOMPARE(eventSpy.count(), 1);

    // Then the warning that it is active
    at(QTime(12, 0, 1));
    monitor.evaluate(inside, 1);
    QCOMPARE(eventSpy.count(), 2);
    QVERIFY(eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>().kind == GeoZoneMonitor::AlertEvent::Activated);

    // The next activation cautions again, also a vehicle within the approach distance, but not one farther
    at(QTime(13, 0, 30));
    monitor.evaluate(near, 1);
    monitor.evaluate(far, 2);
    at(QTime(13, 59, 50));
    eventSpy.clear();
    monitor.evaluate(near, 1);
    monitor.evaluate(far, 2);
    QCOMPARE(eventSpy.count(), 1);
    event = eventSpy.last().first().value<GeoZoneMonitor::AlertEvent>();
    QVERIFY(event.kind == GeoZoneMonitor::AlertEvent::ActivationSoon);
    QVERIFY(!event.inside);
    QVERIFY(_near(event.distance, 50, 0.01));
    QCOMPARE(event.message, QStringLiteral("Vehicle 1: Geozone S (Prohibited) nearby becomes active in 10 s"));

    // The pilot sets how long before
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    const auto restoreDefault = qScopeGuard([settings]() { settings->activationCaution()->setRawValue(settings->activationCaution()->rawDefaultValue()); });
    QCOMPARE(monitor.settings().activationCautionSecs, 600);
    settings->activationCaution()->setRawValue(30);
    QCOMPARE(monitor.settings().activationCautionSecs, 1800);
}
