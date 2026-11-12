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
    /// A notification the operator gave the authority to notify, see GeoZone::Data::notificationLeadSecs, with its
    /// reference as permit, rather than a permit. A zone can have both.
    bool notification = false;
    /// The zone was prohibited when it was authorised, see validFor
    bool prohibitedZone = false;

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

    /// @return true with what the authorisation of a prohibited zone needs: a permit reference and an end
    bool hasPermitAndEnd() const { return !permit.trimmed().isEmpty() && validTo.isValid(); }
    /// @return true if the authorisation can stand for the zone. A prohibited zone, now or when it was authorised, is
    ///         only left out of the checks and alerts for a permit which can be referred to and which ends, see
    ///         hasPermitAndEnd. Notifications authorise nothing and need neither.
    bool validFor(const GeoZone *zone) const;

    /// @return true for a ceiling an authorisation can have: a height above 0 and below the one published for zones
    ///         without ceiling, GeoZoneVertical::unlimitedSentinel, from which a ceiling doesn't limit anything
    static bool ceilingValid(double meters);
    /// @return true if the limits can be applied: the end after the start, the ceiling a valid one, the area a polygon
    ///         of valid vertices or a circle with a radius, and the daily hours both there or none. authorise() and
    ///         fromJson() only give authorisations whose limits are.
    bool limitsValid() const;

    QJsonObject toJson() const;
    /// @return Nothing without a zone key, when a limit which is there can't be read or isn't valid, see
    ///         limitsValid: taken as no limit it would authorise more than the permit does, or for the authorisation
    ///         of a prohibited zone without permit reference or end, see validFor
    static std::optional<GeoZoneAuthorisation> fromJson(const QJsonObject &json);

    friend bool operator==(const GeoZoneAuthorisation &, const GeoZoneAuthorisation &) = default;
};

