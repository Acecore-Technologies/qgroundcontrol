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
#include "GeoZoneAlertHistory.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneGuidedCheck.h"
#include "GeoZoneFlightRecorder.h"
#include "GeoZoneGeoid.h"
#include "GeoZoneBriefingExport.h"
#include "GeoZoneConformance.h"
#include "GeoZoneInfo.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMissionPath.h"
#include "GeoZoneMonitor.h"
#include "GeoZonePlanning.h"
#include "GeoZonePreflightCheck.h"
#include "GeoZoneOperationalIntent.h"
#include "GeoZoneRemoteId.h"
#include "GeoZoneReturnModel.h"
#include "GeoZoneSettings.h"
#include "GeoZoneSourceRepository.h"
#include "GeoZoneTestBase.h"
#include "GeoZoneTestHelpers.h"
#include "GeoZoneText.h"
#include "MissionController.h"
#include "MissionSettingsItem.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "QGroundControlQmlGlobal.h"
#include "QmlObjectListModel.h"
#include "RemoteIDSettings.h"
#include "SettingsManager.h"
#include "SimpleMissionItem.h"
#include "Vehicle.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QScopeGuard>
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
    return zone ? QStringLiteral("%1 geozone %2 (%3)").arg(event, zone->name(), GeoZoneText::restriction(zone->restriction())) : identifier;
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
    if (const GeoZoneVehicleMonitor *const active = monitor.activeMonitor()) {
        for (const GeoZoneAlertTracker::Nearby &nearby : active->nearby()) {
            zones.append(nearby.zone);
        }
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

/// @return The zone feature with its vertical limits in meters above ground
QJsonObject withHeightLimits(QJsonObject feature, double lower, double upper)
{
    QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = lower;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AGL");
    layer[QStringLiteral("upperLimit")] = upper;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AGL");
    geometry[0] = layer;
    feature[QStringLiteral("geometry")] = geometry;
    return feature;
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
    GeoZoneManager::instance()->repository()->clear();
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
    QVERIFY2(manager.repository()->loadFile(kZonesFile, errorString), qPrintable(errorString));
    QCOMPARE(manager.zoneCount(), 8);

    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);

    // Low below the CTRs into the prison zone, climbing out of it into CTR ZURICH, to the airport and descending
    // out of the CTR there. Each zone alerts once on approach and once on entry.
    fly(monitor, kStart, kPrison);
    QCOMPARE(nearbyIdentifiers(monitor), QStringList{ QStringLiteral("GZW0001") });
    QVERIFY(monitor.activeMonitor()->inside());
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
    QVERIFY(monitor.activeMonitor()->inside());

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
    QVERIFY2(manager.repository()->loadFile(kZonesFile, errorString), qPrintable(errorString));

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
    QTRY_COMPARE_WITH_TIMEOUT(identifiers(check.zones(GeoZoneMissionCheck::Crossed)), expected, 20000);
    QVERIFY(check.warningText().contains(findZone(manager, QStringLiteral("GZW0001"))->name()));
}

