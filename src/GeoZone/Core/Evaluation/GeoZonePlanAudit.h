/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

#include "GeoZone.h"
#include "GeoZoneSourceVersion.h"

class GeoZoneCatalog;

/// Audit of the geozone data a plan was checked against: when a plan is saved, the versions of the sources covering
/// the mission and the content hashes of the zones relevant to it, those the mission and the returns from along it
/// enter, are kept in the plan. When it is opened again, or the sources reload, the zones are compared with the
/// current data, so that the pilot reviews a plan whose zones changed meanwhile.
class GeoZonePlanAudit
{
    Q_DECLARE_TR_FUNCTIONS(GeoZonePlanAudit)

public:
    /// Zone relevant to the mission when the plan was saved
    struct Zone {
        QString key;            ///< See GeoZone::key
        QString sourceKey;
        QString identifier;
        QString country;
        QString name;
        GeoZone::Restriction restriction = GeoZone::Unknown;
        QByteArray contentHash; ///< See GeoZone::Data::contentHash

        static Zone fromZone(const GeoZone *zone);
        QString displayName() const { return name.isEmpty() ? identifier : name; }

        friend bool operator==(const Zone &, const Zone &) = default;
    };

    /// What a plan keeps of the data it was checked against
    struct Record {
        QDateTime savedAt;                      ///< UTC
        QList<GeoZoneSourceVersion> sources;    ///< Covering the mission
        QList<Zone> zones;                      ///< Relevant to the mission

        static Record make(const QList<const GeoZone*> &zones, const QList<GeoZoneSourceVersion> &sources, const QDateTime &savedAt);
        QJsonObject toJson() const;
        /// @return Nothing for JSON without the version of the audit, like that of other applications
        static std::optional<Record> fromJson(const QJsonObject &json);

        friend bool operator==(const Record &, const Record &) = default;
    };

    struct ZoneChange {
        Zone saved;             ///< Empty for added zones
        Zone current;           ///< Empty for removed zones
    };

    struct SourceChange {
        GeoZoneSourceVersion saved;
        GeoZoneSourceVersion current;   ///< Empty when the source was removed
        bool removed() const { return current.key.isEmpty(); }
    };

    /// Differences between the data a plan was saved with and the current data
    struct Changes {
        QList<ZoneChange> modified;     ///< Zones whose published content changed
        QList<ZoneChange> removed;      ///< Zones no source has anymore
        QList<ZoneChange> added;        ///< Zones now relevant to the mission which weren't in the data when saved
        QList<SourceChange> sources;    ///< Sources updated or removed since

        bool isEmpty() const { return modified.isEmpty() && removed.isEmpty() && added.isEmpty() && sources.isEmpty(); }
        /// @return Short warning, like "Geozone data changed since this plan was saved: 2 zones modified, 1 new.",
        ///         empty without changes
        QString summary() const;
        /// @return One line per change, like "Modified: CTR Zurich (Authorisation required, was Prohibited)"
        QStringList details() const;
    };

    static constexpr int version = 1;   ///< Of the JSON of the record

    /// @return How the current data differs from the record
    /// @param relevantZones The zones relevant to the mission now, see Record::zones
    /// @param catalog Where the zones of the record are looked up, by key, or by country and identifier for zones
    ///                renamed or moved to another source
    /// @param sources The current versions of all sources, see GeoZoneSourceVersion::covering. Zones relevant now which
    ///                the record lacks are only new when their source changed or was loaded since the record.
    static Changes compare(const Record &record, const QList<const GeoZone*> &relevantZones, const GeoZoneCatalog &catalog,
                           const QList<GeoZoneSourceVersion> &sources);
};
