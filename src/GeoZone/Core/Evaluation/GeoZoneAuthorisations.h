/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QTime>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include <memory>
#include <optional>

#include "GeoZone.h"

Q_DECLARE_LOGGING_CATEGORY(GeoZoneAuthorisationsLog)

/// Permit to fly in a zone, as entered by the operator. Its scope may limit it to an altitude, an area, daily hours
/// and an aircraft; without, it covers the whole zone.
struct GeoZoneAuthorisation
{
    QString zoneKey;        ///< See GeoZoneAuthorisations::zoneKey
    QString zoneName;       ///< Shown when the zone isn't loaded
    QString permit;         ///< Permit or reference number, may be empty
    QDateTime validFrom;    ///< UTC, invalid when valid until validTo
    QDateTime validTo;      ///< UTC and exclusive, invalid when without end

    std::optional<double> ceiling;  ///< Highest altitude allowed in the zone, meters, see ceilingAboveGround
    bool ceilingAboveGround = false;///< The ceiling is above ground level, otherwise above mean sea level
    QList<QGeoCoordinate> area;     ///< Unclosed polygon the flight in the zone must stay in, empty without
    QGeoCircle areaCircle;          ///< Circle the flight in the zone must stay in, invalid without
    QTime dailyFrom;                ///< Local time of day the flight may start, invalid without daily hours
    QTime dailyTo;                  ///< Local time of day the flight must end by, before dailyFrom across midnight
    QString aircraft;               ///< Registration or description of the aircraft, for information

    static constexpr double areaBuffer = 250;   ///< Meters around the mission area of an authorisation limited to it

    /// @return true if valid during the whole period from start to end, by date and daily hours
    bool covers(const QDateTime &start, const QDateTime &end) const;
    bool expired(const QDateTime &now) const { return validTo.isValid() && (validTo <= now); }
    bool hasDailyHours() const { return dailyFrom.isValid() && dailyTo.isValid(); }
    /// @return true if the whole period is within the daily hours, or without daily hours
    bool withinDailyHours(const QDateTime &start, const QDateTime &end) const;
    bool hasArea() const { return (area.count() >= 3) || areaCircle.isValid(); }
    /// @return true if the scope limits where the flight may go in the zone: a ceiling or an area
    bool limitsPath() const { return ceiling.has_value() || hasArea(); }
    /// @return The part of the zone the scope allows flying in: the area, or the zone's horizontal extent, below the
    ///         ceiling and without floor. Null without zone.
    std::shared_ptr<const GeoZone> scopeZone(const GeoZone *zone) const;
    /// @return true if the position is inside the area and below the ceiling of the scope. An unknown altitude, or
    ///         terrain for a ceiling above ground, isn't below the ceiling.
    bool scopeContains(const GeoZone *zone, const GeoZone::Position &position) const;
    /// @return The area around a mission's bounds, widened by areaBuffer
    static QList<QGeoCoordinate> missionArea(const QGeoRectangle &missionBounds);

    QJsonObject toJson() const;
    /// @return Nothing without a zone key
    static std::optional<GeoZoneAuthorisation> fromJson(const QJsonObject &json);

    friend bool operator==(const GeoZoneAuthorisation &, const GeoZoneAuthorisation &) = default;
};

/// Zones the operator is authorised to fly in, which the checks and alerts leave out while the authorisation is
/// valid and the flight stays within its scope, see GeoZonePolicy::excludingAuthorised. Kept in a file, with roles zoneKey, zoneName, permit and statusText.
class GeoZoneAuthorisations : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int          count                   READ count                  NOTIFY changed FINAL)
    /// Authorisations of a loaded plan waiting for the user to add them, as maps with zoneName, permit and statusText
    Q_PROPERTY(QVariantList pendingAuthorisations   READ pendingAuthorisations  NOTIFY pendingChanged FINAL)

