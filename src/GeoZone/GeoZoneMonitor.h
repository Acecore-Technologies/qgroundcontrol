/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

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

class GeoZoneAlertLog;
class GeoZoneManager;
class Vehicle;

/// Alerts the user once when a vehicle approaches or enters an active restricted geozone or U-space airspace,
/// considering the zone altitude limits. Every connected vehicle is monitored, the nearby zones are those of the
/// active vehicle. Alerts and the approach distances are set in GeoZoneSettings.
class GeoZoneMonitor : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool         monitoring          READ monitoring         NOTIFY monitoringChanged)       ///< Alerts are enabled and vehicles are monitored against loaded zones
    Q_PROPERTY(QVariantList nearbyZones         READ nearbyZones        NOTIFY nearbyZonesChanged)      ///< Maps with zone, inside and distance (meters), most restrictive first
    Q_PROPERTY(int          alertRestriction    READ alertRestriction   NOTIFY nearbyZonesChanged)      ///< Most restrictive GeoZone::Restriction nearby, -1 without
    Q_PROPERTY(bool         inside              READ inside             NOTIFY nearbyZonesChanged)      ///< The vehicle is inside one of the nearby zones

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

    /// Vehicle whose nearby zones are published, noVehicle by default
    int activeVehicleId() const { return _activeVehicleId; }
    void setActiveVehicleId(int vehicleId);

    /// Forgets the alert state of a vehicle, so that its alerts repeat
    void removeVehicle(int vehicleId);

    /// Log of the alerts of vehicles, null for monitors of positions passed to evaluate()
    GeoZoneAlertLog *alertLog() const { return _alertLog; }

signals:
    /// Emitted once when a zone is approached and once when it is entered. The message names the vehicle when
    /// several vehicles are monitored.
    void alert(const QString &message);
    void alertEvent(const GeoZoneMonitor::AlertEvent &event);
    void monitoringChanged();
    void nearbyZonesChanged();

private slots:
    void _vehicleAdded(Vehicle *vehicle);
    void _vehicleRemoved(Vehicle *vehicle);
    void _activeVehicleChanged(Vehicle *vehicle);
    void _evaluateVehicles();
    void _updateEvaluateTimer();
    void _forgetZones();
    void _alertsEnabledChanged();

private:
    enum class State {
        Away,
        Near,
        Inside
    };

    struct Nearby {
        GeoZone *zone = nullptr;
        bool inside = false;
        int distance = 0;   ///< Rounded meters

        bool operator==(const Nearby &other) const = default;
    };

    /// Alert state of one vehicle
    struct Tracker {
        QHash<QString, State> states;   ///< Zones which are not Away by _zoneKey, which stays the same when a source reloads
        QList<Nearby> nearby;
    };

    static void _speak(const QString &message);
    static QString _zoneKey(const GeoZone *zone);
    const QList<Nearby> &_activeNearby() const;
    QString _vehicleLabel(int vehicleId) const;
    void _resetTrackers();

    GeoZoneManager *_manager = nullptr;
    QHash<int, Tracker> _trackers;              ///< By vehicle id
    QList<QPointer<Vehicle>> _vehicles;         ///< Monitored vehicles of MultiVehicleManager
    int _activeVehicleId = noVehicle;
    QTimer _evaluateTimer;
    GeoZoneAlertLog *_alertLog = nullptr;
};
