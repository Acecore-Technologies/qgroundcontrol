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
#include <QtCore/QString>

/// Zones of a fence selection, see GeoZoneFenceImporter, with roles key, name, restriction, restrictionText,
/// proximityText, entered, points, simplifyTolerance, selected, included, includable and reason. Views filter the selected zones
/// and those left out by SelectedRole. Rows keep their order relative to each other from one selection to the next, so
/// rows are inserted, removed and updated in place: views keep their delegates and their scroll position while zones
/// move between lists.
class GeoZoneFenceChoiceModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    struct Row {
        QString key;                    ///< See GeoZone::key
        QString name;
        int restriction = 0;            ///< GeoZone::Restriction
        QString restrictionText;
        QString proximityText;          ///< Like "1.2 km from the flight"
        bool entered = false;           ///< The planned flight enters the zone
        int points = 0;                 ///< Fence items
        double simplifyTolerance = 0;   ///< Meters
        bool selected = false;
        bool included = true;           ///< Not left out by the user
        bool includable = true;         ///< The user may include or leave out the zone, see GeoZoneFences::Choice::includable
        QString reason;                 ///< Why the zone is left out

        friend bool operator==(const Row &, const Row &) = default;
    };

    enum Roles : int {
        KeyRole = Qt::UserRole + 1,
        NameRole,
        RestrictionRole,
        RestrictionTextRole,
        ProximityTextRole,
        EnteredRole,
        PointsRole,
        SimplifyToleranceRole,
        SelectedRole,
        IncludedRole,
        IncludableRole,
        ReasonRole,
    };
    Q_ENUM(Roles)

    explicit GeoZoneFenceChoiceModel(QObject *parent = nullptr);

    int count() const { return static_cast<int>(_rows.count()); }
    const QList<Row> &rows() const { return _rows; }
    /// Replaces the rows, those with the same key in place
    void setRows(const QList<Row> &rows);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    QList<Row> _rows;
};
