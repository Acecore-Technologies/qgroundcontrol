/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneNearbyModel.h"
#include "GeoZoneInfo.h"

#include <algorithm>

GeoZoneNearbyModel::GeoZoneNearbyModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void GeoZoneNearbyModel::setItems(const QList<Item> &items)
{
    const bool sameZones = (items.count() == _items.count()) &&
                           std::equal(items.cbegin(), items.cend(), _items.cbegin(), [](const Item &a, const Item &b) { return a.zone == b.zone; });
    if (!sameZones) {
        const bool countChanged = (items.count() != _items.count());
        beginResetModel();
        _items = items;
        endResetModel();
        if (countChanged) {
            emit this->countChanged();
        }
        return;
    }

    for (qsizetype row = 0; row < items.count(); row++) {
        if (items[row] != _items[row]) {
            _items[row] = items[row];
            const QModelIndex changed = index(static_cast<int>(row));
            emit dataChanged(changed, changed, { InsideRole, DistanceRole, TimeToEntryRole });
        }
    }
}

int GeoZoneNearbyModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant GeoZoneNearbyModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return QVariant();
    }
    const Item &item = _items[index.row()];
    switch (role) {
    case ZoneRole:
        return QVariant::fromValue(GeoZoneInfo(item.zone));
    case InsideRole:
        return item.inside;
    case DistanceRole:
        return item.distance;
    case TimeToEntryRole:
        return item.timeToEntry;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GeoZoneNearbyModel::roleNames() const
{
    return { { ZoneRole, "zone" }, { InsideRole, "inside" }, { DistanceRole, "distance" }, { TimeToEntryRole, "timeToEntry" } };
}
