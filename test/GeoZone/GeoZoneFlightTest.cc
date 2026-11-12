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
#include "GeoZoneAlertLevel.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMissionReader.h"
#include "GeoZoneMonitor.h"
#include "GeoZonePermits.h"
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
#include "TerrainQuery.h"
#include "Vehicle.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryDir>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlPropertyMap>
#include <QtQuick/QQuickItem>
#include <QtCore/QTimer>
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

/// Makes objects of QML source like the main window does, with its `globals`
class QmlHarness
{
public:
    QmlHarness()
    {
        engine.addImportPath(QStringLiteral("qrc:/qml"));
        engine.rootContext()->setContextProperty(QStringLiteral("globals"), &globals);
    }

    /// @return The object of the source, null when it has errors, see error
    QObject *create(const QByteArray &source)
    {
        QQmlComponent component(&engine);
        component.setData(source, QUrl(QStringLiteral("qrc:/qml/QGroundControl/GeoZone/Flight/GeoZoneTest.qml")));
        object.reset(component.create());
        error = component.errorString();
        return object.get();
    }

    /// @return Result of the function of the object called with the arguments
    static QVariant call(QObject *object, const char *function, const QVariantList &arguments = {})
    {
        QVariant result;
        switch (arguments.count()) {
        case 0:
            (void) QMetaObject::invokeMethod(object, function, Q_RETURN_ARG(QVariant, result));
            break;
        case 1:
            (void) QMetaObject::invokeMethod(object, function, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, arguments[0]));
            break;
        default:
            break;
        }
        return result;
    }

    QQmlEngine engine;
    QQmlPropertyMap globals;
    std::unique_ptr<QObject> object;
    QString error;
};

void restoreSettingDefaults()
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    settings->audioAlerts()->setRawValue(settings->audioAlerts()->rawDefaultValue());
}

// The checked accessors below fail the test where a result is missing, rather than crash the whole test run by
// dereferencing nothing, and go on with an empty result.

/// @return The monitor, or one of no vehicle after failing the test when it is null
GeoZoneVehicleMonitor &checkedMonitor(GeoZoneVehicleMonitor *vehicleMonitor, const char *what, const char *file, int line)
{
    if (vehicleMonitor) {
        return *vehicleMonitor;
    }
    QTest::qFail(what, file, line);
    // Kept for the test run: it would outlive the application as a static object
    static GeoZoneVehicleMonitor *const none = new GeoZoneVehicleMonitor(nullptr, GeoZoneMonitor::noVehicle, 0, []() { return qint64{ 0 }; });
    return *none;
}

/// @return The zone with the identifier, or an empty zone after failing the test when it isn't loaded
const GeoZone &checkedZone(const GeoZoneManager &manager, const QString &identifier, const char *file, int line)
{
    if (const GeoZone *const zone = findZone(manager, identifier)) {
        return *zone;
    }
    QTest::qFail(qPrintable(QStringLiteral("No zone %1").arg(identifier)), file, line);
    static const GeoZone none{ GeoZone::Data() };
    return none;
}

