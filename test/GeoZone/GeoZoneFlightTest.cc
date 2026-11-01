/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFlightTest.h"
#include "GeoZone.h"
#include "GeoZoneAlertLog.h"
#include "GeoZoneGuidedCheck.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMonitor.h"
#include "GeoZoneSettings.h"
#include "MissionController.h"
#include "MultiVehicleManager.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "SimpleMissionItem.h"
#include "Vehicle.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <algorithm>
#include <cmath>

namespace {

// Zones of the Swiss geozones published by FOCA in a region of Zurich, see the description in the file
const QString kZonesFile = QStringLiteral(":/unittest/ch_zurich_ed269.json");

// MockLink vehicle position, inside CTR ZURICH and CTR DUEBENDORF which start 120 m above ground
const QGeoCoordinate kOrigin(47.397, 8.5455);

constexpr double kTerrainAMSL = 430;

// Meters east and north of kOrigin, height above the terrain
struct Waypoint {
    double east;
    double north;
    double height;
};

// West of Gefängnis Zürich West, a prison zone up to 150 m above ground
constexpr Waypoint kStart{ -3500, -1736, 60 };
// Center of Gefängnis Zürich West. Gefängnis Zürich is 565 m south of the legs.
constexpr Waypoint kPrison{ -1937, -1736, 60 };
// 1.7 km into LSZH Zürich, the airport zone from the ground. Its boundary is 1288 m north on the leg from kPrison.
constexpr Waypoint kAirport{ -1937, 3000, 60 };

constexpr double kCruiseHeight = 250;   ///< Inside the CTRs, above Gefängnis Zürich West

QGeoCoordinate coordinate(double east, double north)
{
    return kOrigin.atDistanceAndAzimuth(std::hypot(east, north), qRadiansToDegrees(std::atan2(east, north)));
}

GeoZone::Position position(double east, double north, double height)
{
    return GeoZone::Position{ coordinate(east, north), kTerrainAMSL + height, kTerrainAMSL };
}

/// Evaluates positions every 10 m horizontally and 5 m vertically, like vehicle updates
void fly(GeoZoneMonitor &monitor, const Waypoint &from, const Waypoint &to)
{
    const double horizontal = std::hypot(to.east - from.east, to.north - from.north);
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max(horizontal / 10, std::abs(to.height - from.height) / 5))));
    for (int i = 1; i <= steps; i++) {
        const double fraction = static_cast<double>(i) / steps;
        monitor.evaluate(position(from.east + ((to.east - from.east) * fraction), from.north + ((to.north - from.north) * fraction),
                                  from.height + ((to.height - from.height) * fraction)));
    }
}

const GeoZone *findZone(const GeoZoneManager &manager, const QString &identifier)
{
    for (const GeoZone *zone : manager.zoneList()) {
        if (zone->identifier() == identifier) {
            return zone;
        }
    }
    return nullptr;
}

QString alertText(const GeoZoneManager &manager, const QString &event, const QString &identifier)
{
    const GeoZone *const zone = findZone(manager, identifier);
    return zone ? QStringLiteral("%1 geozone %2 (%3)").arg(event, zone->name(), zone->restrictionText()) : identifier;
}

QStringList identifiers(const QList<GeoZone*> &zones)
{
    QStringList list;
    for (const GeoZone *zone : zones) {
        list.append(zone->identifier());
    }
    list.sort();
    return list;
}

QStringList nearbyIdentifiers(const GeoZoneMonitor &monitor)
{
    QList<GeoZone*> zones;
    for (const QVariant &nearby : monitor.nearbyZones()) {
        zones.append(const_cast<GeoZone*>(GeoZoneInfo::fromVariant(nearby.toMap().value(QStringLiteral("zone")))));
    }
    return identifiers(zones);
}

QStringList spiedAlerts(const QSignalSpy &spy)
{
    QStringList alerts;
    for (const QList<QVariant> &arguments : spy) {
        alerts.append(arguments.constFirst().toString());
    }
    return alerts;
}

void restoreSettingDefaults()
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    for (Fact *const fact : { settings->alertsEnabled(), settings->audioAlerts(), settings->logAlerts() }) {
        fact->setRawValue(fact->rawDefaultValue());
    }
}

} // namespace

void GeoZoneFlightTest::cleanup()
{
    GeoZoneManager::instance()->clear();
    restoreSettingDefaults();
    UnitTest::cleanup();
}