/// Zones the operator is authorised to fly in, which the checks and alerts leave out while the authorisation is
/// valid and the flight stays within its scope, see GeoZonePolicy::excludingAuthorised, and the notifications given to the
/// authorities of zones requiring them. Kept in a file, with roles zoneKey, zoneName, permit, statusText and
/// notification.
///
/// The file has a version and the entries of GeoZoneAuthorisation::toJson. A file which can't be read, or only in
/// part, isn't lost to the next change: it is moved or copied to a backup next to it ("<file>.<time>.bak") before
/// anything is written, and when that fails, or the file is of a newer version, it is left alone and the
/// authorisations are kept for the session, see loadError.
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
        NotificationRole,
    };

    static constexpr const char *fileName = "authorisations.json";

    /// @param filePath File keeping the authorisations, empty to keep them in memory
    explicit GeoZoneAuthorisations(const QString &filePath, QObject *parent = nullptr);

    /// @return Key matching the zone in any source: its country and identifier, or GeoZone::key without identifier
    static QString zoneKey(const GeoZone *zone);

    int count() const { return static_cast<int>(_authorisations.count()); }
    const QList<GeoZoneAuthorisation> &authorisations() const { return _authorisations; }
    /// @return Why the file wasn't read, or only in part, with what became of it, empty when it was. Also emitted as
    ///         loadFailed.
    QString loadError() const { return _loadError; }

    /// @return Authorisation of the zone, null without or when it can't stand for the zone, see
    ///         GeoZoneAuthorisation::validFor
    const GeoZoneAuthorisation *find(const GeoZone *zone) const;
    /// @return Notification recorded for the zone, null without
    const GeoZoneAuthorisation *findNotification(const GeoZone *zone) const { return _find(zone, true); }

    /// Adds the authorisation, replacing the one of the same zone and kind
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

    /// Authorises the zone from local dates, like 2026-12-31, both included. A prohibited zone needs the permit and
    /// the last day.
    /// @param validFrom First day, empty to start now
    /// @param validTo Last day, empty without end
    /// @param scope Optional limits, all texts empty without: ceiling in the vertical distance units above the
    ///              reference of ceilingUnits, dailyFrom and dailyTo as local times like 08:00, aircraft, and
    ///              limitToMission with missionArea, the bounds of the mission, see GeoZonePlanning::missionArea.
    ///              Without missionArea, the area of the authorisation being edited is kept.
    /// @return Error message, empty on success
    Q_INVOKABLE QString authorise(const QVariant &zone, const QString &permit, const QString &validFrom, const QString &validTo,
                                  const QVariantMap &scope = QVariantMap());
    /// Records the notification of the zone's authority from local dates, like authorise
    /// @param reference Reference of the notification, may be empty
    Q_INVOKABLE QString recordNotification(const QVariant &zone, const QString &reference, const QString &validFrom, const QString &validTo);
    Q_INVOKABLE void revoke(const QString &zoneKey, bool notification = false);
    /// @return Map with zoneKey, permit, validFrom and validTo (local dates, empty when open), the scope (ceiling in
    ///         the vertical distance units, dailyFrom, dailyTo, aircraft and limitToMission when it has an area) and
    ///         statusText of the authorisation of a GeoZoneInfo, empty without. One which can't stand for the zone,
    ///         see find, is returned to be completed, its statusText telling what it misses.
    Q_INVOKABLE QVariantMap authorisationOf(const QVariant &zone) const;
    /// @return Map with zoneKey, permit (the reference), validFrom, validTo and statusText of the notification
    ///         recorded for a GeoZoneInfo, empty without
    Q_INVOKABLE QVariantMap notificationOf(const QVariant &zone) const;
    /// @return What notice the authority of the GeoZoneInfo needs, like "Notify Tower 2 d before the flight",
    ///         empty when it doesn't need any
    Q_INVOKABLE QString notificationRequirement(const QVariant &zone) const;
    /// @return Notice the zone's authority needs before the flight, like "notify Tower by 2026-06-13 12:00" with a
    ///         start, empty without notification
    /// @param overdue Set to whether the notice is due by now
    static QString notificationText(const GeoZone *zone, const QDateTime &start, const QDateTime &now, bool *overdue = nullptr);
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
    /// The file couldn't be read, or not all of it, see loadError. Emitted from the event loop after the
    /// authorisations are made, so that it can be connected to.
    void loadFailed(const QString &message);

private:
    /// @return Validity of the authorisation at the time without its scope
    static QString validityText(const GeoZoneAuthorisation &authorisation, const QDateTime &now);
    /// @return The entry of the zone, whether it can stand for it or not
    const GeoZoneAuthorisation *_find(const GeoZone *zone, bool notification) const;
    /// Reads the authorisation, or the notification, of the zone from what the user entered, see authorise
    /// @return Error message, empty on success
    QString _readInput(const QVariant &zone, const QString &permit, const QString &validFrom, const QString &validTo, const QVariantMap &scope,
                  bool notification, GeoZoneAuthorisation &authorisation) const;

    void _load();
    /// Reads the file
    /// @param unread Set to the number of entries left out, see GeoZoneAuthorisation::fromJson
    /// @param newer Set to whether the file is of a newer version, which is neither read nor written
    /// @return Why it can't be read, empty when it was
    QString _readFile(QList<GeoZoneAuthorisation> &authorisations, int &unread, bool &newer) const;
    /// Keeps the file as it is in a backup next to it
    /// @param move The file is moved rather than copied
    /// @return Path of the backup, empty when it couldn't be made
    QString _backUp(bool move) const;
    void _save();
    void _reset(const QList<GeoZoneAuthorisation> &authorisations);
    /// @return The known authorisations with those of the list added, keeping the longer of the same zone
    QList<GeoZoneAuthorisation> _merged(const QList<GeoZoneAuthorisation> &authorisations) const;

    QString _filePath;
    QString _loadError;
    bool _keepFile = false;         ///< The file wasn't read and couldn't be backed up, or is of a newer version: it isn't written
    bool _keepFileReported = false;
    QList<GeoZoneAuthorisation> _authorisations;
    QList<GeoZoneAuthorisation> _pending;
};
