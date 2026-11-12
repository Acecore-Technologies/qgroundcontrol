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
#include <QtCore/QList>

#include "GeoZoneInfo.h"

class GeoZone;

/// The zones labelled on a map, one row per zone with the role zone (GeoZoneInfo). setZones() only inserts the rows of
/// the zones which are new and removes those of the zones which are gone, so that a view keeps the items of the
/// others, like the labels of a map which is panned. The rows are in the order the zones were first set, not in the
/// order of the zones set last.
class GeoZoneLabelModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles : int {
        ZoneRole = Qt::UserRole + 1,
    };

    explicit GeoZoneLabelModel(QObject *parent = nullptr);

    /// The zones of the rows
    QList<const GeoZone*> zones() const;
    /// @return true when rows were inserted or removed
    bool setZones(const QList<const GeoZone*> &zones);
    /// Removes the rows of the zones of a source, before they are deleted
    /// @return true when rows were removed
    bool removeSource(const QString &sourceKey);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    /// Removes the rows which aren't kept, in runs
    template<typename Kept>
    bool _removeRows(const Kept &kept);

    QList<GeoZoneInfo> _rows;
};