void GeoZoneFlightTest::_testMockLinkVehicle()
{
    // The alerts are logged on one day, the guided checks have no terrain tiles
    const ScopedEnvironment environment;
    GeoZoneManager *const manager = GeoZoneManager::instance();
    QString errorString;
    QVERIFY2(manager->repository()->loadFile(kZonesFile, errorString), qPrintable(errorString));
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    settings->audioAlerts()->setRawValue(false);

    // Monitors the vehicles like the application
    GeoZoneMonitor monitor;
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    const QTemporaryDir logDir;
    QVERIFY(logDir.isValid());
    monitor.alertHistory()->startLog(logDir.path());
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
    QTRY_VERIFY(monitor.activeMonitor() && !std::isnan(monitor.activeMonitor()->altitudeAMSL()));
    QVERIFY(!monitor.activeMonitor()->inside());
    QVERIFY(spiedAlerts(alertSpy).filter(QStringLiteral("Entered")).isEmpty());

    // Climbing 300 m enters the CTRs, MockLink reports the arming with its next heartbeat
    vehicle->sendMavCommand(vehicle->defaultComponentId(), MAV_CMD_NAV_TAKEOFF, false /* showError */, 0, 0, 0, 0, 0, 0, 300);
    QTRY_COMPARE_WITH_TIMEOUT(vehicle->altitudeRelative()->rawValue().toInt(), 300, 30000);
    const QStringList ctrs = { QStringLiteral("CTR0004"), QStringLiteral("CTR0015") };
    QTRY_VERIFY_WITH_TIMEOUT(monitor.activeMonitor()->inside(), 5000);
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

    // The alerts are logged with the vehicle, in a file per day, each followed by the popup showing it
    monitor.alertHistory()->flushLog();
    QFile log(monitor.alertHistory()->logFilePath(environment.now().date()));
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    QStringList logLines = QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    (void) logLines.removeAll(GeoZoneAlertHistory::logHeader());
    QCOMPARE(logLines.filter(QStringLiteral(",notified,")).count(), alertSpy.count());
    QCOMPARE(logLines.count(), alertSpy.count() * 2);
    QCOMPARE(logLines.filter(QStringLiteral(",entered,")).count(), 2);
    QVERIFY(logLines.last().section(QLatin1Char(','), 1, 1) == QString::number(vehicle->id()));

    // Alerts pause while the link is lost, since the last position isn't where the vehicle is, which is alerted once
    // with the zone it was last seen in
    _mockLink->setCommLost(true);
    QTRY_VERIFY_WITH_TIMEOUT(monitor.activeMonitor()->positionUnavailable(), 10000);
    const QStringList lost = spiedAlerts(alertSpy).filter(QStringLiteral("Position lost inside geozone"));
    QCOMPARE(lost.count(), 1);
    QVERIFY2((lost.first() == alertText(*manager, QStringLiteral("Position lost inside"), ctrs[0])) ||
             (lost.first() == alertText(*manager, QStringLiteral("Position lost inside"), ctrs[1])), qPrintable(lost.first()));
    _mockLink->setCommLost(false);
    QTRY_VERIFY_WITH_TIMEOUT(!monitor.activeMonitor()->positionUnavailable(), 10000);
    QCOMPARE(spiedAlerts(alertSpy).filter(QStringLiteral("Position lost")).count(), 1);

    // Alerts can be turned off in flight
    settings->alertsEnabled()->setRawValue(false);
    QVERIFY(!monitor.monitoring());
    QVERIFY(monitor.activeMonitor()->nearby().isEmpty());
    // Not even after the vehicle is evaluated again
    QVERIFY(!alertSpy.wait(GeoZoneMonitor::evaluateIntervalMSecs + 200));
    QVERIFY(monitor.activeMonitor()->nearby().isEmpty());

    // Disconnecting the vehicle while monitoring clears its state
    settings->alertsEnabled()->setRawValue(true);
    QVERIFY(monitor.monitoring());
    QTRY_COMPARE_WITH_TIMEOUT(nearbyIdentifiers(monitor), ctrs, 5000);
    QVERIFY(monitor.activeMonitor()->inside());
    _disconnectMockLink();
    QTRY_VERIFY(!monitor.monitoring());
    QVERIFY(!monitor.activeMonitor());
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
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ zone }), errorString), qPrintable(errorString));
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
    QVERIFY(check.zones(GeoZoneMissionCheck::Contingency).isEmpty());
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());

    // Returns at RTL_ALT 100 m from around the second item enter it, the mission is checked again when it changes
    returnAltitude->setRawValue(10000);
    QTRY_COMPARE(identifiers(check.zones(GeoZoneMissionCheck::Contingency)), QStringList{ QStringLiteral("X") });
    QVERIFY2(check.contingencyText().startsWith(QStringLiteral("A return to launch from items")), qPrintable(check.contingencyText()));
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    returnAltitude->setRawValue(1500);
    QTRY_VERIFY(check.zones(GeoZoneMissionCheck::Contingency).isEmpty());

    check.setMissionController(nullptr);
    _disconnectMockLink();
}

