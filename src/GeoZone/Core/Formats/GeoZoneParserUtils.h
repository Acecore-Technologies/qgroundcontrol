/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "GeoZone.h"
#include "GeoZoneParser.h"

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include <optional>

/// Conversions shared by the geozone formats
namespace GeoZoneParserUtils {

inline constexpr double kMetersPerFoot = 0.3048;
inline constexpr double kUnlimitedSentinel = 99999.0;   ///< Upper limit of airspaces without a ceiling, as in GeoZone
// Bounds keeping a broken or hostile file from stalling the application
inline constexpr qsizetype kMaxRingVertices = 200000;
inline constexpr qsizetype kMaxVolumesPerZone = 10000;
inline constexpr double kMaxRadiusMeters = 1000000;
inline constexpr double kMaxDurationSecs = 100.0 * 365 * 24 * 3600;    ///< Longer durations, 100 years, are invalid
// Geometry checks of _normalizeVolumes
inline constexpr double kDuplicateVertexMeters = 0.01;      ///< Consecutive vertices closer than this are one
inline constexpr double kMinRingAreaSquareMeters = 1;       ///< Rings with less area are degenerate unless they cross themselves
inline constexpr double kMaxPolygonLatitude = 89.9;         ///< Polygons reaching closer to a pole are rejected, about 11 km

/// Sets the preferred language of localized texts for the parse running on this thread
/// @return Code of the preferred language of the parse running on this thread, like "de", empty for English
const QString &language();

struct LanguageScope {
    explicit LanguageScope(const QString &language);
    ~LanguageScope();
};

/// Sets the cancel token of the parse running on this thread, which formats check between features
struct CancelScope {
    explicit CancelScope(const GeoZoneCancelToken &cancelToken);
    ~CancelScope();
};

/// @return true if the parse running on this thread was canceled
bool cancelled();

/// Returns text from either a plain string or an ED-318 localized text array ([{ "text": "...", "lang": "en-GB" }])
QString _text(const QJsonValue &value);

/// @return The text of HTML markup, which some publishers put in their messages: paragraphs and line breaks become
///         new lines, other tags are dropped and entities are decoded. Text without markup is returned unchanged.
QString _plainText(const QString &text);

QStringList _stringList(const QJsonValue &value);

/// @return The number of a JSON number or string, NaN when it isn't a finite number
double _number(const QJsonValue &value);

/// @return The number the text is, NaN when it isn't a finite number
double _numberFromText(QStringView text);

/// Parses ISO 8601 dates and times. Times without an offset are UTC in both standards, dates start at midnight UTC.
QDateTime _dateTime(const QJsonValue &value);

/// Parses ED-269/ED-318 times of day such as "06:00Z", "05:00:00.00Z", "08:00+02:00" or "08:00". The time is
/// returned as published, utcOffsetSecs is set to its offset from UTC, or to nothing for a local time without offset.
QTime _time(const QJsonValue &value, std::optional<int> &utcOffsetSecs);

/// Reads the applicability of ED-269 and ED-318 zones. Daily times with an offset keep it. Times without offset, local
/// times or UTC since the formats don't tell, get the assumed time zone of the zone's country (see
/// GeoZoneSchedule::countryTimeZone and DailyPeriod::timeZoneAssumed), or are taken as UTC with an issue when it isn't
/// known.
/// @param country ISO 3166-1 code of the zone
/// @param volumes Of the zone, whose location tells the time zone of countries with several
/// @param issues  Problems found, like unknown day codes, are added to it
QList<GeoZone::TimePeriod> _applicability(const QJsonValue &value, const QString &country, const QList<GeoZone::Volume> &volumes, QStringList &issues);

/// Reads a unit of length
/// @return false for an unknown unit, which is added to issues. An empty unit is meters.
bool _lengthUnit(const QJsonValue &value, bool &feet, QStringList &issues);

QStringList _authorities(const QJsonValue &value);

/// Reads the first authority with the NOTIFICATION purpose into the zone's notificationAuthority and
/// notificationLeadSecs, from its name or contact and its intervalBefore
void _notification(const QJsonValue &value, GeoZone::Data &zone);

/// @return Seconds of an ISO 8601 duration like P2D or PT12H, years as 365 days and months as 30, -1 when invalid or
///         longer than kMaxDurationSecs
qint64 _durationSecs(const QString &duration);

bool _position(const QJsonValue &value, QGeoCoordinate &coordinate);

/// Reads the positions of a ring, without the closing one. Their number is checked by _normalizeVolumes.
/// @param hole   A hole of a polygon, which the zone covers when it can't be read
/// @param issues Why the ring can't be read is added to it
/// @return false for rings without positions, with an invalid position or with more than kMaxRingVertices
bool _ring(const QJsonValue &value, GeoZone::Ring &ring, bool hole, QStringList &issues);

/// Reads the rings of a polygon. A hole which can't be read is left out, so that the zone covers it.
/// @param issues The rings which can't be read are added to it
/// @return false when the outer ring can't be read
bool _polygonRings(const QJsonValue &value, GeoZone::Volume &volume, QStringList &issues);

/// Adds the volume, unless the zone has kMaxVolumesPerZone already, which is added to the issues
void _appendVolume(const GeoZone::Volume &volume, QList<GeoZone::Volume> &volumes, QStringList &issues);

bool _validRadius(double radius);

/// Adds points on the great circles of the polygon edges of the volumes, for formats whose edges are geodesics, see
/// GeoZoneGeometry::densifyGreatCircles. Polygon edges are otherwise straight in latitude and longitude, like in
/// GeoJSON (RFC 7946 section 3.1.1), which is how the zone queries take them.
void _densifyGreatCircles(QList<GeoZone::Volume> &volumes);

/// Checks and normalizes the volumes of a parsed zone, adding the problems found to its issues. Consecutive duplicate
/// vertices and the closing vertex are removed. Polygons with fewer than 3 distinct points, without area, or touching
/// or enclosing a pole are dropped, as are circles without a valid radius; invalid holes are dropped, which enlarges
/// the zone. Outer rings are made counterclockwise and holes clockwise. Rings crossing themselves are kept, with the
/// even-odd rule of the containment tests, and reported.
/// @return true if the zone has a volume left
bool _normalizeVolumes(GeoZone::Data &zone);

/// Adds the volumes described by a GeoJSON-like horizontal geometry. Vertical limits are copied from volumeTemplate.
/// What is left out is added to the issues, so that a zone isn't smaller than published without telling: polygons
/// and holes which can't be read, circles without center and geometries of other types. A zone keeps its other
/// polygons, GeoZoneParser reports the one left without any. A geometry without type adds nothing.
void _appendHorizontalGeometry(const QJsonObject &geometry, const GeoZone::Volume &volumeTemplate, double radiusScale, QList<GeoZone::Volume> &volumes,
                               QStringList &issues);

/// Reads the zone properties ED-269 and ED-318 share. The reasons may also be given as "reasons", and the extended
/// properties as text. A message in localizedMessages of the extended properties (Austro Control) in the preferred
/// language or English replaces the message, which ED-269 limits to 200 characters.
void _commonProperties(const QJsonObject &object, const QString &restrictionKey, GeoZone::Data &zone);

} // namespace GeoZoneParserUtils
