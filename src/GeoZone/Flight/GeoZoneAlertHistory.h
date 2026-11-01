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
#include <QtCore/QTimer>
#include <QtPositioning/QGeoCoordinate>

#include <optional>

#include "GeoZoneMonitor.h"

class GeoZoneManager;

/// Geozone alerts of this session, newest first, with those logged earlier today, for reviewing and acknowledging
/// them after a flight or while the alert popups are gone
class GeoZoneAlertHistory : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int              count               READ count                  NOTIFY countChanged FINAL)
    Q_PROPERTY(int              unacknowledgedCount READ unacknowledgedCount    NOTIFY unacknowledgedCountChanged FINAL)
    Q_PROPERTY(QGeoCoordinate   shownPosition       READ shownPosition          NOTIFY shownPositionChanged FINAL)  ///< Vehicle position of the alert shown on the map, invalid when none

public:
    enum Roles {
        TimeRole = Qt::UserRole + 1,    ///< Local time
        MessageRole,
        ZoneNameRole,
        RestrictionRole,                ///< GeoZone::Restriction
        InsideRole,                     ///< Entered, otherwise approached
        CoordinateRole,                 ///< Of the vehicle
        AltitudeRole,                   ///< Of the vehicle in meters AMSL, NaN when unknown
        AcknowledgedRole,
        EarlierRole,                    ///< Logged by an earlier session
    };

    static constexpr int maxEntries = 500;
    static constexpr int shownPositionMSecs = 2 * 60 * 1000;   ///< How long the map shows the position of a shown alert

    /// @param manager Zones the alerts are shown on, GeoZoneManager::instance() when null
    explicit GeoZoneAlertHistory(GeoZoneManager *manager = nullptr, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(_entries.count()); }
    int unacknowledgedCount() const;
    QGeoCoordinate shownPosition() const { return _shownPosition; }

    /// Adds the alerts of a log file written by GeoZoneAlertLog before this session as acknowledged entries
    void loadLog(const QString &filePath);

    Q_INVOKABLE void acknowledge(int row);
    Q_INVOKABLE void acknowledgeAll();
    /// Acknowledges the alert by GeoZoneMonitor::AlertEvent::id
    void acknowledgeAlert(int alertId);
    /// Shows the zone of the alert with its details and the vehicle position on the visible map
    Q_INVOKABLE void show(int row);
    Q_INVOKABLE void clearShownPosition();

    /// @return The alert of a line of a GeoZoneAlertLog file, nothing when it can't be read
    static std::optional<GeoZoneMonitor::AlertEvent> parseLogLine(const QString &line);

public slots:
    void append(const GeoZoneMonitor::AlertEvent &event);

signals:
    void countChanged();
    void unacknowledgedCountChanged();
    /// An alert of the session was acknowledged, by GeoZoneMonitor::AlertEvent::id
    void alertAcknowledged(int alertId);
    void allAcknowledged();
    void shownPositionChanged();

private:
    struct Entry {
        GeoZoneMonitor::AlertEvent event;
        bool acknowledged = false;
        bool earlier = false;
    };

    GeoZoneManager *_manager = nullptr;
    QList<Entry> _entries;          ///< Newest first
    QGeoCoordinate _shownPosition;
    QTimer _shownTimer;
};
