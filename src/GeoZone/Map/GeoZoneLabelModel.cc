/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneLabelModel.h"
#include "GeoZone.h"

#include <QtCore/QSet>

GeoZoneLabelModel::GeoZoneLabelModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

QList<const GeoZone*> GeoZoneLabelModel::zones() const
{
    QList<const GeoZone*> zones;
    zones.reserve(_rows.count());
    for (const GeoZoneInfo &row : _rows) {
        zones.append(row.zone());
    }
    return zones;
}

template<typename Kept>
bool GeoZoneLabelModel::_removeRows(const Kept &kept)
{
    bool removed = false;
    // From the last row, so that the rows still to check keep their numbers
    qsizetype last = _rows.count() - 1;
    while (last >= 0) {
        if (kept(_rows[last])) {
            last--;
            continue;
        }
        qsizetype first = last;
        while ((first > 0) && !kept(_rows[first - 1])) {
            first--;
        }
        beginRemoveRows(QModelIndex(), static_cast<int>(first), static_cast<int>(last));
        _rows.remove(first, last - first + 1);
        endRemoveRows();
        removed = true;
        last = first - 1;
    }
    return removed;
}

bool GeoZoneLabelModel::setZones(const QList<const GeoZone*> &zones)
{
    const QSet<const GeoZone*> wanted(zones.cbegin(), zones.cend());
    bool changed = _removeRows([&wanted](const GeoZoneInfo &row) { return wanted.contains(row.zone()); });

    QSet<const GeoZone*> present;
    for (const GeoZoneInfo &row : std::as_const(_rows)) {
        present.insert(row.zone());
    }
    QList<GeoZoneInfo> added;
    for (const GeoZone *zone : zones) {
        if (zone && !present.contains(zone)) {
            present.insert(zone);
            added.append(GeoZoneInfo(zone));
        }
    }
    if (!added.isEmpty()) {
        beginInsertRows(QModelIndex(), static_cast<int>(_rows.count()), static_cast<int>(_rows.count() + added.count() - 1));
        _rows.append(added);
        endInsertRows();
        changed = true;
    }
    return changed;
}

bool GeoZoneLabelModel::removeSource(const QString &sourceKey)
{
    return _removeRows([&sourceKey](const GeoZoneInfo &row) { return row.sourceKey() != sourceKey; });
}

int GeoZoneLabelModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(_rows.count());
}

QVariant GeoZoneLabelModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid) || (role != ZoneRole)) {
        return QVariant();
    }
    return QVariant::fromValue(_rows[index.row()]);
}

QHash<int, QByteArray> GeoZoneLabelModel::roleNames() const
{
    static const QHash<int, QByteArray> roles{ { ZoneRole, "zone" } };
    return roles;
}