#define ACTIVE(monitor) checkedMonitor((monitor).activeMonitor(), "No monitor of the active vehicle of " #monitor, __FILE__, __LINE__)
#define ZONE(manager, identifier) checkedZone((manager), QStringLiteral(identifier), __FILE__, __LINE__)

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
    QVERIFY(ACTIVE(monitor).inside());
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
    // CTR ZURICH 60 m above stays near: within the vertical alert threshold of 45 m without velocity, and the
    // hysteresis of the zone just left
    QCOMPARE(nearbyIdentifiers(monitor), (QStringList{ QStringLiteral("CTR0015"), QStringLiteral("LSZH001") }));
    QVERIFY(ACTIVE(monitor).inside());
    QVERIFY(!ACTIVE(monitor).insideZoneKeys().contains(ZONE(manager, "CTR0015").key()));

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
    QVERIFY(check.warningText().contains(ZONE(manager, "GZW0001").name()));
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

    // Monitors the vehicles like the application. The folder of its log outlives it.
    const QTemporaryDir logDir;
    QVERIFY(logDir.isValid());
    GeoZoneMonitor monitor;
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
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
    QCOMPARE(GeoZoneMissionReader::returnParameters(vehicle).returnAltitude, returnAltitude);

    // A parked vehicle is on the ground below the CTRs, even before terrain data is received
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->coordinate().isValid() && !std::isnan(vehicle->altitudeAMSL()->rawValue().toDouble()), 30000);
    QVERIFY(!vehicle->armed());
    QTRY_VERIFY(monitor.activeMonitor() && !std::isnan(ACTIVE(monitor).altitudeAMSL()));
    QVERIFY(!ACTIVE(monitor).inside());
    QVERIFY(spiedAlerts(alertSpy).filter(QStringLiteral("Entered")).isEmpty());

    // Climbing 300 m enters the CTRs, MockLink reports the arming with its next heartbeat
    vehicle->sendMavCommand(vehicle->defaultComponentId(), MAV_CMD_NAV_TAKEOFF, false /* showError */, 0, 0, 0, 0, 0, 0, 300);
    QTRY_COMPARE_WITH_TIMEOUT(vehicle->altitudeRelative()->rawValue().toInt(), 300, 30000);
    const QStringList ctrs = { QStringLiteral("CTR0004"), QStringLiteral("CTR0015") };
    QTRY_VERIFY_WITH_TIMEOUT(ACTIVE(monitor).inside(), 5000);
    QCOMPARE(nearbyIdentifiers(monitor), ctrs);

    // Guided commands name the zones they would enter, not those the vehicle is in
    const QString airportWarning = monitor.guidedCheck()->gotoWarning(vehicle, coordinate(kAirport.east, kAirport.north));
    QVERIFY2(airportWarning.contains(ZONE(*manager, "LSZH001").name()), qPrintable(airportWarning));
    QVERIFY(!airportWarning.contains(QStringLiteral("CTR")));
    const double terrainAMSL = vehicle->altitudeAMSL()->rawValue().toDouble() - vehicle->altitudeAboveTerr()->rawValue().toDouble();
    const QString prisonWarning = monitor.guidedCheck()->gotoWarning(vehicle, coordinate(kPrison.east, kPrison.north), std::isnan(terrainAMSL) ? kTerrainAMSL + 60 : terrainAMSL + 60);
    QVERIFY2(prisonWarning.contains(ZONE(*manager, "GZW0001").name()), qPrintable(prisonWarning));
    const QString orbitWarning = monitor.guidedCheck()->orbitWarning(vehicle, coordinate(kAirport.east, kAirport.north), 200, vehicle->altitudeAMSL()->rawValue().toDouble());
    QVERIFY2(orbitWarning.contains(ZONE(*manager, "LSZH001").name()), qPrintable(orbitWarning));
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
    QCOMPARE(logLines.last().section(QLatin1Char(','), 1, 1), QString::number(vehicle->id()));

    // Alerts pause while the link is lost, since the last position isn't where the vehicle is, which is alerted once
    // with the zone it was last seen in
    _mockLink->setCommLost(true);
    QTRY_VERIFY_WITH_TIMEOUT(ACTIVE(monitor).positionUnavailable(), 10000);
    // ... and geo-awareness is unavailable, a warning once it lasts
    QTRY_COMPARE_WITH_TIMEOUT(spiedAlerts(alertSpy).filter(QStringLiteral("Geo-awareness unavailable: telemetry link lost")).count(), 1, 10000);
    QCOMPARE(monitor.awareness()->stateValue(), static_cast<int>(GeoZoneAwareness::Unavailable));
    QCOMPARE(monitor.awareness()->reasons().constFirst(), QStringLiteral("telemetry link lost"));
    const QStringList lost = spiedAlerts(alertSpy).filter(QStringLiteral("Position lost inside geozone"));
    QCOMPARE(lost.count(), 1);
    QVERIFY2((lost.first() == alertText(*manager, QStringLiteral("Position lost inside"), ctrs[0])) ||
             (lost.first() == alertText(*manager, QStringLiteral("Position lost inside"), ctrs[1])), qPrintable(lost.first()));
    _mockLink->setCommLost(false);
    QTRY_VERIFY_WITH_TIMEOUT(!ACTIVE(monitor).positionUnavailable(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(monitor.awareness()->stateValue() != GeoZoneAwareness::Unavailable, 10000);
    // The state it comes back to once it lasts, an advisory when available
    QTRY_VERIFY_WITH_TIMEOUT(spiedAlerts(alertSpy).constLast().startsWith(QStringLiteral("Geo-awareness ")) &&
                             !spiedAlerts(alertSpy).constLast().contains(QStringLiteral("link lost")), 10000);
    QCOMPARE(spiedAlerts(alertSpy).filter(QStringLiteral("Position lost")).count(), 1);

    // Disconnecting the vehicle while monitoring clears its state
    QVERIFY(monitor.monitoring());
    QTRY_COMPARE_WITH_TIMEOUT(nearbyIdentifiers(monitor), ctrs, 5000);
    QVERIFY(ACTIVE(monitor).inside());
    _disconnectMockLink();
    QTRY_VERIFY(!monitor.monitoring());
    QVERIFY(!monitor.activeMonitor());
}

void GeoZoneFlightTest::_testTerrainQueryRetry()
{
    // The limits above ground of the zones are compared with the vehicle's height above the terrain, which it gets
    // from terrain queries for its position. The queries are sent in batches by the timer of their manager.
    _connectMockLink(MAV_AUTOPILOT_PX4);
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->coordinate().isValid(), 30000);
    const QList<QTimer*> timers = TerrainAtCoordinateBatchManager::instance()->findChildren<QTimer*>(Qt::FindDirectChildrenOnly);
    QCOMPARE(timers.count(), 1);
    const QTimer *const batchTimer = timers.constFirst();
    QTRY_VERIFY_WITH_TIMEOUT(!batchTimer->isActive(), 10000);

    // A query fails, like when the terrain tile can't be downloaded: the height is unknown. The vehicle stays where
    // it is, so no query follows for a new position: the same position is queried again after a while.
    QVERIFY(QMetaObject::invokeMethod(vehicle, "_altitudeAboveTerrainReceived", Q_ARG(bool, false), Q_ARG(QList<double>, QList<double>())));
    QVERIFY(std::isnan(vehicle->altitudeAboveTerr()->rawValue().toDouble()));
    QVERIFY(!batchTimer->isActive());
    QTRY_VERIFY_WITH_TIMEOUT(batchTimer->isActive(), 15000);
    _disconnectMockLink();
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
    const GeoZoneReturnModel::Parameters parameters = GeoZoneMissionReader::returnParameters(vehicle);
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
    QTRY_COMPARE(GeoZoneMissionReader::path(missionController).segments().count(), 3);

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
    QTRY_COMPARE(GeoZoneMissionReader::path(missionController).segments().count(), 4);
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
    const GeoZoneMissionPath missionPath = GeoZoneMissionReader::path(missionController);
    GeoZoneReturnModel::Start start;
    start.position = monitor.vehiclePosition(vehicle, vehicle->coordinate());
    GeoZoneReturnModel::Path forward;
    forward.destination = GeoZoneReturnModel::Destination::MissionForward;
    QVERIFY(check->pathsWarning(GeoZoneGuidedCheck::returnPaths(forward, start, destinations, missionPath, 2, home.altitude())).contains(QStringLiteral("EAST")));
    QVERIFY(check->pathsWarning(GeoZoneGuidedCheck::returnPaths(forward, start, destinations, missionPath, 3, home.altitude())).isEmpty());
    GeoZoneReturnModel::Parameters parameters = GeoZoneMissionReader::returnParameters(vehicle);
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
    QVERIFY(manager.planning()->authorisations()->authorise(QVariant::fromValue(GeoZoneInfo(prohibited)), QStringLiteral("P-1"), QString(), QStringLiteral("2026-12-31")).isEmpty());
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
    // Treated as invalid, like the Plan View banner and the indicator report it
    const Check::Item expired = Check::dataItem(3, QStringLiteral("A: 40 days old"), true);
    QCOMPARE(expired.state, Check::Warning);
    QCOMPARE(expired.text, QStringLiteral("Expired geozone data, its zones are still checked: A: 40 days old"));
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
    const GeoZoneFlightRecorder::Flight *const recorded = recorder.flight(vehicleId);
    QVERIFY(recorded);
    QCOMPARE_GE(recorded->alerts.count(), 4);

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

    // When the log can't be written, the alerts raised during the flight
    QFile blocker(dir.filePath(QStringLiteral("blocker")));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    history->startLog(dir.filePath(QStringLiteral("blocker/logs")));
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

    // A record which can't be written, here for a file in the way of its folder, is reported
    QSignalSpy writtenSpy(&recorder, &GeoZoneFlightRecorder::recordWritten);
    QSignalSpy failedSpy(&recorder, &GeoZoneFlightRecorder::recordFailed);
    const QString records = recorder.directory();
    recorder.setDirectory(dir.filePath(QStringLiteral("blocker/records")));
    recorder.start(vehicleId);
    environment.addSecs(1);
    recorder.addPosition(vehicleId, position(kAirport.east, kAirport.north, kAirport.height));
    QVERIFY(recorder.finish(vehicleId).isEmpty());
    QVERIFY(!recorder.recording(vehicleId));
    QVERIFY(writtenSpy.isEmpty());
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY2(failedSpy.first().first().toString().startsWith(QStringLiteral("The geozone record of the flight of vehicle 1 couldn't be written: ")),
             qPrintable(failedSpy.first().first().toString()));
    // Without a flight, there is nothing to report
    QVERIFY(recorder.finish(vehicleId).isEmpty());
    QCOMPARE(failedSpy.count(), 1);
    recorder.setDirectory(records);

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
    QTRY_VERIFY(GeoZoneMissionReader::path(missionController).totalDistance() > 900);
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
    // Without the height of the geoid there, the altitude can't be compared either: not "above" by an infinite distance
    QCOMPARE(Conformance::check({ volume }, at(0, 500), during, qQNaN()).state, Conformance::Conforming);
    QCOMPARE(Conformance::check({ volume }, at(0, 590), during, qQNaN()).state, Conformance::Conforming);
    QCOMPARE(Conformance::check({ volume }, at(0, qInf()), during).state, Conformance::Conforming);
    QCOMPARE(Conformance::check({ volume }, at(150, 500), during, qQNaN()).state, Conformance::OutsideHorizontally);
    QCOMPARE(Conformance::check({ volume }, at(0, 590), during, geoid).state, Conformance::Above);
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

    // Shown in the alert popup like the other alerts
    QCOMPARE(monitor.alertModel()->count(), 3);
    QCOMPARE(monitor.alertModel()->alerts().constFirst().level, GeoZoneAlertLevel::Caution);

    // Logged like the other alerts, each followed by the popup showing it
    GeoZoneAlertHistory *const history = monitor.alertHistory();
    QVERIFY(history->flushLog());
    QFile log(history->logFilePath(environment.now().date()));
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines = QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.count(), 7);
    const std::optional<GeoZoneMonitor::AlertEvent> logged = GeoZoneAlertHistory::parseLogLine(lines[5]);
    QVERIFY(logged);
    QCOMPARE(logged->kind, GeoZoneMonitor::AlertEvent::NonConformance);
    QCOMPARE(logged->zoneIdentifier, QStringLiteral("time"));
    QCOMPARE(logged->message, QStringLiteral("Outside planned volume: outside its time window"));
    QCOMPARE(GeoZoneAlertHistory::logFields(lines[6]).value(6), QStringLiteral("notified"));
    QCOMPARE(GeoZoneAlertHistory::logFields(lines[6]).value(14), GeoZoneAlertHistory::logFields(lines[5]).value(14));
    const std::optional<GeoZoneMonitor::AlertEvent> first = GeoZoneAlertHistory::parseLogLine(lines[1]);
    QVERIFY(first);
    QCOMPARE(GeoZoneMonitor::alertMessage(*first, QStringLiteral("50 m beside it"), QString()), QStringLiteral("Outside planned volume: 50 m beside it"));
}


