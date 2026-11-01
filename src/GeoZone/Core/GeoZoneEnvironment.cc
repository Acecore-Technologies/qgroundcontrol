/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneEnvironment.h"

#include <QtCore/QtNumeric>

namespace GeoZoneEnvironment {

namespace {

Hooks &installedHooks()
{
    static Hooks hooks;
    return hooks;
}

} // namespace

void setHooks(const Hooks &hooks)
{
    installedHooks() = hooks;
}

const Hooks &hooks()
{
    return installedHooks();
}

QDateTime currentDateTimeUtc()
{
    return hooks().currentDateTimeUtc ? hooks().currentDateTimeUtc() : QDateTime::currentDateTimeUtc();
}

SourceSettings sourceSettings()
{
    return hooks().sourceSettings ? hooks().sourceSettings() : SourceSettings();
}

QString verticalDistanceUnits()
{
    return hooks().verticalDistanceUnits ? hooks().verticalDistanceUnits() : QStringLiteral("m");
}

double metersToVerticalDistanceUnits(double meters)
{
    return hooks().metersToVerticalDistanceUnits ? hooks().metersToVerticalDistanceUnits(meters) : meters;
}

double terrainElevation(const QGeoCoordinate &coordinate)
{
    return hooks().terrainElevation ? hooks().terrainElevation(coordinate) : qQNaN();
}

void prepareDownload(QNetworkReply &reply)
{
    if (hooks().prepareDownload) {
        hooks().prepareDownload(reply);
    }
}

} // namespace GeoZoneEnvironment
