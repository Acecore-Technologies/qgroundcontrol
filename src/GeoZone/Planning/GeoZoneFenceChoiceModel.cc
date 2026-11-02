/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFenceChoiceModel.h"

#include <QtCore/QSet>

GeoZoneFenceChoiceModel::GeoZoneFenceChoiceModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void GeoZoneFenceChoiceModel::setRows(const QList<Row> &rows)
{
    const int previousCount = count();

    QSet<QString> newKeys;
    newKeys.reserve(rows.count());
    for (const Row &row : rows) {
        newKeys.insert(row.key);
    }
    QSet<QString> oldKeys;
    oldKeys.reserve(_rows.count());
    QStringList keptOld;
    for (const Row &row : std::as_const(_rows)) {
        oldKeys.insert(row.key);
        if (newKeys.contains(row.key)) {
            keptOld.append(row.key);
        }
    }
    QStringList keptNew;
    for (const Row &row : rows) {
        if (oldKeys.contains(row.key)) {
            keptNew.append(row.key);
        }
    }

    if ((keptOld != keptNew) || (newKeys.count() != rows.count()) || (oldKeys.count() != _rows.count())) {
        beginResetModel();
        _rows = rows;
        endResetModel();
    } else {
        // Removed in ranges from the end, then the rows kept are in their new order
        for (qsizetype last = _rows.count() - 1; last >= 0; last--) {
            if (newKeys.contains(_rows[last].key)) {
                continue;
            }
            qsizetype first = last;
            while ((first > 0) && !newKeys.contains(_rows[first - 1].key)) {
                first--;
            }
            beginRemoveRows(QModelIndex(), static_cast<int>(first), static_cast<int>(last));
            _rows.remove(first, last - first + 1);
            endRemoveRows();
            last = first;
        }

        for (qsizetype first = 0; first < rows.count(); first++) {
            if (oldKeys.contains(rows[first].key)) {
                continue;
            }
            qsizetype last = first;
            while (((last + 1) < rows.count()) && !oldKeys.contains(rows[last + 1].key)) {
                last++;
            }
            beginInsertRows(QModelIndex(), static_cast<int>(first), static_cast<int>(last));
            for (qsizetype i = first; i <= last; i++) {
                _rows.insert(i, rows[i]);
            }
            endInsertRows();
            first = last;
        }

        for (qsizetype first = 0; first < rows.count(); first++) {
            if (_rows[first] == rows[first]) {
                continue;
            }
            qsizetype last = first;
            while (((last + 1) < rows.count()) && !(_rows[last + 1] == rows[last + 1])) {
                last++;
            }
            for (qsizetype i = first; i <= last; i++) {
                _rows[i] = rows[i];
            }
            emit dataChanged(index(static_cast<int>(first)), index(static_cast<int>(last)));
            first = last;
        }
    }

    if (count() != previousCount) {
        emit countChanged();
    }
}

int GeoZoneFenceChoiceModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant GeoZoneFenceChoiceModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || (index.row() >= _rows.count())) {
        return QVariant();
    }
    const Row &row = _rows[index.row()];
    switch (role) {
    case KeyRole:
        return row.key;
    case NameRole:
        return row.name;
    case RestrictionRole:
        return row.restriction;
    case RestrictionTextRole:
        return row.restrictionText;
    case ProximityTextRole:
        return row.proximityText;
    case EnteredRole:
        return row.entered;
    case PointsRole:
        return row.points;
    case SimplifyToleranceRole:
        return row.simplifyTolerance;
    case SelectedRole:
        return row.selected;
    case IncludedRole:
        return row.included;
    case IncludableRole:
        return row.includable;
    case ReasonRole:
        return row.reason;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GeoZoneFenceChoiceModel::roleNames() const
{
    return {
        { KeyRole,                  "key" },
        { NameRole,                 "name" },
        { RestrictionRole,          "restriction" },
        { RestrictionTextRole,      "restrictionText" },
        { ProximityTextRole,        "proximityText" },
        { EnteredRole,              "entered" },
        { PointsRole,               "points" },
        { SimplifyToleranceRole,    "simplifyTolerance" },
        { SelectedRole,             "selected" },
        { IncludedRole,             "included" },
        { IncludableRole,           "includable" },
        { ReasonRole,               "reason" },
    };
}