void GeoZoneFlightTest::_testGroundInsideZone()
{
    const ScopedEnvironment environment;
    GeoZoneManager *const manager = GeoZoneManager::instance();
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A prohibited zone from the ground around the MockLink vehicles
    QString errorString;
    QVERIFY2(manager->repository()->loadFile(_writeFeatures(dir, QStringLiteral("ground.json"), QJsonArray{
        _circleFeature(QStringLiteral("GROUND"), QStringLiteral("PROHIBITED"), kOrigin, 1000),
    }), errorString), qPrintable(errorString));
    GeoZoneSettings::instance()->audioAlerts()->setRawValue(false);
    const QString onGround = QStringLiteral("On the ground inside prohibited geozone GROUND \u2014 take-off not allowed here");
    const QString arming = QStringLiteral("Arming inside prohibited geozone GROUND \u2014 take-off not allowed here");

    for (const MAV_AUTOPILOT autopilot : { MAV_AUTOPILOT_PX4, MAV_AUTOPILOT_ARDUPILOTMEGA }) {
        GeoZoneMonitor monitor;
        monitor.alertHistory()->startLog(dir.filePath(QStringLiteral("logs")));
        QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
        _connectMockLink(autopilot);
        Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
        QVERIFY(vehicle);

        // Once its position is known, the disarmed vehicle is alerted like by a pre-arm check
        QTRY_VERIFY_WITH_TIMEOUT(spiedAlerts(alertSpy).contains(onGround), 30000);
        QVERIFY(!vehicle->armed());
        QVERIFY(ACTIVE(monitor).onGround());
        QCOMPARE(ACTIVE(monitor).groundWarning(), onGround);
        QCOMPARE(ACTIVE(monitor).groundLevel(), static_cast<int>(GeoZoneAlertLevel::Warning));
        QVERIFY(spiedAlerts(alertSpy).filter(QStringLiteral("Entered")).isEmpty());

        // The checklist fails and arming from the Fly View warns
        PlanMasterController masterController;
        masterController.setFlyView(false);
        masterController.start();
        QTRY_COMPARE(masterController.managerVehicle(), vehicle);
        GeoZonePreflightCheck check(manager);
        check.setMonitor(&monitor);
        check.setPlanController(&masterController);
        QTRY_COMPARE(check.takeoff().state, GeoZonePreflightCheck::Failed);
        QCOMPARE(check.takeoff().text, onGround + QLatin1Char('.'));
        QCOMPARE(monitor.guidedCheck()->groundWarning(vehicle), onGround);

        // Arming is alerted again, once for the acknowledged command and the armed vehicle
        vehicle->setArmed(true, false /* showError */);
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->armed(), 10000);
        QTRY_VERIFY(spiedAlerts(alertSpy).contains(arming));
        QTest::qWait(GeoZoneMonitor::evaluateIntervalMSecs * 2);
        QCOMPARE(spiedAlerts(alertSpy).filter(QStringLiteral("Arming inside")).count(), 1);
        QTRY_COMPARE(check.takeoff().state, GeoZonePreflightCheck::NotApplicable);
        QVERIFY(monitor.guidedCheck()->groundWarning(vehicle).isEmpty());

        // Both are logged
        monitor.alertHistory()->flushLog();
        QFile log(monitor.alertHistory()->logFilePath(environment.now().date()));
        QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(log.readAll()).count(QStringLiteral(",on_ground,")), autopilot == MAV_AUTOPILOT_PX4 ? 2 : 4);
        _disconnectMockLink();
    }
}


