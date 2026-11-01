/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneModule.h"
#include "FactMetaData.h"
#include "GeoZone.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneBriefingExport.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneFenceImporter.h"
#include "GeoZoneFenceTags.h"
#include "GeoZoneGuidedCheck.h"
#include "GeoZoneManager.h"
#include "GeoZoneMapLayer.h"
#include "GeoZoneMapModel.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMonitor.h"
#include "GeoZoneParser.h"
#include "GeoZoneProfile.h"
#include "GeoZoneRoutePlanner.h"
#include "GeoZoneSettings.h"
#include "GeoZoneSource.h"
#include "QGCApplication.h"
#include "QGCFileDownload.h"
#include "QGCLoggingCategory.h"
#include "TerrainQuery.h"

#include <QtQml/QQmlEngine>

#include <atomic>

namespace GeoZoneModule {

namespace {

constexpr const char *kUri = "QGroundControl.GeoZone";

// The geozone core logs to plain categories, listed here with those of QGroundControl
const QGCLoggingCategory qgcCategoryGeoZoneAuthorisationsLog(QString::fromLatin1(GeoZoneAuthorisationsLog().categoryName()));
const QGCLoggingCategory qgcCategoryGeoZoneParserLog(QString::fromLatin1(GeoZoneParserLog().categoryName()));
const QGCLoggingCategory qgcCategoryGeoZoneSourceLog(QString::fromLatin1(GeoZoneSourceLog().categoryName()));

void installEnvironment()
{
    // Read by checks on worker threads, so the setting is copied when it changes rather than read from them
    static std::atomic<double> qnhHPa = GeoZoneEnvironment::kStandardPressureHPa;
    Fact *const qnhFact = GeoZoneSettings::instance()->qnhHPa();
    qnhHPa = qnhFact->rawValue().toDouble();
    (void) QObject::connect(qnhFact, &Fact::rawValueChanged, qnhFact, [](const QVariant &value) { qnhHPa = value.toDouble(); });

    GeoZoneEnvironment::Hooks hooks;
    hooks.sourceSettings = []() {
        GeoZoneSettings *const settings = GeoZoneSettings::instance();
        GeoZoneEnvironment::SourceSettings sourceSettings;
        sourceSettings.feedRefreshHours = settings->feedRefreshHours()->rawValue().toInt();
        sourceSettings.liveFeedRefreshMinutes = settings->liveFeedRefreshMinutes()->rawValue().toInt();
        sourceSettings.staleDays = settings->staleDays()->rawValue().toInt();
        sourceSettings.liveStaleHours = settings->liveStaleHours()->rawValue().toInt();
        return sourceSettings;
    };
    hooks.verticalDistanceUnits = []() { return FactMetaData::appSettingsVerticalDistanceUnitsString(); };
    hooks.metersToVerticalDistanceUnits = [](double meters) { return FactMetaData::metersToAppSettingsVerticalDistanceUnits(meters).toDouble(); };
    hooks.terrainElevation = [](const QGeoCoordinate &coordinate) {
        QList<double> altitudes;
        bool error = false;
        if (!TerrainAtCoordinateQuery::getAltitudesForCoordinates({ coordinate }, altitudes, error) || error || altitudes.isEmpty()) {
            return qQNaN();
        }
        return altitudes.constFirst();
    };
    hooks.qnhHPa = []() { return qnhHPa.load(); };
    hooks.prepareDownload = [](QNetworkReply &reply) { QGCFileDownload::setIgnoreSSLErrorsIfNeeded(reply); };
    GeoZoneEnvironment::setHooks(hooks);
}

template<typename T>
QObject *singleton(T *instance)
{
    QQmlEngine::setObjectOwnership(instance, QQmlEngine::CppOwnership);
    return instance;
}

} // namespace

void registerQmlTypes()
{
    installEnvironment();

    (void) qmlRegisterUncreatableMetaObject(GeoZone::staticMetaObject, kUri, 1, 0, "GeoZone", "Enums only");
    (void) qmlRegisterSingletonType<GeoZoneManager>(kUri, 1, 0, "GeoZoneManager", [](QQmlEngine*, QJSEngine*) { return singleton(GeoZoneManager::instance()); });
    (void) qmlRegisterSingletonType<GeoZoneMonitor>(kUri, 1, 0, "GeoZoneMonitor", [](QQmlEngine*, QJSEngine*) { return singleton(GeoZoneMonitor::instance()); });
    (void) qmlRegisterSingletonType<GeoZoneFenceTags>(kUri, 1, 0, "GeoZoneFenceTags", [](QQmlEngine*, QJSEngine*) -> QObject* { return new GeoZoneFenceTags(); });
    (void) qmlRegisterUncreatableType<GeoZoneFenceTag>(kUri, 1, 0, "GeoZoneFenceTag", "Reference only");
    (void) qmlRegisterUncreatableType<GeoZoneGuidedCheck>(kUri, 1, 0, "GeoZoneGuidedCheck", "Reference only");
    (void) qmlRegisterUncreatableType<GeoZoneAuthorisations>(kUri, 1, 0, "GeoZoneAuthorisations", "Reference only");
    (void) qmlRegisterUncreatableType<GeoZoneSettings>(kUri, 1, 0, "GeoZoneSettings", "Reference only");
    (void) qmlRegisterType<GeoZoneMapModel>(kUri, 1, 0, "GeoZoneMapModel");
    (void) qmlRegisterType<GeoZoneMapLayer>(kUri, 1, 0, "GeoZoneMapLayer");
    (void) qmlRegisterType<GeoZoneMissionCheck>(kUri, 1, 0, "GeoZoneMissionCheck");
    (void) qmlRegisterType<GeoZoneBriefingExport>(kUri, 1, 0, "GeoZoneBriefingExport");
    (void) qmlRegisterType<GeoZoneFenceImporter>(kUri, 1, 0, "GeoZoneFenceImporter");
    (void) qmlRegisterType<GeoZoneProfile>(kUri, 1, 0, "GeoZoneProfile");
    (void) qmlRegisterType<GeoZoneRoutePlanner>(kUri, 1, 0, "GeoZoneRoutePlanner");
}

void init()
{
    GeoZoneManager *const manager = GeoZoneManager::instance();
    QGCApplication *const app = qgcApp();
    (void) QObject::connect(manager, &GeoZoneManager::loadFailed, app, [app](const QString &, const QString &message) { app->showAppMessage(message); });
    (void) QObject::connect(manager, &GeoZoneManager::storageFailed, app, [app](const QString &message) { app->showAppMessage(message, QCoreApplication::translate("GeoZoneModule", "Geozones")); });
    (void) QObject::connect(manager, &GeoZoneManager::outdatedDataFound, app, [app](const QString &message) { app->showAppMessage(message, QCoreApplication::translate("GeoZoneModule", "Geozones")); });
    manager->init();
    (void) GeoZoneMonitor::instance();
}

} // namespace GeoZoneModule