void GeoZoneFlightTest::_testFlightAlertSequence()
{
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.path());
    QString errorString;
    QVERIFY2(manager.loadFile(kZonesFile, errorString), qPrintable(errorString));
    QCOMPARE(manager.zoneCount(), 8);

    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);

    // Low below the CTRs into the prison zone, climbing out of it into CTR ZURICH, to the airport and descending
    // out of the CTR there. Each zone alerts once on approach and once on entry.
    fly(monitor, kStart, kPrison);
    QCOMPARE(nearbyIdentifiers(monitor), QStringList{ QStringLiteral("GZW0001") });
    QVERIFY(monitor.inside());
    fly(monitor, kPrison, { kPrison.east, kPrison.north, kCruiseHeight });
    QCOMPARE(nearbyIdentifiers(monitor), QStringList{ QStringLiteral("CTR0015") });
    fly(monitor, { kPrison.east, kPrison.north, kCruiseHeight }, { kAirport.east, kAirport.north, kCruiseHeight });
    fly(monitor, { kAirport.east, kAirport.north, kCruiseHeight }, kAirport);

    const QStringList expected = {
        alertText(manager, QStringLiteral("Approaching"), QStringLiteral("GZW0001")),
        alertText(manager, QStringLiteral("Entered"), QStringLiteral("GZW0001")),
        alertText(manager, QStringLiteral("Approaching"), QStringLiteral("CTR0015")),
        alertText(manager, QStringLiteral("Entered"), QStringLiteral("CTR0015")),
        alertText(manager, QStringLiteral("Approaching"), QStringLiteral("LSZH001")),
        alertText(manager, QStringLiteral("Entered"), QStringLiteral("LSZH001")),
    };
    QCOMPARE(spiedAlerts(alertSpy), expected);
    QCOMPARE(nearbyIdentifiers(monitor), QStringList{ QStringLiteral("LSZH001") });
    QVERIFY(monitor.inside());

    // Without terrain data the height above ground is unknown, so the CTRs above count as entered
    alertSpy.clear();
    monitor.evaluate(GeoZone::Position{ coordinate(kAirport.east, kAirport.north), kTerrainAMSL + kAirport.height });
    QCOMPARE(spiedAlerts(alertSpy), QStringList{ alertText(manager, QStringLiteral("Entered"), QStringLiteral("CTR0015")) });
}

void GeoZoneFlightTest::_testMissionCrossedZones()
{
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.path());
    QString errorString;
    QVERIFY2(manager.loadFile(kZonesFile, errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    GeoZoneMissionCheck check(&manager);
    check.setMissionController(missionController);

    // The simulated flight as a mission. Whether or not terrain heights are available, it enters the prison zone
    // low, CTR ZURICH high and the airport zone, and stays far from the other zones.
    const QList<Waypoint> waypoints = {
        kStart,
        kPrison,
        { kPrison.east, kPrison.north, kCruiseHeight },
        { kAirport.east, kAirport.north, kCruiseHeight },
        kAirport,
    };
    for (int i = 0; i < waypoints.count(); i++) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(
            missionController->insertSimpleMissionItem(coordinate(waypoints[i].east, waypoints[i].north), i + 1));
        QVERIFY(item);
        item->altitude()->setRawValue(waypoints[i].height);
    }

    const QStringList expected = { QStringLiteral("CTR0015"), QStringLiteral("GZW0001"), QStringLiteral("LSZH001") };
    QTRY_COMPARE_WITH_TIMEOUT(identifiers(check.crossedZones()), expected, 20000);
    QVERIFY(check.warningText().contains(findZone(manager, QStringLiteral("GZW0001"))->name()));
}

