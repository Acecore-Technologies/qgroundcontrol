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
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"
#include "GeoZoneAlertTracker.h"
#include "GeoZoneNearbyModel.h"

Q_DECLARE_LOGGING_CATEGORY(GeoZoneMonitorLog)

Q_MOC_INCLUDE("GeoZoneGuidedCheck.h")

class GeoZoneAlertLog;
class GeoZoneGuidedCheck;
class GeoZoneManager;
class GeoZoneSource;
class Vehicle;

/// Alerts the user once when a vehicle approaches or enters an active restricted geozone or U-space airspace,
/// considering the zone altitude limits. Every connected vehicle is monitored with a GeoZoneAlertTracker, the nearby
/// zones and vertical bands shown are those of the active vehicle. Alerts and the approach distances are set in
/// GeoZoneSettings.
class GeoZoneMonitor : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool         monitoring          READ monitoring         NOTIFY monitoringChanged FINAL)       ///< Alerts are enabled and vehicles are monitored against loaded zones
    Q_PROPERTY(QVariantList nearbyZones         READ nearbyZones        NOTIFY nearbyZonesChanged FINAL)      ///< Maps with zone, inside and distance (meters), most restrictive first
    Q_PROPERTY(int          alertRestriction    READ alertRestriction   NOTIFY nearbyZonesChanged FINAL)      ///< Most restrictive GeoZone::Restriction nearby, -1 without
    Q_PROPERTY(bool         inside              READ inside             NOTIFY nearbyZonesChanged FINAL)      ///< The vehicle is inside one of the nearby zones
    Q_PROPERTY(GeoZoneNearbyModel *nearbyModel  READ nearbyModel        CONSTANT FINAL)                       ///< nearbyZones as a list model
    Q_PROPERTY(GeoZoneGuidedCheck *guidedCheck  READ guidedCheck        CONSTANT FINAL)                       ///< Checks of guided commands
    /// Volumes of the active zones above and below the active vehicle, as maps with restriction, name, limitsText, floor and
    /// ceiling (meters AMSL, NaN when the zone extends to the ground or has no ceiling, or the limit is unknown)
    Q_PROPERTY(QVariantList verticalBands       READ verticalBands      NOTIFY verticalBandsChanged FINAL)
    Q_PROPERTY(double       altitudeAMSL        READ altitudeAMSL       NOTIFY verticalBandsChanged FINAL)    ///< Of the active vehicle, NaN when unknown
    Q_PROPERTY(double       terrainAMSL         READ terrainAMSL        NOTIFY verticalBandsChanged FINAL)    ///< Below the active vehicle, NaN when unknown

public:
    /// Monitors the vehicles of MultiVehicleManager against the zones of GeoZoneManager::instance() and logs the alerts
    explicit GeoZoneMonitor(QObject *parent = nullptr);

    /// Monitors positions passed to evaluate() against the zones of the manager
    explicit GeoZoneMonitor(GeoZoneManager *manager, QObject *parent = nullptr);

    static GeoZoneMonitor *instance();

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
        double timeToEntry = qQNaN();   ///< Seconds until the predicted entry of an approach alert, NaN when not predicted
    };

    using Velocity = GeoZoneAlertTracker::Velocity;

    /// Updates the alert state of the zones for a position of the vehicle. Unknown altitudes are inside all
    /// vertical limits. The nearby zones follow the active vehicle, or positions without a vehicle.
    /// With a valid velocity, the vehicle is also alerted when it would enter a zone within the approach time.
    void evaluate(const GeoZone::Position &position, int vehicleId, const Velocity &velocity);
    void evaluate(const GeoZone::Position &position, int vehicleId = noVehicle) { evaluate(position, vehicleId, Velocity()); }

    /// @return Position of the vehicle with its altitude above mean sea level and the terrain elevation from its
    ///         altitude above terrain, which is unknown without terrain data
    static GeoZone::Position vehiclePosition(Vehicle *vehicle, const QGeoCoordinate &coordinate);

    GeoZoneManager *manager() const { return _manager; }
    GeoZoneGuidedCheck *guidedCheck() const { return _guidedCheck; }

    bool monitoring() const;
    QVariantList nearbyZones() const;
    int alertRestriction() const;
    bool inside() const;
    GeoZoneNearbyModel *nearbyModel() const { return _nearbyModel; }
    QVariantList verticalBands() const;
    double altitudeAMSL() const;
    double terrainAMSL() const;

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
    void verticalBandsChanged();

private slots:
    void _vehicleAdded(Vehicle *vehicle);
    void _vehicleRemoved(Vehicle *vehicle);
    void _activeVehicleChanged(Vehicle *vehicle);
    void _evaluateVehicles();
    void _updateEvaluateTimer();
    void _forgetZones(GeoZoneSource *source);
    void _alertsEnabledChanged();

private:
    using Nearby = GeoZoneAlertTracker::Nearby;

    /// Monitoring state of one vehicle
    struct Tracker {
        GeoZoneAlertTracker alerts;
        GeoZoneVelocityEstimator velocity;
        QList<Nearby> nearby;
        QList<GeoZoneVerticalBand> bands;
        GeoZone::Position position;     ///< Last evaluated position
    };

    const Tracker *_activeTracker() const;
    AlertEvent _alertEvent(const GeoZoneAlertTracker::Alert &alert, const GeoZone::Position &position, int vehicleId) const;

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
    QElapsedTimer _clock;
    GeoZoneAlertLog *_alertLog = nullptr;
    GeoZoneNearbyModel *_nearbyModel = nullptr;
    GeoZoneGuidedCheck *_guidedCheck = nullptr;
};