void GeoZoneFlightTest::_testChecklistButton()
{
    using Check = GeoZonePreflightCheck;
    QmlHarness harness;
    QObject *const root = harness.create(R"(
        import QtQuick
        import QGroundControl.GeoZone
        import QGroundControl.Palette
        Item {
            property alias button: button
            function buttonPassedChanged() { }
            QGCPalette { id: qgcPal }
            GeoZonePreFlightCheckButton { id: button; name: "Fence" }
        }
    )");
    QVERIFY2(root, qPrintable(harness.error));
    QObject *const button = root->property("button").value<QObject*>();
    QVERIFY(button);
    const auto setItem = [button](Check::State state, const QString &text) {
        QVERIFY(button->setProperty("item", QVariantMap{ { QStringLiteral("state"), static_cast<int>(state) }, { QStringLiteral("text"), text } }));
    };
    const auto passed = [button]() { return button->property("passed").toBool(); };
    const auto text = [button]() { return button->property("_text").toString(); };

    // A warning is accepted by clicking it
    setItem(Check::Warning, QStringLiteral("No current position"));
    QVERIFY(!passed());
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QVERIFY(passed());
    QVERIFY2(text().endsWith(QStringLiteral("No current position (accepted)")), qPrintable(text()));
    // ... and stays accepted while the checklist is updated with the same text
    setItem(Check::Warning, QStringLiteral("No current position"));
    QVERIFY(passed());

    // Another warning is to be accepted again
    setItem(Check::Warning, QStringLiteral("Inside geozone X"));
    QVERIFY(!passed());
    QVERIFY2(text().endsWith(QStringLiteral("Inside geozone X")), qPrintable(text()));
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QVERIFY(passed());
    // ... as after a reset of the checklist
    QVERIFY(QMetaObject::invokeMethod(button, "reset"));
    QVERIFY(!passed());
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QVERIFY(passed());

    // A failure can't be accepted, and what passes needs no click
    setItem(Check::Failed, QStringLiteral("Inside prohibited geozone P"));
    QVERIFY(!passed());
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QVERIFY(!passed());
    setItem(Check::Passed, QStringLiteral("Fine"));
    QVERIFY(passed());
    QVERIFY2(text().endsWith(QStringLiteral("Fine")), qPrintable(text()));
    setItem(Check::Warning, QStringLiteral("Inside geozone X"));
    QVERIFY(!passed());

    // Names of external data aren't markup
    setItem(Check::Warning, QStringLiteral("Inside <img src=\"x\"> & <b>P</b>"));
    QVERIFY2(text().endsWith(QStringLiteral("Inside &lt;img src=\"x\"&gt; &amp; &lt;b&gt;P&lt;/b&gt;")), qPrintable(text()));
}

void GeoZoneFlightTest::_testChecklistReset()
{
    // The checklist of the active vehicle, without geozone data: its Geozones group warns about that
    _connectMockLink(MAV_AUTOPILOT_PX4);
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->parameterManager()->parametersReady(), 30000);
    QmlHarness harness;
    harness.globals.insert(QStringLiteral("activeVehicle"), QVariant::fromValue(vehicle));
    harness.globals.insert(QStringLiteral("planMasterControllerFlyView"), QVariant::fromValue<QObject*>(nullptr));
    QObject *const root = harness.create(R"(
        import QtQuick
        import QGroundControl.GeoZone
        import QGroundControl.Palette
        Item {
            property alias list: list
            QGCPalette { id: qgcPal }
            GeoZonePreFlightCheckList { id: list }
        }
    )");
    QVERIFY2(root, qPrintable(harness.error));
    QObject *const list = root->property("list").value<QObject*>();
    QVERIFY(list);
    // The checklist of the vehicle type, in its loader
    QObject *standard = nullptr;
    for (QObject *const child : list->findChildren<QObject*>()) {
        if (child->property("allChecksPassed").isValid() && child->property("vehicleCopy").isValid()) {
            standard = child;
        }
    }
    QVERIFY(standard);
    QList<QObject*> geoZoneButtons;
    for (QObject *const child : list->findChildren<QObject*>()) {
        if (child->property("item").isValid() && child->property("telemetryFailure").isValid()) {
            geoZoneButtons.append(child);
        }
    }
    QCOMPARE(geoZoneButtons.count(), 8);
    const auto pending = [&geoZoneButtons]() {
        QList<QObject*> buttons;
        for (QObject *const button : std::as_const(geoZoneButtons)) {
            if (!button->property("passed").toBool()) {
                buttons.append(button);
            }
        }
        return buttons;
    };
    const auto acceptAll = [&pending]() {
        for (QObject *const button : pending()) {
            (void) QMetaObject::invokeMethod(button, "clicked");
        }
    };
    // Once the items are checked, the warnings left are the pilot's to accept
    QTRY_VERIFY_WITH_TIMEOUT(!pending().isEmpty() && std::all_of(geoZoneButtons.cbegin(), geoZoneButtons.cend(), [](QObject *button) {
        return button->property("passed").toBool() || button->property("allowTelemetryFailureOverride").toBool();
    }), 10000);
    const qsizetype warnings = pending().count();

    // The vehicle's state is written here only: not passed by the checklist of the vehicle type alone
    QList<int> states;
    const QMetaObject::Connection connection = connect(vehicle, &Vehicle::checkListStateChanged, this, [vehicle, &states]() { states.append(vehicle->checkListState()); });
    QVERIFY(standard->setProperty("allChecksPassed", true));
    QVERIFY(!list->property("allChecksPassed").toBool());
    QVERIFY2(!states.contains(Vehicle::CheckListPassed), "passed without the geozone checks");
    acceptAll();
    QVERIFY(pending().isEmpty());
    QTRY_VERIFY(list->property("allChecksPassed").toBool());
    QCOMPARE(states, QList<int>{ Vehicle::CheckListPassed });

    // The reset of the checklist resets the geozone checks too
    QVERIFY(QMetaObject::invokeMethod(standard, "_resetGroups"));
    QCOMPARE(pending().count(), warnings);
    QVERIFY(!list->property("allChecksPassed").toBool());
    QCOMPARE(states.constLast(), static_cast<int>(Vehicle::CheckListFailed));
    acceptAll();
    QVERIFY(pending().isEmpty());

    // ... like another active vehicle, which mustn't inherit what was accepted
    Vehicle other(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    harness.globals.insert(QStringLiteral("activeVehicle"), QVariant::fromValue(&other));
    QCOMPARE(pending().count(), warnings);
    disconnect(connection);
    harness.object.reset();
    _disconnectMockLink();
}

