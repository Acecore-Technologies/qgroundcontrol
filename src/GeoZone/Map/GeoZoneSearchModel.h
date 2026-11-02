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
#include <QtCore/QPointer>
#include <QtCore/QString>

#include "GeoZoneInfo.h"

class GeoZoneManager;
class QTimer;

/// The loaded zones found by a search text, see GeoZoneCatalog::search, for the search fields of the maps, with roles
/// zone (GeoZoneInfo), displayName, identifier, restriction and restrictionText. The zones are searched again once
/// the text settles and when the zones change.
class GeoZoneSearchModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString  text        READ text       WRITE setText       NOTIFY textChanged FINAL)
    Q_PROPERTY(int      maxResults  READ maxResults WRITE setMaxResults NOTIFY maxResultsChanged FINAL)
    Q_PROPERTY(int      count       READ count                          NOTIFY countChanged FINAL)
    Q_PROPERTY(bool     hasMore     READ hasMore                        NOTIFY countChanged FINAL)    ///< More zones match than maxResults
    Q_PROPERTY(bool     pending     READ pending                        NOTIFY pendingChanged FINAL)  ///< The text changed and the zones aren't searched yet

public:
    enum Roles : int {
        ZoneRole = Qt::UserRole + 1,
        DisplayNameRole,
        IdentifierRole,
        RestrictionRole,
        RestrictionTextRole,
        SourceCountRole,    ///< Sources publishing the zone, see GeoZone::sourceCount
    };
    Q_ENUM(Roles)

    explicit GeoZoneSearchModel(QObject *parent = nullptr);
    /// @param manager Zones searched, GeoZoneManager::instance() when null
    explicit GeoZoneSearchModel(GeoZoneManager *manager, QObject *parent = nullptr);
    ~GeoZoneSearchModel() override;

    QString text() const { return _text; }
    void setText(const QString &text);
    int maxResults() const { return _maxResults; }
    void setMaxResults(int maxResults);
    int count() const { return static_cast<int>(_zones.count()); }
    bool hasMore() const { return _hasMore; }
    bool pending() const;
    const QList<GeoZoneInfo> &zones() const { return _zones; }

    /// Sets how long the text settles before the zones are searched, 300 ms by default
    void setDelay(int msecs);
    /// Searches the zones now
    Q_INVOKABLE void update();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void textChanged();
    void maxResultsChanged();
    void countChanged();
    void pendingChanged();

private:
    QPointer<GeoZoneManager> _manager;
    QTimer *const _timer;
    QString _text;
    int _maxResults = 8;
    QList<GeoZoneInfo> _zones;
    bool _hasMore = false;
};