void GeoZoneFlightTest::_testGuidedReturnLandMission()
{
    ScopedEnvironment environment;
    _connectMockLink(MAV_AUTOPILOT_PX4);
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->parameterManager()->parametersReady(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->homePosition().isValid() && vehicle->coordinate().isValid() && !std::isnan(vehicle->altitudeAMSL()->rawValue().toDouble()), 30000);
    const QGeoCoordinate home = vehicle->homePosition();
    environment.setTerrain(home.altitude());
    const auto offset = [&home](double north, double east) {
        return home.atDistanceAndAzimuth(std::hypot(north, east), qRadiansToDegrees(std::atan2(east, north)));
    };

    // Around home from the ground and higher up, and a zone 2 km east at all altitudes
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        withHeightLimits(_circleFeature(QStringLiteral("UNDER"), QStringLiteral("PROHIBITED"), home, 300), 0, 150),
        withHeightLimits(_circleFeature(QStringLiteral("ABOVE"), QStringLiteral("PROHIBITED"), home, 100), 50, 1000),
        _circleFeature(QStringLiteral("EAST"), QStringLiteral("PROHIBITED"), offset(0, 2000), 300),
    }), errorString), qPrintable(errorString));
    GeoZoneMonitor monitor(&manager);
    GeoZoneGuidedCheck *const check = monitor.guidedCheck();

    // A mission 100 m high through the zone east
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    QTRY_COMPARE(masterController.managerVehicle(), vehicle);
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(home);
    for (const QGeoCoordinate &coordinate : { offset(1000, 0), offset(0, 2000), offset(0, -1000), offset(-1000, 0) }) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
        QVERIFY(item);
        item->altitude()->setRawValue(100);
    }
    QTRY_COMPARE(GeoZoneMissionPath::fromMission(missionController).segments().count(), 4);
    const GeoZoneReturnModel::Destinations destinations = GeoZoneGuidedCheck::returnDestinations(vehicle, missionController);
    QVERIFY(destinations.home.coordinate.distanceTo(home) < 0.01);
    QVERIFY(destinations.firstWaypoint.coordinate.distanceTo(offset(1000, 0)) < 0.01);

    // On the ground in the zone below, a resumed mission takes off through the zone above to the item
    QVERIFY(check->landWarning(vehicle).isEmpty());
    QVERIFY(check->returnWarning(vehicle, missionController).isEmpty());
    const QString resumeEast = check->missionWarning(vehicle, missionController, 2, true);
    QVERIFY2(resumeEast.startsWith(QStringLiteral("The mission from item 2")) && resumeEast.contains(QStringLiteral("EAST")), qPrintable(resumeEast));
    const QString resumeWest = check->missionWarning(vehicle, missionController, 3, true);
    QVERIFY2(resumeWest.contains(QStringLiteral("ABOVE")) && !resumeWest.contains(QStringLiteral("EAST")), qPrintable(resumeWest));
    QVERIFY(check->missionWarning(vehicle, missionController, 3, false).isEmpty());
    // Starting the mission now enters the zone east, which the fence on the vehicle doesn't exclude
    const QString startWarning = check->startWarning(missionController);
    QVERIFY2(startWarning.startsWith(QStringLiteral("The mission path enters restricted geozones:")) && startWarning.contains(QStringLiteral("EAST")) &&
             startWarning.contains(QStringLiteral("The GeoFence doesn't exclude")), qPrintable(startWarning));
    QVERIFY(check->startWarning(nullptr).isEmpty());

    // 300 m up, landing or returning to launch descends into the zone below
    vehicle->sendMavCommand(vehicle->defaultComponentId(), MAV_CMD_NAV_TAKEOFF, false /* showError */, 0, 0, 0, 0, 0, 0, 300);
    QTRY_COMPARE_WITH_TIMEOUT(vehicle->altitudeRelative()->rawValue().toInt(), 300, 30000);
    const QString landWarning = check->landWarning(vehicle);
    QVERIFY2(landWarning.startsWith(QStringLiteral("The landing")) && landWarning.contains(QStringLiteral("UNDER")) && !landWarning.contains(QStringLiteral("ABOVE")),
             qPrintable(landWarning));
    const QString returnWarning = check->returnWarning(vehicle, missionController);
    QVERIFY2(returnWarning.startsWith(QStringLiteral("The return to launch")) && returnWarning.contains(QStringLiteral("UNDER")), qPrintable(returnWarning));
    QVERIFY(check->returnWarning(nullptr, missionController).isEmpty());

    // The current item set: from the vehicle to the item and on along the mission
    QVERIFY2(check->missionWarning(vehicle, missionController, 2).contains(QStringLiteral("EAST")), qPrintable(check->missionWarning(vehicle, missionController, 2)));
    QVERIFY(check->missionWarning(vehicle, missionController, 3).isEmpty());
    QVERIFY(check->missionWarning(vehicle, missionController, 9).isEmpty());

    // Returns along the mission: on to its end, or back to the first waypoint and home
    const GeoZoneMissionPath missionPath = GeoZoneMissionPath::fromMission(missionController);
    GeoZoneReturnModel::Start start;
    start.position = monitor.vehiclePosition(vehicle, vehicle->coordinate());
    GeoZoneReturnModel::Path forward;
    forward.destination = GeoZoneReturnModel::Destination::MissionForward;
    QVERIFY(check->pathsWarning(GeoZoneGuidedCheck::returnPaths(forward, start, destinations, missionPath, 2, home.altitude())).contains(QStringLiteral("EAST")));
    QVERIFY(check->pathsWarning(GeoZoneGuidedCheck::returnPaths(forward, start, destinations, missionPath, 3, home.altitude())).isEmpty());
    GeoZoneReturnModel::Parameters parameters = GeoZoneMissionPath::returnParameters(vehicle);
    parameters.returnType = 2;
    const GeoZoneReturnModel::Path reverse = GeoZoneReturnModel::returnPath(parameters, destinations, start);
    QCOMPARE(static_cast<int>(reverse.destination), static_cast<int>(GeoZoneReturnModel::Destination::ReverseMission));
    QVERIFY(check->pathsWarning(GeoZoneGuidedCheck::returnPaths(reverse, start, destinations, missionPath, 4, home.altitude())).contains(QStringLiteral("EAST")));
    QVERIFY(!check->pathsWarning(GeoZoneGuidedCheck::returnPaths(reverse, start, destinations, missionPath, 2, home.altitude())).contains(QStringLiteral("EAST")));

    _disconnectMockLink();
}