void GeoZoneFlightTest::_testGuidedActionWarning()
{
    // The MockLink vehicle below a zone from 50 m above ground, 1.2 km west of a zone at all altitudes
    const ScopedMetricUnits metricUnits;
    ScopedEnvironment environment;
    _connectMockLink(MAV_AUTOPILOT_PX4);
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->parameterManager()->parametersReady(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->homePosition().isValid() && vehicle->coordinate().isValid() && !std::isnan(vehicle->altitudeAMSL()->rawValue().toDouble()), 30000);
    const QGeoCoordinate home = vehicle->homePosition();
    environment.setTerrain(home.altitude());
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager *const manager = GeoZoneManager::instance();
    QString errorString;
    QVERIFY2(manager->repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        withHeightLimits(_circleFeature(QStringLiteral("ABOVE"), QStringLiteral("PROHIBITED"), home, 100), 50, 1000),
        _circleFeature(QStringLiteral("EAST"), QStringLiteral("PROHIBITED"), home.atDistanceAndAzimuth(1500, 90), 300),
    }), errorString), qPrintable(errorString));
    QVERIFY(GeoZoneMonitor::instance()->guidedCheck());

    QmlHarness harness;
    QObject *const root = harness.create(R"(
        import QtQuick
        import QtQuick.Layouts
        import QGroundControl.GeoZone
        ColumnLayout {
            id: root
            property alias warning: warning
            property real sliderValue: 0
            property real loiterRadius: 0
            property var loiterCenter
            property var dialog: QtObject {
                property bool visible: true
                property int action: 0
                property var actionData
                property string optionText: ""
                property bool optionChecked: false
                property var guidedValueSlider: QtObject {
                    function getOutputValue() { return root.sliderValue }
                }
                property var guidedController: QtObject {
                    readonly property int actionRTL: 1
                    readonly property int actionLand: 2
                    readonly property int actionTakeoff: 3
                    readonly property int actionArm: 4
                    readonly property int actionChangeAlt: 7
                    readonly property int actionGoto: 8
                    readonly property int actionSetWaypoint: 9
                    readonly property int actionOrbit: 10
                    readonly property int actionStartMission: 12
                    readonly property int actionContinueMission: 13
                    readonly property int actionResumeMission: 14
                    readonly property int actionResumeMissionUploadFail: 16
                    readonly property int actionPause: 17
                    readonly property int actionChangeLoiterRadius: 33
                    property var missionController: null
                    property var orbitMapCircle: null
                    property var fwdFlightGotoMapCircle: QtObject {
                        property var coordinate: root.loiterCenter
                        property var radius: QtObject { property real rawValue: root.loiterRadius }
                    }
                }
            }
            GeoZoneGuidedActionWarning { id: warning; confirmDialog: root.dialog }
        }
    )");
    QVERIFY2(root, qPrintable(harness.error));
    QObject *const warning = root->property("warning").value<QObject*>();
    QObject *const dialog = root->property("dialog").value<QObject*>();
    QVERIFY(warning && dialog);
    const auto text = [warning]() { return warning->property("text").toString(); };
    constexpr int actionChangeAlt = 7;
    constexpr int actionPause = 17;
    constexpr int actionChangeLoiterRadius = 33;

    // Hold applies the altitude of the slider like Change Altitude, so it is checked like it, as the slider moves
    QVERIFY(root->setProperty("sliderValue", 100.0));
    for (const int action : { actionChangeAlt, actionPause }) {
        QVERIFY(root->setProperty("sliderValue", 100.0));
        QVERIFY(dialog->setProperty("action", action));
        QVERIFY2(text().contains(QStringLiteral("ABOVE")), qPrintable(QStringLiteral("action %1: %2").arg(action).arg(text())));
        QVERIFY(root->setProperty("sliderValue", 20.0));
        QVERIFY2(text().isEmpty(), qPrintable(text()));
    }

    // The loiter of a go to in forward flight is checked with its radius, as it is changed
    QVERIFY(root->setProperty("loiterCenter", QVariant::fromValue(home.atDistanceAndAzimuth(1000, 90))));
    QVERIFY(root->setProperty("loiterRadius", 100.0));
    QVERIFY(dialog->setProperty("action", actionChangeLoiterRadius));
    QVERIFY2(text().isEmpty(), qPrintable(text()));
    QVERIFY(root->setProperty("loiterRadius", 300.0));
    QVERIFY2(text().contains(QStringLiteral("EAST")), qPrintable(text()));
    // ... and nothing warns while the confirmation is hidden
    QVERIFY(dialog->setProperty("visible", false));
    QVERIFY(text().isEmpty());

    harness.object.reset();
    _disconnectMockLink();
}

