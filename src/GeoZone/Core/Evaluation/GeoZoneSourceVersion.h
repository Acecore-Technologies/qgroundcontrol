/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>

class GeoZoneBox;
class GeoZoneSource;

/// Version of the data of a geozone source a check used, for briefings and the audit of plans: which file or feed, of
/// which date, and the hash of its content
struct GeoZoneSourceVersion
{
    QString key;            ///< See GeoZoneSource::key
    QString name;
    QString format;
    QString provider;
    QDateTime dataDate;     ///< When the data was issued, or the file date
    QDateTime loadedAt;
    QDateTime checkedAt;    ///< When the server last confirmed the data current
    QDateTime validFrom;    ///< Validity published with the data, invalid when not published
    QDateTime validTo;
    QString airacCycle;     ///< AIRAC cycle in effect at the data date, like "2601", empty for live data
    QByteArray contentHash; ///< Of the loaded file, see GeoZoneSource::contentHash

    /// @return The version of the source's current data
    static GeoZoneSourceVersion fromSource(const GeoZoneSource *source);
    /// @return The versions of the enabled, loaded sources whose zones intersect the area, all of them when the area is
    ///         invalid
    static QList<GeoZoneSourceVersion> covering(const QList<const GeoZoneSource*> &sources, const GeoZoneBox &area);

    /// @return Identifier of the AIRAC cycle in effect on the date, two digits of the year and the cycle number in the
    ///         year, like "2601" from 2026-01-22, empty for an invalid date
    static QString airacCycleOf(const QDate &date);

    /// @return true if both have the same data: the same content hash, or the same data date without hashes
    bool sameData(const GeoZoneSourceVersion &other) const;

    QJsonObject toJson() const;
    static GeoZoneSourceVersion fromJson(const QJsonObject &json);

    friend bool operator==(const GeoZoneSourceVersion &, const GeoZoneSourceVersion &) = default;
};
