/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneSettings.h"

#include <QtCore/QSettings>
#include <QtQml/QQmlEngine>

// Same group as earlier versions, which kept the map visibility and the name of a single stored file
DECLARE_SETTINGGROUP(GeoZone, "GeoZoneManager")
{
    qmlRegisterUncreatableType<GeoZoneSettings>("QGroundControl.SettingsManager", 1, 0, "GeoZoneSettings", "Reference only");

    QSettings settings;
    settings.beginGroup(settingsGroup);
    static constexpr const char *legacyVisibleKey = "visible";
    if (settings.contains(legacyVisibleKey)) {
        if (!settings.contains(showZonesName)) {
            settings.setValue(showZonesName, settings.value(legacyVisibleKey));
        }
        settings.remove(legacyVisibleKey);
    }
}

DECLARE_SETTINGSFACT(GeoZoneSettings, showZones)
DECLARE_SETTINGSFACT(GeoZoneSettings, alertsEnabled)
DECLARE_SETTINGSFACT(GeoZoneSettings, audioAlerts)
DECLARE_SETTINGSFACT(GeoZoneSettings, approachDistance)
DECLARE_SETTINGSFACT(GeoZoneSettings, approachVerticalDistance)
DECLARE_SETTINGSFACT(GeoZoneSettings, feedRefreshHours)
DECLARE_SETTINGSFACT(GeoZoneSettings, liveFeedRefreshMinutes)
DECLARE_SETTINGSFACT(GeoZoneSettings, staleDays)
DECLARE_SETTINGSFACT(GeoZoneSettings, liveStaleHours)
