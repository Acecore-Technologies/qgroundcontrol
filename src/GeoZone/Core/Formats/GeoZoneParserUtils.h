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

/// Conversions shared by the geozone formats
namespace GeoZoneParserUtils {

inline constexpr double kMetersPerFoot = 0.3048;
inline constexpr double kUnlimitedSentinel = 99999.0;   ///< Upper limit of airspaces without a ceiling, as in GeoZone
// Bounds keeping a broken or hostile file from stalling the application
inline constexpr qsizetype kMaxRingVertices = 200000;
inline constexpr qsizetype kMaxVolumesPerZone = 10000;
inline constexpr double kMaxRadiusMeters = 1000000;
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

QStringList _stringList(const QJsonValue &value);

double _number(const QJsonValue &value);

/// Parses ISO 8601 dates and times. Times without an offset are UTC in both standards, dates start at midnight UTC.
QDateTime _dateTime(const QJsonValue &value);

/// Parses ED-269/ED-318 times of day such as "06:00Z", "05:00:00.00Z" or "08:00+02:00". The time is returned
/// as published, utcOffsetSecs is set to its offset from UTC.
QTime _time(const QJsonValue &value, int &utcOffsetSecs);

/// @param issues Problems found, like unknown day codes, are added to it
QList<GeoZone::TimePeriod> _applicability(const QJsonValue &value, QStringList &issues);

/// Reads a unit of length
/// @return false for an unknown unit, which is added to issues. An empty unit is meters.
bool _lengthUnit(const QJsonValue &value, bool &feet, QStringList &issues);

QStringList _authorities(const QJsonValue &value);

bool _position(const QJsonValue &value, QGeoCoordinate &coordinate);

/// Reads the positions of a ring, without the closing one. Their number is checked by _normalizeVolumes.
/// @return false for invalid positions and rings over kMaxRingVertices
bool _ring(const QJsonValue &value, GeoZone::Ring &ring);

bool _polygonRings(const QJsonValue &value, GeoZone::Volume &volume);

void _appendVolume(const GeoZone::Volume &volume, QList<GeoZone::Volume> &volumes);

bool _validRadius(double radius);

/// Checks and normalizes the volumes of a parsed zone, adding the problems found to its issues. Consecutive duplicate
/// vertices and the closing vertex are removed. Polygons with fewer than 3 distinct points, without area, or touching
/// or enclosing a pole are dropped, as are circles without a valid radius; invalid holes are dropped, which enlarges
/// the zone. Outer rings are made counterclockwise and holes clockwise. Rings crossing themselves are kept, with the
/// even-odd rule of the containment tests, and reported.
/// @return true if the zone has a volume left
bool _normalizeVolumes(GeoZone::Data &zone);

/// Adds the volumes described by a GeoJSON-like horizontal geometry. Vertical limits are copied from volumeTemplate.
void _appendHorizontalGeometry(const QJsonObject &geometry, const GeoZone::Volume &volumeTemplate, double radiusScale, QList<GeoZone::Volume> &volumes);

void _commonProperties(const QJsonObject &object, const QString &restrictionKey, GeoZone::Data &zone);

} // namespace GeoZoneParserUtils
