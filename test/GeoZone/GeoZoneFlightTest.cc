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
#include "GeoZoneReturnModel.h"
#include "GeoZoneSettings.h"
#include "GeoZoneTestHelpers.h"
#include "MissionController.h"
#include "MissionSettingsItem.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "SimpleMissionItem.h"
#include "Vehicle.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <algorithm>
#include <cmath>

using namespace GeoZoneTestHelpers;

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

QStringList identifiers(const QList<const GeoZone*> &zones)
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
    QList<const GeoZone*> zones;
    for (const QVariant &nearby : monitor.nearbyZones()) {
        zones.append(GeoZoneInfo::fromVariant(nearby.toMap().value(QStringLiteral("zone"))));
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
    // Unknown terrain unless given with the positions
    const ScopedEnvironment environment;
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
    const ScopedEnvironment environment;
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
    check.setUpdateDelay(0);
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
    // The alerts are logged on one day, the guided checks have no terrain tiles
    const ScopedEnvironment environment;
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

    _connectMockLink(MAV_AUTOPILOT_PX4);
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QTRY_VERIFY(monitor.monitoring());

    // Missions are checked returning to launch at the vehicle's return altitude
    QTRY_VERIFY(vehicle->parameterManager()->parametersReady());
    QVERIFY(vehicle->parameterManager()->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("RTL_RETURN_ALT")));
    const double returnAltitude = vehicle->parameterManager()->getParameter(ParameterManager::defaultComponentId, QStringLiteral("RTL_RETURN_ALT"))->rawValue().toDouble();
    QVERIFY(returnAltitude > 0);
    QCOMPARE(GeoZoneMissionPath::returnAltitude(vehicle), returnAltitude);

    // A parked vehicle is on the ground below the CTRs, even before terrain data is received
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->coordinate().isValid() && !std::isnan(vehicle->altitudeAMSL()->rawValue().toDouble()), 30000);
    QVERIFY(!vehicle->armed());
    QTRY_VERIFY(!std::isnan(monitor.altitudeAMSL()));
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
    QFile log(monitor.alertLog()->filePath(environment.now().date()));
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    QStringList logLines = QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    (void) logLines.removeAll(GeoZoneAlertLog::header());
    QCOMPARE(logLines.count(), alertSpy.count());
    QCOMPARE(logLines.filter(QStringLiteral(",entered,")).count(), 2);
    QVERIFY(logLines.last().section(QLatin1Char(','), 1, 1) == QString::number(vehicle->id()));

    // Alerts pause while the link is lost, since the last position isn't where the vehicle is
    _mockLink->setCommLost(true);
    QTRY_VERIFY_WITH_TIMEOUT(monitor.positionUnavailable(), 10000);
    _mockLink->setCommLost(false);
    QTRY_VERIFY_WITH_TIMEOUT(!monitor.positionUnavailable(), 10000);

    // Alerts can be turned off in flight
    settings->alertsEnabled()->setRawValue(false);
    QVERIFY(!monitor.monitoring());
    QVERIFY(monitor.nearbyZones().isEmpty());
    // Not even after the vehicle is evaluated again
    QVERIFY(!alertSpy.wait(GeoZoneMonitor::evaluateIntervalMSecs + 200));
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

void GeoZoneFlightTest::_testReturnParameterChanges()
{
    const ScopedEnvironment environment;
    _connectMockLink(MAV_AUTOPILOT_ARDUPILOTMEGA);
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->parameterManager()->parametersReady(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->homePosition().isValid(), 30000);

    // The return parameters of the MockLink ArduCopter, in centimeters until Copter 4.6
    ParameterManager *const parameterManager = vehicle->parameterManager();
    Fact *const returnAltitude = parameterManager->getParameter(ParameterManager::defaultComponentId, QStringLiteral("RTL_ALT"));
    QCOMPARE(returnAltitude->rawValue().toInt(), 1500);
    const GeoZoneReturnModel::Parameters parameters = GeoZoneMissionPath::returnParameters(vehicle);
    QCOMPARE(static_cast<int>(parameters.autopilot), static_cast<int>(GeoZoneReturnModel::Autopilot::ArduCopter));
    QCOMPARE(parameters.returnAltitude, 15.0);
    QCOMPARE(parameters.coneSlope, 3.0);
    QVERIFY(std::abs(parameters.rallyLimit - 300) < 0.01);

    // A square mission north and east of home 30 m high, the zone in its middle from 80 m above home
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    QTRY_COMPARE(masterController.managerVehicle(), vehicle);
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(vehicle->homePosition());
    const QGeoCoordinate home = missionController->plannedHomePosition();
    QVERIFY(home.isValid() && !std::isnan(home.altitude()));
    const auto offset = [&home](double north, double east) {
        return home.atDistanceAndAzimuth(std::hypot(north, east), qRadiansToDegrees(std::atan2(east, north)));
    };
    QJsonObject zone = _circleFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), offset(1000, 1000), 100);
    QJsonArray geometry = zone[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = home.altitude() + 80;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
    layer[QStringLiteral("upperLimit")] = home.altitude() + 1000;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AMSL");
    geometry[0] = layer;
    zone[QStringLiteral("geometry")] = geometry;
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ zone }), errorString), qPrintable(errorString));
    for (const QGeoCoordinate &coordinate : { offset(2000, 0), offset(2000, 2000), offset(0, 2000) }) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
        QVERIFY(item);
        item->altitude()->setRawValue(30);
    }
    QTRY_COMPARE(GeoZoneMissionPath::fromMission(missionController).segments().count(), 3);

    // Returning at the altitude of the mission stays below the zone
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    check.update();
    QVERIFY(check.contingencyZones().isEmpty());
    QVERIFY(check.crossedZones().isEmpty());

    // Returns at RTL_ALT 100 m from around the second item enter it, the mission is checked again when it changes
    returnAltitude->setRawValue(10000);
    QTRY_COMPARE(identifiers(check.contingencyZones()), QStringList{ QStringLiteral("X") });
    QVERIFY2(check.contingencyText().startsWith(QStringLiteral("A return to launch from items")), qPrintable(check.contingencyText()));
    QVERIFY(check.crossedZones().isEmpty());
    returnAltitude->setRawValue(1500);
    QTRY_VERIFY(check.contingencyZones().isEmpty());

    check.setMissionController(nullptr);
    _disconnectMockLink();
}