void GeoZoneFlightTest::_testAltitudeStrip()
{
    // The strip of a vehicle at 400 m, 100 m above ground, given by a stand-in for the monitor
    QmlHarness harness;
    QObject *const root = harness.create(R"(
        import QtQuick
        import QGroundControl.GeoZone
        Item {
            id: root
            width: 400
            height: 600
            property alias strip: strip
            property var vehicle: QtObject {
                property var verticalBands: []
                property real altitudeAMSL: 400
                property real terrainAMSL: 300
                property bool positionUnavailable: false
                property bool altitudeDegraded: false
            }
            property var fake: QtObject {
                property bool monitoring: true
                property int activeLevel: -1
                property var activeVehicle: root.vehicle
            }
            GeoZoneAltitudeStrip { id: strip; height: 400; monitor: root.fake }
        }
    )");
    QVERIFY2(root, qPrintable(harness.error));
    QQuickItem *const strip = root->property("strip").value<QQuickItem*>();
    QObject *const vehicle = root->property("vehicle").value<QObject*>();
    QVERIFY(strip && vehicle);
    const auto band = [](GeoZone::Restriction restriction, const QString &name, double floor, double ceiling) {
        return QVariantMap{ { QStringLiteral("restriction"), static_cast<int>(restriction) }, { QStringLiteral("severity"), GeoZone::restrictionSeverity(restriction) },
                            { QStringLiteral("name"), name }, { QStringLiteral("limitsText"), QStringLiteral("limits") },
                            { QStringLiteral("floor"), floor }, { QStringLiteral("ceiling"), ceiling } };
    };
    // The items of the bands, which have their label position, in the order they are drawn
    const auto bandItems = [strip]() {
        QList<QPointer<QQuickItem>> items;
        QList<QQuickItem*> pending = { strip };
        while (!pending.isEmpty()) {
            QQuickItem *const item = pending.takeFirst();
            if (item->property("_labelY").isValid() && item->property("index").isValid()) {
                items.append(item);
            }
            pending.append(item->childItems());
        }
        std::sort(items.begin(), items.end(), [](const QPointer<QQuickItem> &a, const QPointer<QQuickItem> &b) {
            return a->property("index").toInt() < b->property("index").toInt();
        });
        return items;
    };

    // The vehicle is in a zone without restriction and in U-space airspace, which is the more severe: on top and named
    const auto makeBands = [&band]() {
        return QVariantList{ band(GeoZone::NoRestriction, QStringLiteral("N"), qQNaN(), 450), band(GeoZone::USpace, QStringLiteral("U"), 350, 900) };
    };
    const QVariantList bands = makeBands();
    QVERIFY(vehicle->setProperty("verticalBands", bands));
    QVERIFY(strip->isVisible());
    const QVariantMap inside = strip->property("_insideBand").toMap();
    QCOMPARE(inside.value(QStringLiteral("names")).toStringList(), QStringList{ QStringLiteral("U") });
    QList<QPointer<QQuickItem>> items = bandItems();
    QCOMPARE(items.count(), 2);

    // The items are kept while the vehicle climbs, which gives the strip a new list of the same bands with each step
    QSignalSpy mergedSpy(strip, SIGNAL(_mergedChanged()));
    QVERIFY(vehicle->setProperty("altitudeAMSL", 400.5));
    QVERIFY(vehicle->setProperty("verticalBands", makeBands()));
    QCOMPARE(mergedSpy.count(), 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    QVERIFY(std::all_of(items.cbegin(), items.cend(), [](const QPointer<QQuickItem> &item) { return !item.isNull(); }));
    QCOMPARE(bandItems().count(), 2);

    // A band wholly above the shown range isn't drawn, and its edges stay within the plot
    const double plotTop = strip->property("_plotTop").toDouble();
    const double plotBottom = plotTop + strip->property("_plotHeight").toDouble();
    QVERIFY(plotBottom > plotTop);
    const QVariantMap high = band(GeoZone::Prohibited, QStringLiteral("HIGH"), 5000, 6000);
    const QVariantMap low = band(GeoZone::Prohibited, QStringLiteral("LOW"), 100, 200);
    for (const QVariantMap &outside : { high, low }) {
        for (const char *const function : { "_floorY", "_ceilingY" }) {
            const double y = QmlHarness::call(strip, function, { outside }).toDouble();
            QVERIFY2((y >= plotTop) && (y <= plotBottom), qPrintable(QStringLiteral("%1 of %2: %3 outside %4..%5").arg(QLatin1String(function), outside.value(QStringLiteral("name")).toString()).arg(y).arg(plotTop).arg(plotBottom)));
        }
        QCOMPARE(QmlHarness::call(strip, "_inRange", { outside }), QVariant(false));
    }
    QCOMPARE(QmlHarness::call(strip, "_inRange", { bands.constLast() }), QVariant(true));
    QVERIFY(vehicle->setProperty("verticalBands", QVariantList{ high, bands.constLast() }));
    items = bandItems();
    QCOMPARE(items.count(), 2);
    // The most severe is the last, on top
    QVERIFY(items.constFirst() && items.constLast());
    QVERIFY(items.constFirst()->isVisible());
    QVERIFY(!items.constLast()->isVisible());
}

void GeoZoneFlightTest::_testPreflightPermits()
{
    using Check = GeoZonePreflightCheck;
    const ScopedEnvironment environment;
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    const QGeoCoordinate center(47, 8);
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center, 300),
    }), errorString), qPrintable(errorString));
    QCOMPARE(Check::permitItem({}).state, Check::NotApplicable);

    // A mission through the zone needs a permit for the flight, which the checklist acknowledges
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    for (const QGeoCoordinate &coordinate : { center.atDistanceAndAzimuth(1000, 270), center.atDistanceAndAzimuth(1000, 90) }) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
        QVERIFY(item);
        item->setAltitudeMode(QGroundControlQmlGlobal::AltitudeModeAbsolute);
        item->altitude()->setRawValue(500);
    }
    GeoZoneMonitor monitor(&manager);
    Check check(&manager);
    check.setMonitor(&monitor);
    check.setPlanController(&masterController);
    QTRY_COMPARE(check.permit().state, Check::Warning);
    QCOMPARE(check.permit().text, QStringLiteral("Acknowledge your permission to enter A for this flight."));
    QCOMPARE(check.permitZones().count(), 1);
    const QVariantMap row = check.permitZones().constFirst().toMap();
    QCOMPARE(row.value(QStringLiteral("name")).toString(), QStringLiteral("A"));
    QVERIFY(!row.value(QStringLiteral("atVehicle")).toBool());
    QVERIFY(!row.value(QStringLiteral("permitted")).toBool());
    QVERIFY(monitor.permits()->acknowledge(row.value(QStringLiteral("zone")), GeoZonePermits::Checklist));
    QTRY_COMPARE(check.permit().state, Check::Passed);
    QCOMPARE(check.permit().text, QStringLiteral("Permits acknowledged for this flight: A."));
    QVERIFY(check.permitZones().constFirst().toMap().value(QStringLiteral("permitted")).toBool());

    // An authorised zone needs none
    QVERIFY(manager.planning()->authorisations()->authorise(row.value(QStringLiteral("zone")), QStringLiteral("A-1"), QString(), QString()).isEmpty());
    QTRY_COMPARE(check.permit().state, Check::NotApplicable);
    manager.repository()->clear();
}

