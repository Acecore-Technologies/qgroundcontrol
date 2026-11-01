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
/// units, the terrain, the air pressure and the handling of downloads. The application sets its hooks once at startup, before zones are
/// loaded. Hooks left empty fall back to defaults suited to tools and headless tests.
namespace GeoZoneEnvironment {

/// Sea level pressure of the ICAO standard atmosphere in hPa, the datum of flight levels
inline constexpr double kStandardPressureHPa = 1013.25;
/// Range of QNH values accepted from the application, beyond the lowest and highest sea level pressures recorded
inline constexpr double kMinQnhHPa = 850;
inline constexpr double kMaxQnhHPa = 1100;

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
    std::function<double()> qnhHPa;                                             ///< Local QNH converting flight levels, called from any thread
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
/// @return Local QNH in hPa, the sea level pressure which flight levels are converted to heights above mean sea level
///         with. kStandardPressureHPa when the hook is empty or returns a value out of the accepted range.
double qnhHPa();
void prepareDownload(QNetworkReply &reply);

} // namespace GeoZoneEnvironment
