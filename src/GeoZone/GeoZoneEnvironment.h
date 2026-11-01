/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>

#include <functional>

class QNetworkReply;

/// What the geozone core needs from the application hosting it: the clock, the settings of the sources, the user's
/// units, the terrain and the handling of downloads. The application sets its hooks once at startup, before zones are
/// loaded. Hooks left empty fall back to defaults suited to tools and headless tests.
namespace GeoZoneEnvironment {

/// When the data of sources is outdated and when feeds are downloaded again
struct SourceSettings {
    int feedRefreshHours = 24;
    int liveFeedRefreshMinutes = 15;    ///< Feeds of temporary restrictions, see GeoZoneSource::isLive
    int staleDays = 28;
    int liveStaleHours = 2;
};

struct Hooks {
    std::function<QDateTime()> currentDateTimeUtc;                              ///< The system clock when empty
    std::function<SourceSettings()> sourceSettings;                             ///< The SourceSettings defaults when empty
    std::function<QString()> verticalDistanceUnits;                             ///< Units of the heights shown, "m" when empty
    std::function<double(double meters)> metersToVerticalDistanceUnits;         ///< Unchanged meters when empty
    std::function<double(const QGeoCoordinate &coordinate)> terrainElevation;  ///< Meters AMSL, NaN when unknown or empty
    std::function<void(QNetworkReply &reply)> prepareDownload;                 ///< Called for each request of GeoZoneDownloader
};

/// Replaces the hooks. Not thread safe: call it while no other thread uses zones.
void setHooks(const Hooks &hooks);
const Hooks &hooks();

QDateTime currentDateTimeUtc();
SourceSettings sourceSettings();
QString verticalDistanceUnits();
double metersToVerticalDistanceUnits(double meters);
/// @return Ground elevation in meters AMSL, NaN when unknown
double terrainElevation(const QGeoCoordinate &coordinate);
void prepareDownload(QNetworkReply &reply);

} // namespace GeoZoneEnvironment