void GeoZoneFlightTest::_testPreflightCheckUpdates()
{
    using Check = GeoZonePreflightCheck;
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(10, 0)));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    const QGeoCoordinate center(47, 8);
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center, 300),
    }), errorString), qPrintable(errorString));
    const auto addMission = [&center](PlanMasterController &masterController) {
        MissionController *const missionController = masterController.missionController();
        for (const QGeoCoordinate &coordinate : { center.atDistanceAndAzimuth(1000, 270), center.atDistanceAndAzimuth(1000, 90) }) {
            SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
            if (item) {
                item->setAltitudeMode(QGroundControlQmlGlobal::AltitudeModeAbsolute);
                item->altitude()->setRawValue(500);
            }
        }
    };
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    addMission(masterController);
    QCOMPARE(masterController.missionController()->visualItems()->count(), 3);
    GeoZoneMonitor monitor(&manager);
    Check check(&manager);
    check.setMonitor(&monitor);
    check.setPlanController(&masterController);
    QTRY_COMPARE(check.permitZones().count(), 1);

    // The zones listed for their permits aren't kept once they are about to be deleted
    QSignalSpy itemsSpy(&check, &Check::itemsChanged);
    manager.repository()->clear();
    QVERIFY(itemsSpy.count() >= 1);
    QVERIFY(check.permitZones().isEmpty());
    QTRY_COMPARE(check.permit().state, Check::NotApplicable);

    // The mission item names the planned start, also when it changes without changing the zones entered
    QTRY_COMPARE(check.mission().state, Check::Passed);
    QVERIFY2(check.mission().text.contains(QStringLiteral("at any time, without a planned start")), qPrintable(check.mission().text));
    manager.planning()->setPlannedStart(environment.now().addSecs(3600));
    QTRY_VERIFY2_WITH_TIMEOUT(check.mission().text.contains(QStringLiteral("for the planned start")), qPrintable(check.mission().text), 5000);
    manager.planning()->setPlannedStart(QDateTime());
    QTRY_VERIFY(check.mission().text.contains(QStringLiteral("at any time, without a planned start")));

    // Another plan and another monitor replace the ones before, which no longer update the check
    PlanMasterController otherController;
    otherController.setFlyView(false);
    otherController.start();
    GeoZoneMonitor otherMonitor(&manager);
    check.setPlanController(&otherController);
    check.setMonitor(&otherMonitor);
    QTRY_COMPARE(check.mission().state, Check::NotApplicable);
    itemsSpy.clear();
    masterController.missionController()->removeAll();
    QTest::qWait(600);
    QCOMPARE(itemsSpy.count(), 0);
    QCOMPARE(check.mission().state, Check::NotApplicable);
    check.setPlanController(&masterController);
    addMission(masterController);
    QTRY_COMPARE(check.mission().state, Check::Passed);
    check.setPlanController(nullptr);
    check.setMonitor(nullptr);
}

