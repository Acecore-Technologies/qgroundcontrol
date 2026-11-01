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
#include <QtCore/QHash>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"

Q_DECLARE_LOGGING_CATEGORY(GeoZoneMonitorLog)

/// Zones near the active vehicle with roles zone, inside and distance (meters). Rows are updated in place
/// while only the distances change, so views keep their delegates.
class GeoZoneNearbyModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    struct Item {
        GeoZone *zone = nullptr;
        bool inside = false;
        int distance = 0;   ///< Rounded meters

        friend bool operator==(const Item &, const Item &) = default;
    };

    enum Roles {
        ZoneRole = Qt::UserRole + 1,
        InsideRole,
        DistanceRole,
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

class GeoZoneAlertLog;
class GeoZoneManager;
class GeoZoneSource;
class Vehicle;

/// Alerts the user once when a vehicle approaches or enters an active restricted geozone or U-space airspace,
/// considering the zone altitude limits. Every connected vehicle is monitored, the nearby zones are those of the
/// active vehicle. Alerts and the approach distances are set in GeoZoneSettings.
class GeoZoneMonitor : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool         monitoring          READ monitoring         NOTIFY monitoringChanged FINAL)       ///< Alerts are enabled and vehicles are monitored against loaded zones
    Q_PROPERTY(QVariantList nearbyZones         READ nearbyZones        NOTIFY nearbyZonesChanged FINAL)      ///< Maps with zone, inside and distance (meters), most restrictive first
    Q_PROPERTY(int          alertRestriction    READ alertRestriction   NOTIFY nearbyZonesChanged FINAL)      ///< Most restrictive GeoZone::Restriction nearby, -1 without
    Q_PROPERTY(bool         inside              READ inside             NOTIFY nearbyZonesChanged FINAL)      ///< The vehicle is inside one of the nearby zones
    Q_PROPERTY(GeoZoneNearbyModel *nearbyModel  READ nearbyModel        CONSTANT FINAL)                       ///< nearbyZones as a list model

public:
    /// Monitors the vehicles of MultiVehicleManager against the zones of GeoZoneManager::instance() and logs the alerts
    explicit GeoZoneMonitor(QObject *parent = nullptr);

    /// Monitors positions passed to evaluate() against the zones of the manager
    explicit GeoZoneMonitor(GeoZoneManager *manager, QObject *parent = nullptr);

    static GeoZoneMonitor *instance();

    static constexpr double hysteresisDistance = 20;        ///< Meters to move away before an alert can repeat
    static constexpr int evaluateIntervalMSecs = 1000;
    static constexpr int noVehicle = -1;                    ///< Vehicle id of positions which don't belong to a vehicle

    /// An approach or entry alert
    struct AlertEvent {
        QDateTime time;             ///< UTC
        int vehicleId = noVehicle;
        GeoZone::Position position;
        bool inside = false;        ///< Entered, otherwise approached
        double distance = 0;        ///< Meters to the zone, 0 inside
        QString zoneIdentifier;
        QString zoneName;
        GeoZone::Restriction restriction = GeoZone::Unknown;
        QString sourceText;
        QString message;            ///< As shown to the user
    };

    /// Updates the alert state of the zones for a position of the vehicle. Unknown altitudes are inside all
    /// vertical limits. The nearby zones follow the active vehicle, or positions without a vehicle.
    void evaluate(const GeoZone::Position &position, int vehicleId = noVehicle);

    /// @return Position of the vehicle with its altitude above mean sea level and the terrain elevation from its
    ///         altitude above terrain, which is unknown without terrain data
    static GeoZone::Position vehiclePosition(Vehicle *vehicle, const QGeoCoordinate &coordinate);

    bool monitoring() const;
    QVariantList nearbyZones() const;
    int alertRestriction() const;
    bool inside() const;
    GeoZoneNearbyModel *nearbyModel() const { return _nearbyModel; }

    /// Vehicle whose nearby zones are published, noVehicle by default
    int activeVehicleId() const { return _activeVehicleId; }
    void setActiveVehicleId(int vehicleId);

    /// Forgets the alert state of a vehicle, so that its alerts repeat
    void removeVehicle(int vehicleId);

    /// Log of the alerts of vehicles, null for monitors of positions passed to evaluate()
    GeoZoneAlertLog *alertLog() const { return _alertLog; }

signals:
    /// Emitted once when a zone is approached and once when it is entered. The message names the vehicle when
    /// several vehicles are monitored. The alerts of an evaluation are emitted most severe first.
    /// @param severity GeoZone::restrictionSeverity of the zone
    void alert(const QString &message, int severity);
    void alertEvent(const GeoZoneMonitor::AlertEvent &event);
    void monitoringChanged();
    void nearbyZonesChanged();

private slots:
    void _vehicleAdded(Vehicle *vehicle);
    void _vehicleRemoved(Vehicle *vehicle);
    void _activeVehicleChanged(Vehicle *vehicle);
    void _evaluateVehicles();
    void _updateEvaluateTimer();
    void _forgetZones(GeoZoneSource *source);
    void _alertsEnabledChanged();

private:
    enum class State {
        Away,
        Near,
        Inside,
    };

    using Nearby = GeoZoneNearbyModel::Item;

    /// Alert state of one vehicle
    struct Tracker {
        QHash<QString, State> states;   ///< Zones which are not Away by GeoZone::key, which stays the same when a source reloads
        QList<Nearby> nearby;
    };

    static void _speak(const QString &message);
    const QList<Nearby> &_activeNearby() const;
    void _publishNearby();
    QString _vehicleLabel(int vehicleId) const;
    void _resetTrackers();

    GeoZoneManager *_manager = nullptr;
    QHash<int, Tracker> _trackers;              ///< By vehicle id
    QList<QPointer<Vehicle>> _vehicles;         ///< Monitored vehicles of MultiVehicleManager
    int _activeVehicleId = noVehicle;
    QTimer _evaluateTimer;
    GeoZoneAlertLog *_alertLog = nullptr;
    GeoZoneNearbyModel *_nearbyModel = nullptr;
};
