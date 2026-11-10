/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneSettings.h"

#include <QtCore/qapplicationstatic.h>
#include <QtCore/QSettings>
#include <QtQml/QQmlEngine>

Q_APPLICATION_STATIC(GeoZoneSettings, _geoZoneSettings);

GeoZoneSettings *GeoZoneSettings::instance()
{
    return _geoZoneSettings();
}

// Same group as earlier versions, which kept the map visibility and the name of a single stored file
DECLARE_SETTINGGROUP(GeoZone, "GeoZoneManager")
{

    QSettings settings;
    settings.beginGroup(settingsGroup);
    static constexpr const char *legacyVisibleKey = "visible";
    if (settings.contains(legacyVisibleKey)) {
        if (!settings.contains(showZonesName)) {
            settings.setValue(showZonesName, settings.value(legacyVisibleKey));
        }
        settings.remove(legacyVisibleKey);
    }

    // The approach distance and time the pilot could set freely, now the manufacturer's thresholds which the pilot can
    // only widen: what was set above the former defaults is kept as extra
    struct Legacy {
        const char *key;
        const char *extraKey;
        double formerDefault;
    };
    for (const Legacy &legacy : { Legacy{ "approachDistance", approachExtraDistanceName, 100 }, Legacy{ "approachTime", approachExtraTimeName, 30 } }) {
        if (!settings.contains(legacy.key)) {
            continue;
        }
        const double extra = settings.value(legacy.key).toDouble() - legacy.formerDefault;
        if ((extra > 0) && !settings.contains(legacy.extraKey)) {
            settings.setValue(legacy.extraKey, extra);
        }
        settings.remove(legacy.key);
    }

    // Flight alerts and their log are always on, EN 4709-003 requires the warnings: stored switches are dropped
    for (const char *key : { "alertsEnabled", "logAlerts" }) {
        settings.remove(key);
    }
}

DECLARE_SETTINGSFACT(GeoZoneSettings, showZones)
DECLARE_SETTINGSFACT(GeoZoneSettings, hiddenRestrictions)
DECLARE_SETTINGSFACT(GeoZoneSettings, altitudeFilter)
DECLARE_SETTINGSFACT(GeoZoneSettings, scheduleFilter)
DECLARE_SETTINGSFACT(GeoZoneSettings, mapSource)
DECLARE_SETTINGSFACT(GeoZoneSettings, audioAlerts)
DECLARE_SETTINGSFACT(GeoZoneSettings, recordRetention)
DECLARE_SETTINGSFACT(GeoZoneSettings, includeOperatorDetails)
DECLARE_SETTINGSFACT(GeoZoneSettings, approachExtraDistance)
DECLARE_SETTINGSFACT(GeoZoneSettings, approachExtraTime)
DECLARE_SETTINGSFACT(GeoZoneSettings, trafficEnabled)
DECLARE_SETTINGSFACT(GeoZoneSettings, qnhHPa)
DECLARE_SETTINGSFACT(GeoZoneSettings, feedRefreshHours)
DECLARE_SETTINGSFACT(GeoZoneSettings, feedCatalogueUrl)
DECLARE_SETTINGSFACT(GeoZoneSettings, liveFeedRefreshMinutes)
DECLARE_SETTINGSFACT(GeoZoneSettings, staleDays)
DECLARE_SETTINGSFACT(GeoZoneSettings, outdatedInvalid)
DECLARE_SETTINGSFACT(GeoZoneSettings, followAreaOfInterest)
DECLARE_SETTINGSFACT(GeoZoneSettings, followMapView)
DECLARE_SETTINGSFACT(GeoZoneSettings, packSigningKeyFile)
DECLARE_SETTINGSFACT(GeoZoneSettings, packPublisherName)
DECLARE_SETTINGSFACT(GeoZoneSettings, trustedPackPublishers)
