/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneModule.h"
#include "GeoZone.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneFenceImporter.h"
#include "GeoZoneGuidedCheck.h"
#include "GeoZoneManager.h"
#include "GeoZoneMapLayer.h"
#include "GeoZoneMapModel.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMonitor.h"
#include "GeoZoneProfile.h"
#include "GeoZoneRoutePlanner.h"
#include "GeoZoneSettings.h"
#include "QGCApplication.h"

#include <QtQml/QQmlEngine>

namespace GeoZoneModule {

namespace {

constexpr const char *kUri = "QGroundControl.GeoZone";

template<typename T>
QObject *singleton(T *instance)
{
    QQmlEngine::setObjectOwnership(instance, QQmlEngine::CppOwnership);
    return instance;
}

} // namespace

void registerQmlTypes()
{
    (void) qmlRegisterUncreatableMetaObject(GeoZone::staticMetaObject, kUri, 1, 0, "GeoZone", "Enums only");
    (void) qmlRegisterSingletonType<GeoZoneManager>(kUri, 1, 0, "GeoZoneManager", [](QQmlEngine*, QJSEngine*) { return singleton(GeoZoneManager::instance()); });
    (void) qmlRegisterSingletonType<GeoZoneMonitor>(kUri, 1, 0, "GeoZoneMonitor", [](QQmlEngine*, QJSEngine*) { return singleton(GeoZoneMonitor::instance()); });
    (void) qmlRegisterUncreatableType<GeoZoneGuidedCheck>(kUri, 1, 0, "GeoZoneGuidedCheck", "Reference only");
    (void) qmlRegisterUncreatableType<GeoZoneAuthorisations>(kUri, 1, 0, "GeoZoneAuthorisations", "Reference only");
    (void) qmlRegisterUncreatableType<GeoZoneSettings>(kUri, 1, 0, "GeoZoneSettings", "Reference only");
    (void) qmlRegisterType<GeoZoneMapModel>(kUri, 1, 0, "GeoZoneMapModel");
    (void) qmlRegisterType<GeoZoneMapLayer>(kUri, 1, 0, "GeoZoneMapLayer");
    (void) qmlRegisterType<GeoZoneMissionCheck>(kUri, 1, 0, "GeoZoneMissionCheck");
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