void GeoZoneFlightTest::_testPreflightChecklist()
{
    using Check = GeoZonePreflightCheck;
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(10, 0)));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    Fact *const qnh = GeoZoneSettings::instance()->qnhHPa();
    Fact *const operatorId = SettingsManager::instance()->remoteIDSettings()->operatorID();
    const auto restore = qScopeGuard([qnh, operatorId]() {
        qnh->setRawValue(qnh->rawDefaultValue());
        operatorId->setRawValue(operatorId->rawDefaultValue());
    });
    operatorId->setRawValue(QString());

    // Without zones and mission
    Check check(&manager);
    QCOMPARE(check.data().state, Check::Warning);
    QVERIFY2(check.data().text.contains(QStringLiteral("No geozone data")), qPrintable(check.data().text));
    QCOMPARE(check.mission().state, Check::NotApplicable);
    QCOMPARE(check.remoteId().state, Check::NotApplicable);

    // A path west to east through a prohibited zone and U-space airspace, beside a zone limited in flight levels
    const QGeoCoordinate center(47, 8);
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("U"), QStringLiteral("USPACE"), center.atDistanceAndAzimuth(700, 90), 150),
        _withLimits(_circleFeature(QStringLiteral("L"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(800, 0), 200), 400, QStringLiteral("STD"), 700, QStringLiteral("STD")),
    }), errorString), qPrintable(errorString));
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    // At 500 m above mean sea level, whatever the terrain at the planned home
    for (const QGeoCoordinate &coordinate : { center.atDistanceAndAzimuth(1000, 270), center.atDistanceAndAzimuth(1000, 90) }) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
        QVERIFY(item);
        item->setAltitudeMode(QGroundControlQmlGlobal::AltitudeModeAbsolute);
        item->altitude()->setRawValue(500);
    }
    check.setPlanController(&masterController);
    // Not passed until the mission is checked
    check.update();
    QCOMPARE(check.mission().state, Check::Checking);
    QTRY_COMPARE(check.mission().state, Check::Failed);
    QVERIFY2(check.mission().text.contains(QStringLiteral("P (Prohibited)")), qPrintable(check.mission().text));
    QCOMPARE(check.data().state, Check::Passed);
    QCOMPARE(check.remoteId().state, Check::Warning);
    QVERIFY2(check.remoteId().text.contains(QStringLiteral("operator ID")), qPrintable(check.remoteId().text));
    QCOMPARE(check.qnh().state, Check::Warning);
    QVERIFY2(check.qnh().text.startsWith(QStringLiteral("Flight level limits of L")), qPrintable(check.qnh().text));
    // The fence must exclude the prohibited zone
    QCOMPARE(check.fence().state, Check::Warning);
    QVERIFY2(check.fence().text.startsWith(QStringLiteral("The vehicle's fence doesn't exclude the prohibited geozones the mission enters: P.")), qPrintable(check.fence().text));

    // Authorised, with the local QNH and the operator ID set
    const GeoZone *const prohibited = findZone(manager, QStringLiteral("P"));
    QVERIFY(prohibited);
    QVERIFY(manager.planning()->authorisations()->authorise(QVariant::fromValue(GeoZoneInfo(prohibited)), QStringLiteral("P-1"), QString(), QString()).isEmpty());
    qnh->setRawValue(1002.5);
    operatorId->setRawValue(QStringLiteral("FIN87astrdge12k8"));
    QTRY_COMPARE(check.mission().state, Check::Passed);
    QVERIFY2(check.mission().text.contains(QStringLiteral("authorised at any time")), qPrintable(check.mission().text));
    QTRY_COMPARE(check.qnh().state, Check::Passed);
    QVERIFY(check.qnh().text.contains(QStringLiteral("1002.5 hPa")));
    QTRY_COMPARE(check.remoteId().state, Check::Passed);
    // Without fence on the offline vehicle
    QCOMPARE(check.fence().state, Check::NotApplicable);

    // Outdated data and no mission
    QCOMPARE(Check::dataItem(3, QStringLiteral("A: 40 days old\nB: expired")).text, QStringLiteral("Geozone data may be outdated: A: 40 days old; B: expired"));
    missionController->removeAll();
    QTRY_COMPARE(check.mission().state, Check::NotApplicable);
    QCOMPARE(check.qnh().state, Check::NotApplicable);
    QCOMPARE(check.remoteId().state, Check::NotApplicable);

    // The fence verified by an upload, with the data it was made from
    Check::FenceInput fence;
    QCOMPARE(Check::fenceItem(fence).state, Check::NotApplicable);
    fence.vehicleFence.circles.append({ false, QGeoCircle(center, 400) });
    QCOMPARE(Check::fenceItem(fence).state, Check::Warning);
    QVERIFY(Check::fenceItem(fence).text.contains(QStringLiteral("wasn't verified")));
    GeoZoneSourceVersion source;
    source.key = QStringLiteral("zones.json");
    source.name = QStringLiteral("Zones");
    source.contentHash = QByteArrayLiteral("1");
    GeoZoneFenceUpload::Verification verification;
    verification.fence = fence.vehicleFence;
    verification.time = environment.now();
    verification.geoZoneFences = 1;
    verification.sources = { source };
    fence.verification = verification;
    fence.sources = { source };
    QCOMPARE(Check::fenceItem(fence).state, Check::Passed);
    QVERIFY(Check::fenceItem(fence).text.contains(QStringLiteral("made from the current geozone data")));
    fence.sources[0].contentHash = QByteArrayLiteral("2");
    QCOMPARE(Check::fenceItem(fence).state, Check::Warning);
    QVERIFY(Check::fenceItem(fence).text.contains(QStringLiteral("data which changed since: Zones")));
    fence.sources = { source };
    fence.vehicleFence.circles[0].circle.setRadius(500);
    QVERIFY(Check::fenceItem(fence).text.contains(QStringLiteral("changed since it was verified")));
    fence.vehicleFence = verification.fence;
    fence.unfenced = { prohibited };
    QVERIFY(Check::fenceItem(fence).text.startsWith(QStringLiteral("The vehicle's fence doesn't exclude the prohibited geozones the mission enters: P.")));
    manager.repository()->clear();
}

