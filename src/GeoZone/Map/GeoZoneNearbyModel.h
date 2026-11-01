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

#include "GeoZoneAlertTracker.h"

/// Zones near the active vehicle with roles zone, inside, distance (meters) and timeToEntry (seconds, -1 when
/// the vehicle isn't heading into the zone). Rows are updated in place while only the distances change, so views
/// keep their delegates.
class GeoZoneNearbyModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    using Item = GeoZoneAlertTracker::Nearby;

    enum Roles : int {
        ZoneRole = Qt::UserRole + 1,
        InsideRole,
        DistanceRole,
        TimeToEntryRole,
    };

    explicit GeoZoneNearbyModel(QObject *parent = nullptr);

    int count() const { return static_cast<int>(_items.count()); }
    const QList<Item> &items() const { return _items; }
    void setItems(const QList<Item> &items);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    QList<Item> _items;
};
