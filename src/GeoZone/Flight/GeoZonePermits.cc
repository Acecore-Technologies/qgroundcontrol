/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePermits.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneInfo.h"
#include "GeoZoneText.h"

#include <algorithm>

GeoZonePermits::GeoZonePermits(QObject *parent)
    : QAbstractListModel(parent)
{
}

int GeoZonePermits::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant GeoZonePermits::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || (index.row() >= _permits.count())) {
        return QVariant();
    }
    const Permit &permit = _permits[index.row()];
    switch (role) {
    case KeyRole:
        return permit.key;
    case ZoneNameRole:
        return permit.zoneName;
    case RestrictionTextRole:
        return GeoZoneText::restriction(permit.restriction);
    case TimeRole:
        return permit.time.toLocalTime();
    case OriginTextRole:
        switch (permit.origin) {
        case Checklist:
            return tr("Pre-flight checklist");
        case AlertPopup:
            return tr("Alert");
        case ZoneDetails:
            break;
        }
        return tr("Zone details");
    case VehicleIdRole:
        return permit.vehicleId;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GeoZonePermits::roleNames() const
{
    return {
        { KeyRole, "key" },
        { ZoneNameRole, "zoneName" },
        { RestrictionTextRole, "restrictionText" },
        { TimeRole, "time" },
        { OriginTextRole, "originText" },
        { VehicleIdRole, "vehicleId" },
    };
}

bool GeoZonePermits::applies(const GeoZone *zone)
{
    if (!zone || (zone->restriction() == GeoZone::Prohibited)) {
        return false;
    }
    return (zone->restriction() == GeoZone::ReqAuthorisation) || (zone->data().notificationLeadSecs >= 0);
}

QString GeoZonePermits::key(const GeoZone *zone)
{
    return GeoZoneAuthorisations::zoneKey(zone);
}

QString GeoZonePermits::originText(Origin origin)
{
    switch (origin) {
    case Checklist:
        return QStringLiteral("pre-flight checklist");
    case AlertPopup:
        return QStringLiteral("alert popup");
    case ZoneDetails:
        break;
    }
    return QStringLiteral("zone details");
}

bool GeoZonePermits::contains(const QString &key) const
{
    return !key.isEmpty() && std::any_of(_permits.cbegin(), _permits.cend(), [&key](const Permit &permit) { return permit.key == key; });
}

bool GeoZonePermits::acknowledge(const GeoZone *zone, Origin origin, int vehicleId)
{
    if (!applies(zone) || covers(zone)) {
        return false;
    }
    Permit permit;
    permit.key = key(zone);
    permit.zoneKey = zone->key();
    permit.zoneIdentifier = zone->identifier();
    permit.zoneName = zone->displayName();
    permit.restriction = zone->restriction();
    permit.sourceText = zone->sourceText();
    permit.time = GeoZoneEnvironment::currentDateTimeUtc();
    permit.vehicleId = vehicleId;
    permit.origin = origin;
    beginInsertRows(QModelIndex(), count(), count());
    _permits.append(permit);
    endInsertRows();
    emit countChanged();
    emit acknowledged(permit);
    return true;
}

bool GeoZonePermits::acknowledge(const QVariant &zone, int origin)
{
    return acknowledge(zone.value<GeoZoneInfo>().zone(), static_cast<Origin>(origin), _vehicleId);
}

bool GeoZonePermits::applies(const QVariant &zone) const
{
    return applies(zone.value<GeoZoneInfo>().zone());
}

bool GeoZonePermits::permitted(const QVariant &zone) const
{
    return covers(zone.value<GeoZoneInfo>().zone());
}

void GeoZonePermits::withdraw(const QString &key)
{
    const auto it = std::find_if(_permits.cbegin(), _permits.cend(), [&key](const Permit &permit) { return permit.key == key; });
    if (it != _permits.cend()) {
        _remove(static_cast<int>(std::distance(_permits.cbegin(), it)), QStringLiteral("withdrawn"));
    }
}

void GeoZonePermits::endFlight(int vehicleId, const QString &reason)
{
    for (int row = count() - 1; row >= 0; row--) {
        if ((_permits[row].vehicleId == vehicleId) || (_permits[row].vehicleId < 0)) {
            // Acknowledged before the vehicle connected, the permit was for its flight
            _permits[row].vehicleId = vehicleId;
            _remove(row, reason);
        }
    }
}

void GeoZonePermits::_remove(int row, const QString &reason)
{
    beginRemoveRows(QModelIndex(), row, row);
    const Permit permit = _permits.takeAt(row);
    endRemoveRows();
    emit countChanged();
    emit ended(permit, reason);
}
