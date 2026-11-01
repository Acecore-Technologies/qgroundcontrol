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

#include <optional>

class GeoZone;

/// Permit to fly in a zone, as entered by the operator
struct GeoZoneAuthorisation
{
    QString zoneKey;        ///< See GeoZoneAuthorisations::zoneKey
    QString zoneName;       ///< Shown when the zone isn't loaded
    QString permit;         ///< Permit or reference number, may be empty
    QDateTime validFrom;    ///< UTC, invalid when valid until validTo
    QDateTime validTo;      ///< UTC and exclusive, invalid when without end

    /// @return true if valid during the whole period from start to end
    bool covers(const QDateTime &start, const QDateTime &end) const;
    bool expired(const QDateTime &now) const { return validTo.isValid() && (validTo <= now); }

    QJsonObject toJson() const;
    /// @return Nothing without a zone key
    static std::optional<GeoZoneAuthorisation> fromJson(const QJsonObject &json);

    friend bool operator==(const GeoZoneAuthorisation &, const GeoZoneAuthorisation &) = default;
};

/// Zones the operator is authorised to fly in, which the checks and alerts leave out while the authorisation is
/// valid, see GeoZonePolicy::excludingAuthorised. Kept in a file, with roles zoneKey, zoneName, permit and statusText.
class GeoZoneAuthorisations : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int count READ count NOTIFY changed FINAL)

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

    /// Authorises the zone from local dates, like 2026-12-31, both included
    /// @param validFrom First day, empty to start now
    /// @param validTo Last day, empty without end
    /// @return Error message, empty on success
    Q_INVOKABLE QString authorise(const QVariant &zone, const QString &permit, const QString &validFrom, const QString &validTo);
    Q_INVOKABLE void revoke(const QString &zoneKey);
    /// @return Map with zoneKey, permit, validFrom and validTo (local dates, empty when open) and statusText of the
    ///         authorisation of a GeoZoneInfo, empty without
    Q_INVOKABLE QVariantMap authorisationOf(const QVariant &zone) const;

    /// @return Validity of the authorisation at the time, like "Valid until 2026-12-31"
    static QString statusText(const GeoZoneAuthorisation &authorisation, const QDateTime &now);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void changed();
    /// The file couldn't be written, the authorisations are kept for the session
    void saveFailed(const QString &message);

private:
    void _load();
    void _save();
    void _reset(const QList<GeoZoneAuthorisation> &authorisations);

    QString _filePath;
    QList<GeoZoneAuthorisation> _authorisations;
};
