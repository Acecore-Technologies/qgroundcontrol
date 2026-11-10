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
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QVariant>

#include "GeoZone.h"
#include "GeoZoneInfo.h"

/// The pilot's acknowledgements of the permission to enter zones requiring an authorisation or a notification, for
/// one flight: from the zone details or the pre-flight checklist before the flight, or by answering "I do" to an alert
/// in flight. A permit stops the alerts about the zone, apart from an advisory on entry, and ends after the flight:
/// when the vehicle disarms after having been armed, or disconnects, see endFlight(). Unlike the authorisations of
/// GeoZoneAuthorisations, permits aren't kept. They apply to the zone in every source, see key(). GeoZoneMonitor logs
/// them in the alert log. Roles key, zoneName, restrictionText, time (local), originText and vehicleId.
class GeoZonePermits : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    /// Where the pilot acknowledged the permit
    enum Origin {
        ZoneDetails,
        Checklist,
        AlertPopup,
    };
    Q_ENUM(Origin)

    enum Roles {
        KeyRole = Qt::UserRole + 1,
        ZoneNameRole,
        RestrictionTextRole,
        TimeRole,
        OriginTextRole,
        VehicleIdRole,
    };

    struct Permit {
        QString key;                ///< See key()
        QString zoneKey;            ///< GeoZone::key of the zone acknowledged
        QString zoneIdentifier;
        QString zoneName;           ///< Name, or the identifier of unnamed zones
        GeoZone::Restriction restriction = GeoZone::Unknown;
        QString sourceText;
        QDateTime time;             ///< UTC
        int vehicleId = -1;         ///< Active vehicle when acknowledged, GeoZoneMonitor::noVehicle without
        Origin origin = ZoneDetails;
    };

    explicit GeoZonePermits(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(_permits.count()); }
    const QList<Permit> &permits() const { return _permits; }

    /// @return true for zones requiring an authorisation, or a notification unless they are prohibited
    static bool applies(const GeoZone *zone);
    /// @return Key matching the zone in any source, see GeoZoneAuthorisations::zoneKey
    static QString key(const GeoZone *zone);
    /// @return Where the permit was acknowledged, like "zone details", as logged
    static QString originText(Origin origin);

    bool covers(const GeoZone *zone) const { return zone && contains(key(zone)); }
    bool contains(const QString &key) const;

    /// Acknowledges the permit of the zone for the flight of the vehicle
    /// @return false when it doesn't apply to the zone or is acknowledged already
    bool acknowledge(const GeoZone *zone, Origin origin, int vehicleId);
    /// Acknowledges the permit of a GeoZoneInfo for the flight of the active vehicle, see setVehicleId()
    Q_INVOKABLE bool acknowledge(const QVariant &zone, int origin);
    Q_INVOKABLE bool applies(const QVariant &zone) const;
    Q_INVOKABLE bool permitted(const QVariant &zone) const;
    /// Withdraws a permit by key
    Q_INVOKABLE void withdraw(const QString &key);
    /// Withdraws the permit of a GeoZoneInfo
    Q_INVOKABLE void withdrawZone(const QVariant &zone) { withdraw(key(zone.value<GeoZoneInfo>().zone())); }
    /// Ends the permits of the flight of the vehicle: those acknowledged for it or without vehicle, which are logged as
    /// the vehicle's
    /// @param reason Logged, like "flight ended"
    void endFlight(int vehicleId, const QString &reason);

    /// Vehicle the permits acknowledged from QML are for
    void setVehicleId(int vehicleId) { _vehicleId = vehicleId; }

signals:
    void countChanged();
    void acknowledged(const GeoZonePermits::Permit &permit);
    /// @param reason Like "withdrawn" or "flight ended"
    void ended(const GeoZonePermits::Permit &permit, const QString &reason);

private:
    void _remove(int row, const QString &reason);

    QList<Permit> _permits;     ///< In the order acknowledged
    int _vehicleId = -1;
};
