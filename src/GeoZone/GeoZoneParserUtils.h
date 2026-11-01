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

/// Sets the preferred language of localized texts for the parse running on this thread
/// @return Code of the preferred language of the parse running on this thread, like "de", empty for English
const QString &language();

struct LanguageScope {
    explicit LanguageScope(const QString &language);
    ~LanguageScope();
};

/// Returns text from either a plain string or an ED-318 localized text array ([{ "text": "...", "lang": "en-GB" }])
QString _text(const QJsonValue &value);

QStringList _stringList(const QJsonValue &value);

double _number(const QJsonValue &value);

/// Parses ISO 8601 dates and times. Times without an offset are UTC in both standards, dates start at midnight UTC.
QDateTime _dateTime(const QJsonValue &value);

/// Parses ED-269/ED-318 times of day such as "06:00Z", "05:00:00.00Z" or "08:00+02:00". The time is returned
/// as published, utcOffsetSecs is set to its offset from UTC.
QTime _time(const QJsonValue &value, int &utcOffsetSecs);

QList<GeoZone::TimePeriod> _applicability(const QJsonValue &value);

QStringList _authorities(const QJsonValue &value);

bool _position(const QJsonValue &value, QGeoCoordinate &coordinate);

bool _ring(const QJsonValue &value, QList<QGeoCoordinate> &ring);

bool _polygonRings(const QJsonValue &value, GeoZone::Volume &volume);

void _appendVolume(const GeoZone::Volume &volume, QList<GeoZone::Volume> &volumes);

bool _validRadius(double radius);

/// Adds the volumes described by a GeoJSON-like horizontal geometry. Vertical limits are copied from volumeTemplate.
void _appendHorizontalGeometry(const QJsonObject &geometry, const GeoZone::Volume &volumeTemplate, double radiusScale, QList<GeoZone::Volume> &volumes);

void _commonProperties(const QJsonObject &object, const QString &restrictionKey, GeoZone::Data &zone);

} // namespace GeoZoneParserUtils
