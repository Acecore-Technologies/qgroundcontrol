/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "SettingsGroup.h"

/// Settings of the geozone module, kept by the module rather than SettingsManager
class GeoZoneSettings : public SettingsGroup
{
    Q_OBJECT
public:
    GeoZoneSettings(QObject* parent = nullptr);
    DEFINE_SETTING_NAME_GROUP()

    static GeoZoneSettings *instance();

    DEFINE_SETTINGFACT(showZones)
    DEFINE_SETTINGFACT(hiddenRestrictions)
    DEFINE_SETTINGFACT(altitudeFilter)
    DEFINE_SETTINGFACT(altitudeFilterMargin)
    DEFINE_SETTINGFACT(scheduleFilter)
    DEFINE_SETTINGFACT(scheduleFilterHours)
    DEFINE_SETTINGFACT(mapSource)
    DEFINE_SETTINGFACT(alertsEnabled)
    DEFINE_SETTINGFACT(audioAlerts)
    DEFINE_SETTINGFACT(logAlerts)
    DEFINE_SETTINGFACT(alertLogRetentionDays)
    DEFINE_SETTINGFACT(approachDistance)
    DEFINE_SETTINGFACT(approachVerticalDistance)
    DEFINE_SETTINGFACT(approachTime)
    DEFINE_SETTINGFACT(trafficEnabled)
    DEFINE_SETTINGFACT(trafficRadius)
    DEFINE_SETTINGFACT(trafficAlerts)
    DEFINE_SETTINGFACT(qnhHPa)
    DEFINE_SETTINGFACT(feedRefreshHours)
    DEFINE_SETTINGFACT(liveFeedRefreshMinutes)
    DEFINE_SETTINGFACT(staleDays)
    DEFINE_SETTINGFACT(liveStaleHours)
    DEFINE_SETTINGFACT(followAreaOfInterest)
    DEFINE_SETTINGFACT(areaOfInterestMargin)
    DEFINE_SETTINGFACT(packSigningKeyFile)
    DEFINE_SETTINGFACT(packPublisherName)
    DEFINE_SETTINGFACT(trustedPackPublishers)
    DEFINE_SETTINGFACT(intentHorizontalBuffer)
    DEFINE_SETTINGFACT(intentVerticalBuffer)
    DEFINE_SETTINGFACT(intentTimeBuffer)
    DEFINE_SETTINGFACT(intentMaxVolumes)
};