void GeoZoneFlightTest::_testChecksFollowSchedules()
{
    using Check = GeoZonePreflightCheck;
    // A prohibited zone and, north of it, a zone limited in flight levels, both from 12:00 to 13:00 UTC and loaded
    // an hour before
    ScopedEnvironment environment;
    const QDate day(2027, 3, 1);
    environment.setNow(_utc(day, QTime(11, 0)));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    Fact *const qnh = GeoZoneSettings::instance()->qnhHPa();
    const auto restore = qScopeGuard([qnh]() { qnh->setRawValue(qnh->rawDefaultValue()); });
    qnh->setRawValue(qnh->rawDefaultValue());
    const QGeoCoordinate center(47, 8);
    const QJsonArray noon{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-03-01T12:00:00Z" }, { "endDateTime", "2027-03-01T13:00:00Z" } } };
    QJsonObject prohibited = _circleFeature(QStringLiteral("S"), QStringLiteral("PROHIBITED"), center, 300);
    prohibited[QStringLiteral("applicability")] = noon;
    QJsonObject levels = _withLimits(_circleFeature(QStringLiteral("L"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(800, 0), 200),
                                     400, QStringLiteral("STD"), 700, QStringLiteral("STD"));
    levels[QStringLiteral("applicability")] = noon;
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ prohibited, levels }), errorString), qPrintable(errorString));
    const GeoZone *const zone = findZone(manager, QStringLiteral("S"));
    const GeoZone *const levelZone = findZone(manager, QStringLiteral("L"));
    QVERIFY(zone && levelZone);
    QVERIFY(!zone->active());

    // A mission west to east through the prohibited zone at 500 m above mean sea level, and a guided command along it
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    QList<GeoZone::Position> path;
    for (const QGeoCoordinate &coordinate : { center.atDistanceAndAzimuth(1000, 270), center.atDistanceAndAzimuth(1000, 90) }) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
        QVERIFY(item);
        item->setAltitudeMode(QGroundControlQmlGlobal::AltitudeModeAbsolute);
        item->altitude()->setRawValue(500);
        path.append(GeoZone::Position{ coordinate, 500 });
    }
    GeoZoneMonitor monitor(&manager);
    const GeoZoneGuidedCheck *const guided = monitor.guidedCheck();
    Check check(&manager);
    check.setPlanController(&masterController);

    // An hour before the zones begin neither a guided command nor the mission started now enters them, and no flight
    // level limit applies. Without a planned start the mission may be flown at any time, so its zones count.
    QVERIFY2(guided->pathWarning(path).isEmpty(), qPrintable(guided->pathWarning(path)));
    QVERIFY2(guided->startWarning(missionController).isEmpty(), qPrintable(guided->startWarning(missionController)));
    QTRY_COMPARE(check.mission().state, Check::Failed);
    QCOMPARE(check.qnh().state, Check::NotApplicable);
    // The upload of the mission asks about them
    const auto uploadAsksAboutZone = [&check]() { return check.missionCheck()->uploadReasons().join(QChar(u'\n')).contains(QStringLiteral("prohibited geozones: S.")); };
    QVERIFY(uploadAsksAboutZone());

    // The zones begin. The manager only evaluates the schedules of the zones away from the vehicles once a minute: the
    // checks of what the pilot does now don't wait for it.
    environment.setNow(_utc(day, QTime(12, 0, 1)));
    QVERIFY(!zone->active());
    QVERIFY2(guided->pathWarning(path).contains(QStringLiteral("S (Prohibited)")), qPrintable(guided->pathWarning(path)));
    QVERIFY2(guided->startWarning(missionController).contains(QStringLiteral("S (Prohibited)")), qPrintable(guided->startWarning(missionController)));
    QVERIFY(!zone->active());
    // The checklist follows the zones becoming active, here the prohibited one only, with the schedules of the others
    QCOMPARE(check.qnh().state, Check::NotApplicable);
    QVERIFY(manager.updateActive(GeoZoneBox(QGeoRectangle(center.atDistanceAndAzimuth(100, 315), center.atDistanceAndAzimuth(100, 135)))));
    QVERIFY(zone->active());
    QVERIFY(!levelZone->active());
    QTRY_COMPARE(check.qnh().state, Check::Warning);
    QVERIFY2(check.qnh().text.startsWith(QStringLiteral("Flight level limits of L")), qPrintable(check.qnh().text));

    // The zones ended, beyond the tolerance of the times of a mission: they no longer count, whatever their state
    environment.setNow(_utc(day, QTime(13, 30)));
    QVERIFY(zone->active());
    QVERIFY2(guided->pathWarning(path).isEmpty(), qPrintable(guided->pathWarning(path)));
    QVERIFY2(guided->startWarning(missionController).isEmpty(), qPrintable(guided->startWarning(missionController)));
    check.update();
    QCOMPARE(check.qnh().state, Check::NotApplicable);

    // With a planned start the mission is checked for the time of the flight, whatever the zones are now
    QCOMPARE(check.mission().state, Check::Failed);
    manager.planning()->setPlannedStart(_utc(day, QTime(15, 0)));
    QTRY_COMPARE(check.mission().state, Check::Passed);
    QVERIFY(!uploadAsksAboutZone());
    manager.planning()->setPlannedStart(_utc(day, QTime(12, 10)));
    QTRY_COMPARE(check.mission().state, Check::Failed);
    QVERIFY(uploadAsksAboutZone());
    check.setPlanController(nullptr);
}

void GeoZoneFlightTest::_testArduPilotTakeoff()
{
    const ScopedEnvironment environment;
    GeoZoneManager *const manager = GeoZoneManager::instance();
    QString errorString;
    QVERIFY2(manager->repository()->loadFile(kZonesFile, errorString), qPrintable(errorString));
    GeoZoneSettings::instance()->audioAlerts()->setRawValue(false);
    GeoZoneMonitor monitor;
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);

    // The MockLink ArduCopter starts in Stabilize with the modes of the firmware plugin
    _connectMockLink(MAV_AUTOPILOT_ARDUPILOTMEGA);
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->parameterManager()->parametersReady(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->coordinate().isValid() && !std::isnan(vehicle->altitudeAMSL()->rawValue().toDouble()), 30000);
    QTRY_COMPARE(vehicle->flightMode(), QStringLiteral("Stabilize"));
    QVERIFY(vehicle->flightModes().contains(QStringLiteral("Loiter")));

    // A guided takeoff switches to Guided with MAV_CMD_DO_SET_MODE, arms and climbs into the CTRs, which are alerted
    _mockLink->clearReceivedMavCommandCounts();
    vehicle->guidedModeTakeoff(300);
    QCOMPARE(_mockLink->receivedMavCommandCount(MAV_CMD_DO_SET_MODE), 1);
    QTRY_COMPARE(vehicle->flightMode(), QStringLiteral("Guided"));
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->armed(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(vehicle->altitudeRelative()->rawValue().toInt(), 300, 30000);
    // The monitor evaluates every second: under load its timer and the position messages come late
    if (!QTest::qWaitFor([&monitor]() { return monitor.activeMonitor() && monitor.activeMonitor()->inside(); }, 20000)) {
        const GeoZoneVehicleMonitor &active = ACTIVE(monitor);
        const GeoZone::Position &position = active.position();
        QFAIL(qPrintable(QStringLiteral("Not inside: unavailable %1 position %2 amsl %3 terrain %4 source %5 nearby %6 zones %7 | vehicle %8 amsl %9 rel %10 armed %11 mode %12 | alerts %13")
                             .arg(active.positionUnavailable()).arg(position.coordinate.toString(QGeoCoordinate::Degrees)).arg(position.altitudeAMSL).arg(position.terrainAMSL)
                             .arg(static_cast<int>(active.terrainSource())).arg(active.nearby().count()).arg(manager->zoneCount())
                             .arg(vehicle->coordinate().toString(QGeoCoordinate::Degrees)).arg(vehicle->altitudeAMSL()->rawValue().toDouble())
                             .arg(vehicle->altitudeRelative()->rawValue().toDouble()).arg(vehicle->armed()).arg(vehicle->flightMode())
                             .arg(spiedAlerts(alertSpy).join(QStringLiteral(" / ")))));
    }
    QTRY_COMPARE_WITH_TIMEOUT(spiedAlerts(alertSpy).filter(QStringLiteral("Entered geozone CTR")).count(), 2, 20000);

    // The pilot changes modes like with the flight mode menu
    vehicle->setFlightMode(QStringLiteral("Loiter"));
    QTRY_COMPARE(vehicle->flightMode(), QStringLiteral("Loiter"));
    QVERIFY(vehicle->armed());
    _disconnectMockLink();
}

GEOZONE_TEST(GeoZoneFlightTest)