void GeoZoneFlightTest::_testFlightRecord()
{
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(10, 0)));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(kZonesFile, errorString), qPrintable(errorString));
    const GeoZone *const prison = findZone(manager, QStringLiteral("GZW0001"));
    QVERIFY(prison);
    QVERIFY(manager.planning()->authorisations()->authorise(QVariant::fromValue(GeoZoneInfo(prison)), QStringLiteral("GZW-42"), QString(), QString()).isEmpty());

    GeoZoneMonitor monitor(&manager);
    GeoZoneAlertHistory *const history = monitor.alertHistory();
    history->startLog(dir.filePath(QStringLiteral("logs")));
    GeoZoneFlightRecorder recorder(&manager, &monitor);
    recorder.setDirectory(dir.filePath(QStringLiteral("records")));

    // Through the authorised prison zone into CTR ZURICH and LSZH Zürich, a position each second
    constexpr int vehicleId = 1;
    int steps = 0;
    const auto flyTo = [&](const Waypoint &from, const Waypoint &to) {
        const double horizontal = std::hypot(to.east - from.east, to.north - from.north);
        const int count = std::max(1, static_cast<int>(std::ceil(std::max(horizontal / 10, std::abs(to.height - from.height) / 5))));
        for (int i = 1; i <= count; i++) {
            const double fraction = static_cast<double>(i) / count;
            environment.addSecs(1);
            const GeoZone::Position at = position(from.east + ((to.east - from.east) * fraction), from.north + ((to.north - from.north) * fraction),
                                                  from.height + ((to.height - from.height) * fraction));
            monitor.evaluate(at, vehicleId);
            recorder.addPosition(vehicleId, at);
            steps++;
        }
    };
    recorder.addPosition(vehicleId, position(kStart.east, kStart.north, kStart.height));
    QVERIFY(!recorder.recording(vehicleId));
    recorder.start(vehicleId, QStringLiteral("Quadrotor"), QStringLiteral("PX4 1.15.0"));
    const QDateTime start = environment.now();
    const Waypoint overPrison{ kPrison.east, kPrison.north, kCruiseHeight };
    const Waypoint overAirport{ kAirport.east, kAirport.north, kCruiseHeight };
    flyTo(kStart, kPrison);
    flyTo(kPrison, overPrison);
    flyTo(overPrison, overAirport);
    flyTo(overAirport, kAirport);
    QVERIFY(recorder.flight(vehicleId)->alerts.count() >= 4);

    // The pilot's answer to the last alert, which the alert popup logs
    history->flushLog();
    QFile log(history->logFilePath(environment.now().date()));
    QVERIFY(log.open(QIODevice::Append | QIODevice::Text));
    (void) log.write(QStringLiteral("%1,%2,47.42,8.53,710.0,430.0,approved,,LSZH001,Zürich,Prohibited,,,\n").arg(environment.now().toString(Qt::ISODateWithMs)).arg(vehicleId).toUtf8());
    log.close();
    environment.addSecs(5);
    const QString path = recorder.finish(vehicleId);
    QVERIFY(!path.isEmpty());
    QVERIFY(!recorder.recording(vehicleId));
    QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("GeoZoneFlight-2026-06-15T10-00-00Z-vehicle1.json"));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(json[QStringLiteral("schema")].toString(), QStringLiteral("qgc-geozone-flight-record"));
    QCOMPARE(json[QStringLiteral("version")].toInt(), 1);
    const QJsonObject flight = json[QStringLiteral("flight")].toObject();
    QCOMPARE(flight[QStringLiteral("vehicleId")].toInt(), vehicleId);
    QCOMPARE(flight[QStringLiteral("firmware")].toString(), QStringLiteral("PX4 1.15.0"));
    QCOMPARE(QDateTime::fromString(flight[QStringLiteral("start")].toString(), Qt::ISODateWithMs), start);
    QCOMPARE(flight[QStringLiteral("durationSeconds")].toDouble(), static_cast<double>(steps + 5));
    QVERIFY(_near(flight[QStringLiteral("maxAltitudeAMSL")].toDouble(), kTerrainAMSL + kCruiseHeight));

    // The track keeps a position each 10 m or 5 m of climb, with the last one
    const QJsonArray track = json[QStringLiteral("track")].toArray();
    QVERIFY2((track.count() > (steps / 3)) && (track.count() <= steps), qPrintable(QString::number(track.count())));
    QCOMPARE(QDateTime::fromString(track.last()[QStringLiteral("time")].toString(), Qt::ISODateWithMs), start.addSecs(steps));
    QVERIFY(_near(track.last()[QStringLiteral("altitudeAMSL")].toDouble(), kTerrainAMSL + kAirport.height));

    // The zones entered with the times, the briefing of the track flown
    QStringList entered;
    for (const QJsonValue &zone : json[QStringLiteral("zones")].toArray()) {
        if (zone[QStringLiteral("involvement")].toString() == QStringLiteral("entered")) {
            entered.append(zone[QStringLiteral("identifier")].toString());
            const QJsonObject crossing = zone[QStringLiteral("crossings")].toArray().first().toObject();
            QVERIFY(QDateTime::fromString(crossing[QStringLiteral("entryTime")].toString(), Qt::ISODate) > start);
        }
    }
    for (const QString &identifier : { QStringLiteral("GZW0001"), QStringLiteral("CTR0015"), QStringLiteral("LSZH001") }) {
        QVERIFY2(entered.contains(identifier), qPrintable(entered.join(QLatin1Char(' '))));
    }
    const QJsonArray permits = json[QStringLiteral("permits")].toArray();
    QCOMPARE(permits.count(), 1);
    QCOMPARE(permits[0][QStringLiteral("permit")].toString(), QStringLiteral("GZW-42"));
    QVERIFY(permits[0][QStringLiteral("coversFlight")].toBool());
    QVERIFY(!json[QStringLiteral("sources")].toArray().isEmpty());

    // The alerts and answers of the log
    QStringList events;
    for (const QJsonValue &alert : json[QStringLiteral("alerts")].toArray()) {
        QCOMPARE(alert[QStringLiteral("vehicle")].toInt(), vehicleId);
        events.append(alert[QStringLiteral("event")].toString());
    }
    QVERIFY2(events.contains(QStringLiteral("approaching")) && events.contains(QStringLiteral("entered")), qPrintable(events.join(QLatin1Char(' '))));
    QCOMPARE(events.constLast(), QStringLiteral("approved"));
    QCOMPARE(json[QStringLiteral("alerts")].toArray().last()[QStringLiteral("altitude_amsl_m")].toDouble(), 710.0);

    // Without the log, the alerts raised during the flight
    GeoZoneSettings::instance()->logAlerts()->setRawValue(false);
    recorder.start(vehicleId);
    environment.addSecs(1);
    monitor.removePositionMonitor(vehicleId);
    monitor.evaluate(position(kAirport.east, kAirport.north, kAirport.height), vehicleId);
    environment.addSecs(1);
    const QString secondPath = recorder.finish(vehicleId);
    QFile secondFile(secondPath);
    QVERIFY(secondFile.open(QIODevice::ReadOnly));
    const QJsonArray alerts = QJsonDocument::fromJson(secondFile.readAll()).object()[QStringLiteral("alerts")].toArray();
    QVERIFY(!alerts.isEmpty());
    QCOMPARE(alerts[0][QStringLiteral("event")].toString(), QStringLiteral("entered"));

    // Records older than the retention are deleted, none when kept forever
    QCOMPARE(GeoZoneFlightRecorder::retentionDays(), 0);
    QFile old(dir.filePath(QStringLiteral("records/GeoZoneFlight-2025-06-15T09-00-00Z-vehicle1.json")));
    QVERIFY(old.open(QIODevice::WriteOnly));
    old.close();
    QCOMPARE(recorder.removeOldRecords(QDate(2026, 6, 15), 0), 0);
    QCOMPARE(recorder.removeOldRecords(QDate(2026, 6, 15), 365), 1);
    QVERIFY(!old.exists());
    QVERIFY(QFile::exists(path));
    manager.repository()->clear();
}