public:
    enum Roles : int {
        ZoneKeyRole = Qt::UserRole + 1,
        ZoneNameRole,
        PermitRole,
        StatusTextRole,
    };

    static constexpr const char *fileName = "authorisations.json";

    /// @param filePath File keeping the authorisations, empty to keep them in memory
    explicit GeoZoneAuthorisations(const QString &filePath, QObject *parent = nullptr);

    /// @return Key matching the zone in any source: its country and identifier, or GeoZone::key without identifier
    static QString zoneKey(const GeoZone *zone);

    int count() const { return static_cast<int>(_authorisations.count()); }
    const QList<GeoZoneAuthorisation> &authorisations() const { return _authorisations; }

    /// @return Authorisation of the zone, null without
    const GeoZoneAuthorisation *find(const GeoZone *zone) const;

    /// Adds the authorisation, replacing the one of the same zone
    void set(const GeoZoneAuthorisation &authorisation);
    /// Adds authorisations, like those of a plan file. They replace those of the same zones which end sooner.
    void merge(const QList<GeoZoneAuthorisation> &authorisations);

    /// Offers the authorisations of a loaded plan, those which would add to the known ones, for the user to accept.
    /// They replace those offered before.
    void offer(const QList<GeoZoneAuthorisation> &authorisations);
    const QList<GeoZoneAuthorisation> &pending() const { return _pending; }
    QVariantList pendingAuthorisations() const;
    /// Adds the offered authorisations
    Q_INVOKABLE void acceptPending();
    Q_INVOKABLE void dismissPending();

    /// Authorises the zone from local dates, like 2026-12-31, both included
    /// @param validFrom First day, empty to start now
    /// @param validTo Last day, empty without end
    /// @param scope Optional limits, all texts empty without: ceiling in the vertical distance units above the
    ///              reference of ceilingUnits, dailyFrom and dailyTo as local times like 08:00, aircraft, and
    ///              limitToMission with missionArea, the bounds of the mission, see GeoZonePlanning::missionArea.
    ///              Without missionArea, the area of the authorisation being edited is kept.
    /// @return Error message, empty on success
    Q_INVOKABLE QString authorise(const QVariant &zone, const QString &permit, const QString &validFrom, const QString &validTo,
                                  const QVariantMap &scope = QVariantMap());
    Q_INVOKABLE void revoke(const QString &zoneKey);
    /// @return Map with zoneKey, permit, validFrom and validTo (local dates, empty when open), the scope (ceiling in
    ///         the vertical distance units, dailyFrom, dailyTo, aircraft and limitToMission when it has an area) and
    ///         statusText of the authorisation of a GeoZoneInfo, empty without
    Q_INVOKABLE QVariantMap authorisationOf(const QVariant &zone) const;
    /// @return Units of the ceiling of an authorisation of the GeoZoneInfo, above ground when the zone has limits
    ///         above ground, like "m AGL"
    Q_INVOKABLE QString ceilingUnits(const QVariant &zone) const;

    /// @return Validity of the authorisation at the time, like "Valid until 2026-12-31", with its scope
    static QString statusText(const GeoZoneAuthorisation &authorisation, const QDateTime &now);
    /// @return The limits of the scope, like "up to 120 m AGL, 08:00–18:00, within its area, aircraft HB-ABC", empty
    ///         when unlimited
    static QString scopeText(const GeoZoneAuthorisation &authorisation);
    /// @return The ceiling in the vertical distance units with its reference, like "120 m AGL"
    static QString heightText(double meters, bool aboveGround);
    /// @return Why the authorisation doesn't cover the period from start to end, like "authorised from 2026-12-01",
    ///         empty when it does
    static QString uncoveredText(const GeoZoneAuthorisation &authorisation, const QDateTime &start, const QDateTime &end);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void changed();
    void pendingChanged();
    /// The file couldn't be written, the authorisations are kept for the session
    void saveFailed(const QString &message);

private:
    /// @return Validity of the authorisation at the time without its scope
    static QString validityText(const GeoZoneAuthorisation &authorisation, const QDateTime &now);

    void _load();
    void _save();
    void _reset(const QList<GeoZoneAuthorisation> &authorisations);
    /// @return The known authorisations with those of the list added, keeping the longer of the same zone
    QList<GeoZoneAuthorisation> _merged(const QList<GeoZoneAuthorisation> &authorisations) const;

    QString _filePath;
    QList<GeoZoneAuthorisation> _authorisations;
    QList<GeoZoneAuthorisation> _pending;
};