void GeoZoneFlightTest::_testMockLinkVehicle()
{
    GeoZoneManager *const manager = GeoZoneManager::instance();
    QString errorString;
    QVERIFY2(manager->loadFile(kZonesFile, errorString), qPrintable(errorString));
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    settings->audioAlerts()->setRawValue(false);

    // Monitors the vehicles like the application
    GeoZoneMonitor monitor;
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    const QTemporaryDir logDir;
    QVERIFY(logDir.isValid() && monitor.alertLog());
    monitor.alertLog()->setDirectory(logDir.path());
    QVERIFY(!monitor.monitoring());

    const QDate startDate = QDateTime::currentDateTimeUtc().date();
    _connectMockLink(MAV_AUTOPILOT_PX4);
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QTRY_VERIFY(monitor.monitoring());

    // A parked vehicle is on the ground below the CTRs, even before terrain data is received
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->coordinate().isValid() && !std::isnan(vehicle->altitudeAMSL()->rawValue().toDouble()), 30000);
    QVERIFY(!vehicle->armed());
    QTest::qWait(1500);
    QVERIFY(!monitor.inside());
    QVERIFY(spiedAlerts(alertSpy).filter(QStringLiteral("Entered")).isEmpty());

    // Climbing 300 m enters the CTRs, MockLink reports the arming with its next heartbeat
    vehicle->sendMavCommand(vehicle->defaultComponentId(), MAV_CMD_NAV_TAKEOFF, false /* showError */, 0, 0, 0, 0, 0, 0, 300);
    QTRY_COMPARE_WITH_TIMEOUT(vehicle->altitudeRelative()->rawValue().toInt(), 300, 30000);
    const QStringList ctrs = { QStringLiteral("CTR0004"), QStringLiteral("CTR0015") };
    QTRY_VERIFY_WITH_TIMEOUT(monitor.inside(), 5000);
    QCOMPARE(nearbyIdentifiers(monitor), ctrs);

    // Guided commands name the zones they would enter, not those the vehicle is in
    const QString airportWarning = monitor.guidedCheck()->gotoWarning(vehicle, coordinate(kAirport.east, kAirport.north));
    QVERIFY2(airportWarning.contains(findZone(*manager, QStringLiteral("LSZH001"))->name()), qPrintable(airportWarning));
    QVERIFY(!airportWarning.contains(QStringLiteral("CTR")));
    const double terrainAMSL = vehicle->altitudeAMSL()->rawValue().toDouble() - vehicle->altitudeAboveTerr()->rawValue().toDouble();
    const QString prisonWarning = monitor.guidedCheck()->gotoWarning(vehicle, coordinate(kPrison.east, kPrison.north), std::isnan(terrainAMSL) ? kTerrainAMSL + 60 : terrainAMSL + 60);
    QVERIFY2(prisonWarning.contains(findZone(*manager, QStringLiteral("GZW0001"))->name()), qPrintable(prisonWarning));
    const QString orbitWarning = monitor.guidedCheck()->orbitWarning(vehicle, coordinate(kAirport.east, kAirport.north), 200, vehicle->altitudeAMSL()->rawValue().toDouble());
    QVERIFY2(orbitWarning.contains(findZone(*manager, QStringLiteral("LSZH001"))->name()), qPrintable(orbitWarning));
    QVERIFY(monitor.guidedCheck()->gotoWarning(nullptr, coordinate(kAirport.east, kAirport.north)).isEmpty());

    QStringList entered = spiedAlerts(alertSpy).filter(QStringLiteral("Entered"));
    entered.sort();
    QCOMPARE(entered, QStringList({ alertText(*manager, QStringLiteral("Entered"), ctrs[0]), alertText(*manager, QStringLiteral("Entered"), ctrs[1]) }));

    // The alerts are logged with the vehicle, in a file per day
    QStringList logLines;
    for (QDate date = startDate; date <= QDateTime::currentDateTimeUtc().date(); date = date.addDays(1)) {
        QFile log(monitor.alertLog()->filePath(date));
        if (log.open(QIODevice::ReadOnly | QIODevice::Text)) {
            logLines.append(QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts));
        }
    }
    (void) logLines.removeAll(GeoZoneAlertLog::header());
    QCOMPARE(logLines.count(), alertSpy.count());
    QCOMPARE(logLines.filter(QStringLiteral(",entered,")).count(), 2);
    QVERIFY(logLines.last().section(QLatin1Char(','), 1, 1) == QString::number(vehicle->id()));

    // Alerts can be turned off in flight
    settings->alertsEnabled()->setRawValue(false);
    QVERIFY(!monitor.monitoring());
    QVERIFY(monitor.nearbyZones().isEmpty());
    const qsizetype alertCount = alertSpy.count();
    QTest::qWait(1500);
    QCOMPARE(alertSpy.count(), alertCount);
    QVERIFY(monitor.nearbyZones().isEmpty());

    // Disconnecting the vehicle while monitoring clears its state
    settings->alertsEnabled()->setRawValue(true);
    QVERIFY(monitor.monitoring());
    QTRY_COMPARE_WITH_TIMEOUT(nearbyIdentifiers(monitor), ctrs, 5000);
    QVERIFY(monitor.inside());
    _disconnectMockLink();
    QTRY_VERIFY(!monitor.monitoring());
    QVERIFY(monitor.nearbyZones().isEmpty());
    QVERIFY(!monitor.inside());
}