void GeoZoneFlightTest::_testRemoteId()
{
    ScopedEnvironment environment;
    RemoteIDSettings *const remoteId = SettingsManager::instance()->remoteIDSettings();
    Fact *const include = GeoZoneSettings::instance()->includeOperatorDetails();
    const QList<Fact*> facts = { include, remoteId->operatorID(), remoteId->basicID(), remoteId->basicIDUaType(), remoteId->classificationType(),
                                 remoteId->categoryEU(), remoteId->classEU() };
    const auto restore = qScopeGuard([&facts]() {
        for (Fact *const fact : facts) {
            fact->setRawValue(fact->rawDefaultValue());
        }
    });
    remoteId->operatorID()->setRawValue(QString());

    // Why the Remote ID isn't ready
    GeoZoneRemoteId::Status status;
    QVERIFY(GeoZoneRemoteId::problem(status).contains(QStringLiteral("operator ID")));
    status.operatorIdGood = true;
    QVERIFY(GeoZoneRemoteId::problem(status).isEmpty());
    status.vehicle = true;
    QCOMPARE(GeoZoneRemoteId::problem(status), QStringLiteral("The vehicle reports no Remote ID."));
    status.available = true;
    status.commsGood = true;
    status.armStatusError = QStringLiteral("No GPS");
    QCOMPARE(GeoZoneRemoteId::problem(status), QStringLiteral("The Remote ID isn't ready to arm: No GPS."));
    status.armStatusGood = true;
    QVERIFY(GeoZoneRemoteId::problem(status).isEmpty());

    // The mission check warns while the mission enters U-space airspace
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("U"), QStringLiteral("USPACE"), center, 300),
    }), errorString), qPrintable(errorString));
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 2);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::USpace).count(), 1);
    QVERIFY2(check.remoteIdText().startsWith(QStringLiteral("The mission enters U-space airspace, which requires Remote ID. The operator ID isn't set")),
             qPrintable(check.remoteIdText()));
    QSignalSpy remoteIdSpy(&check, &GeoZoneMissionCheck::remoteIdChanged);
    remoteId->operatorID()->setRawValue(QStringLiteral("FIN87astrdge12k8"));
    QVERIFY(remoteIdSpy.count() > 0);
    QVERIFY(check.remoteIdText().isEmpty());
    missionController->removeAll();
    remoteId->operatorID()->setRawValue(QString());
    QTRY_VERIFY(check.zones(GeoZoneMissionCheck::USpace).isEmpty());
    QVERIFY(check.remoteIdText().isEmpty());

    // The exports name the operator and aircraft only when included
    remoteId->operatorID()->setRawValue(QStringLiteral("FIN87astrdge12k8"));
    remoteId->basicID()->setRawValue(QStringLiteral("1581F5FJD239C00DW22E"));
    remoteId->basicIDUaType()->setRawValue(2);
    remoteId->classificationType()->setRawValue(1);
    remoteId->categoryEU()->setRawValue(2);
    remoteId->classEU()->setRawValue(3);
    QVERIFY(!GeoZoneRemoteId::exportedOperatorDetails());
    GeoZoneBriefing briefing;
    briefing.operatorDetails = GeoZoneRemoteId::exportedOperatorDetails();
    QVERIFY(GeoZoneBriefingExport::toJson(briefing)[QStringLiteral("operator")].isNull());
    QVERIFY(!GeoZoneBriefingExport::toHtml(briefing).contains(QStringLiteral("FIN87astrdge12k8")));

    include->setRawValue(true);
    const std::optional<GeoZoneOperatorDetails> details = GeoZoneRemoteId::exportedOperatorDetails();
    QVERIFY(details);
    QCOMPARE(details->operatorIdType, QStringLiteral("CAA"));
    QCOMPARE(details->uasIdType, QStringLiteral("CAA"));
    QCOMPARE(details->uaType, QStringLiteral("Helicopter/Multirrotor"));
    QCOMPARE(details->category, QStringLiteral("Specific"));
    QCOMPARE(details->aircraftClass, QStringLiteral("Class 2"));
    briefing.operatorDetails = details;
    const QJsonObject briefingJson = GeoZoneBriefingExport::toJson(briefing);
    QCOMPARE(briefingJson[QStringLiteral("operator")][QStringLiteral("id")].toString(), QStringLiteral("FIN87astrdge12k8"));
    QCOMPARE(briefingJson[QStringLiteral("aircraft")][QStringLiteral("uasId")].toString(), QStringLiteral("1581F5FJD239C00DW22E"));
    QVERIFY(GeoZoneBriefingExport::toHtml(briefing).contains(QStringLiteral("FIN87astrdge12k8, type CAA")));
    GeoZoneOperationalIntent intent;
    QVERIFY(intent.toJson()[QStringLiteral("aircraft")].isNull());
    intent.operatorDetails = details;
    QCOMPARE(intent.toJson()[QStringLiteral("aircraft")][QStringLiteral("class")].toString(), QStringLiteral("Class 2"));
    QCOMPARE(intent.toJson()[QStringLiteral("operator")][QStringLiteral("region")].toString(), QStringLiteral("FAA"));
    manager.repository()->clear();
}

void GeoZoneFlightTest::_testConformance()
{
    using Conformance = GeoZoneConformance;
    ScopedEnvironment environment;
    const QDateTime start = QDateTime::fromSecsSinceEpoch(environment.now().toSecsSinceEpoch() + 3600, QTimeZone::UTC);
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    GeoZoneMonitor monitor(&manager);
    monitor.alertHistory()->startLog(dir.filePath(QStringLiteral("logs")));
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    Conformance conformance(&manager, &monitor);

    // Off without planned start, then with the volumes of the operational intent of the mission
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(500, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(500, 90), 2);
    QTRY_VERIFY(GeoZoneMissionPath::fromMission(missionController).totalDistance() > 900);
    conformance.setPlanController(&masterController);
    conformance.updateVolumes();
    QVERIFY(conformance.volumes().isEmpty());
    conformance.evaluate(GeoZone::Position{ center, 500, qQNaN() }, true, 1);
    QCOMPARE(conformance.state(), Conformance::Off);
    QVERIFY(!conformance.active());
    manager.planning()->setPlannedStart(start);
    conformance.updateVolumes();
    QVERIFY(!conformance.volumes().isEmpty());
    QCOMPARE(conformance.volumes().constFirst().timeStart, start.addSecs(-GeoZoneOperationalIntent::defaultTimeBufferSecs));
    QVERIFY(conformance.volumes().constFirst().contains(center.atDistanceAndAzimuth(450, 270)));
    manager.planning()->clearPlannedStart();
    conformance.updateVolumes();
    QCOMPARE(conformance.state(), Conformance::Off);

    // A volume 100 m around the center from 450 m to 550 m above mean sea level, for 10 minutes from the start
    const double geoid = GeoZoneGeometry::geoidHeight(center);
    Conformance::Volume volume;
    volume.circleCenter = center;
    volume.circleRadius = 100;
    volume.altitudeLower = geoid + 450;
    volume.altitudeUpper = geoid + 550;
    volume.timeStart = start;
    volume.timeEnd = start.addSecs(600);
    const auto at = [&center](double east, double altitude) { return GeoZone::Position{ center.atDistanceAndAzimuth(east, 90), altitude, qQNaN() }; };
    const QDateTime during = start.addSecs(60);
    QCOMPARE(Conformance::check({ volume }, at(0, 500), start.addSecs(-1)).state, Conformance::OutsideTime);
    QCOMPARE(Conformance::check({ volume }, at(0, 500), during).state, Conformance::Conforming);
    QCOMPARE(Conformance::check({ volume }, at(0, qQNaN()), during).state, Conformance::Conforming);
    Conformance::Result result = Conformance::check({ volume }, at(150, 500), during);
    QCOMPARE(result.state, Conformance::OutsideHorizontally);
    QVERIFY(_near(result.distance, 50));
    result = Conformance::check({ volume }, at(0, 590), during);
    QCOMPARE(result.state, Conformance::Above);
    QVERIFY(_near(result.distance, 40));
    result = Conformance::check({ volume }, at(50, 420), during);
    QCOMPARE(result.state, Conformance::Below);
    QVERIFY(_near(result.distance, 30));
    Conformance::Volume square = volume;
    square.circleCenter = QGeoCoordinate();
    for (const double azimuth : { 45.0, 135.0, 225.0, 315.0 }) {
        square.outline.append(center.atDistanceAndAzimuth(100 * std::sqrt(2.0), azimuth));
    }
    QVERIFY(_near(Conformance::horizontalDistance(square, center.atDistanceAndAzimuth(160, 90)), 60));
    QCOMPARE(Conformance::horizontalDistance(square, center.atDistanceAndAzimuth(90, 0)), 0.0);

    // In flight, the monitor alerts once when the vehicle leaves the volumes, again after it conformed
    conformance.setVolumes({ volume });
    environment.setNow(during);
    conformance.evaluate(at(150, 500), false, 1);
    QCOMPARE(conformance.state(), Conformance::Waiting);
    QVERIFY(conformance.conforming());
    conformance.evaluate(at(0, 500), true, 1);
    QCOMPARE(conformance.state(), Conformance::Conforming);
    QCOMPARE(conformance.stateText(), QStringLiteral("Inside planned volume"));
    conformance.evaluate(at(150, 500), true, 1);
    QCOMPARE(conformance.state(), Conformance::OutsideHorizontally);
    QCOMPARE(spiedAlerts(alertSpy), QStringList{ QStringLiteral("Outside planned volume: 50 m beside it") });
    conformance.evaluate(at(0, 590), true, 1);
    QCOMPARE(conformance.stateText(), QStringLiteral("Outside planned volume: 40 m above it"));
    QCOMPARE(alertSpy.count(), 1);
    conformance.evaluate(at(0, 500), true, 1);
    QVERIFY(conformance.conforming());
    conformance.evaluate(at(0, 590), true, 1);
    QCOMPARE(alertSpy.count(), 2);
    QCOMPARE(alertSpy.last()[0].toString(), QStringLiteral("Outside planned volume: 40 m above it"));
    conformance.evaluate(at(0, 500), true, 1);
    environment.setNow(start.addSecs(700));
    conformance.evaluate(at(0, 500), true, 1);
    QCOMPARE(conformance.state(), Conformance::OutsideTime);
    QCOMPARE(alertSpy.count(), 3);

    // Logged like the other alerts
    GeoZoneAlertHistory *const history = monitor.alertHistory();
    history->flushLog();
    QFile log(history->logFilePath(environment.now().date()));
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines = QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.count(), 4);
    const std::optional<GeoZoneMonitor::AlertEvent> logged = GeoZoneAlertHistory::parseLogLine(lines.last());
    QVERIFY(logged && (logged->kind == GeoZoneMonitor::AlertEvent::NonConformance));
    QCOMPARE(logged->zoneIdentifier, QStringLiteral("time"));
    QCOMPARE(logged->message, QStringLiteral("Outside planned volume: outside its time window"));
    QCOMPARE(GeoZoneMonitor::alertMessage(*GeoZoneAlertHistory::parseLogLine(lines[1]), QStringLiteral("50 m beside it"), QString()),
             QStringLiteral("Outside planned volume: 50 m beside it"));
}

GEOZONE_TEST(GeoZoneFlightTest)
