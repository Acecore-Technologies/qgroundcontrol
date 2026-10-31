/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"

Q_DECLARE_LOGGING_CATEGORY(GeoZoneMonitorLog)

class GeoZoneManager;
class Vehicle;

/// Alerts the user once when the active vehicle approaches or enters an active restricted geozone,
/// considering the zone altitude limits.
class GeoZoneMonitor : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool         monitoring          READ monitoring         NOTIFY monitoringChanged)       ///< A vehicle is monitored against loaded zones
    Q_PROPERTY(QVariantList nearbyZones         READ nearbyZones        NOTIFY nearbyZonesChanged)      ///< Maps with zone, inside and distance (meters), most relevant first
    Q_PROPERTY(int          alertRestriction    READ alertRestriction   NOTIFY nearbyZonesChanged)      ///< GeoZone::Restriction of the first nearby zone, -1 without
    Q_PROPERTY(bool         inside              READ inside             NOTIFY nearbyZonesChanged)      ///< The vehicle is inside a nearby zone

public:
    /// Monitors the active vehicle against the zones of GeoZoneManager::instance()
    explicit GeoZoneMonitor(QObject *parent = nullptr);

    /// Monitors coordinates passed to evaluate() against the zones of the manager
    explicit GeoZoneMonitor(GeoZoneManager *manager, QObject *parent = nullptr);

    static GeoZoneMonitor *instance();

    static constexpr double approachDistance = 100;         ///< Meters horizontally
    static constexpr double approachVerticalDistance = 30;  ///< Meters below a zone floor or above its ceiling
    static constexpr double hysteresisDistance = 20;        ///< Meters to move away before an alert can repeat
    static constexpr int evaluateIntervalMSecs = 1000;

    /// Updates the alert state of the zones for the vehicle position. Unknown altitudes are inside all
    /// vertical limits.
    void evaluate(const GeoZone::Position &position);

    /// @return Position of the vehicle with its altitude above mean sea level and the terrain elevation from its
    ///         altitude above terrain, which is unknown without terrain data
    static GeoZone::Position vehiclePosition(Vehicle *vehicle, const QGeoCoordinate &coordinate);

    bool monitoring() const;
    QVariantList nearbyZones() const;
    int alertRestriction() const;
    bool inside() const;

signals:
    /// Emitted once when a zone is approached and once when it is entered
    void alert(const QString &message);
    void monitoringChanged();
    void nearbyZonesChanged();

private slots:
    void _activeVehicleChanged(Vehicle *vehicle);
    void _evaluateVehicle();
    void _updateEvaluateTimer();
    void _reset();

private:
    enum class State {
        Away,
        Near,
        Inside
    };

    static void _showAlert(const QString &message);

    struct Nearby {
        GeoZone *zone = nullptr;
        bool inside = false;
        int distance = 0;   ///< Rounded meters

        bool operator==(const Nearby &other) const = default;
    };

    void _setNearby(const QList<Nearby> &nearby);

    GeoZoneManager *_manager = nullptr;
    QPointer<Vehicle> _vehicle;
    QHash<const GeoZone*, State> _states;   ///< Zones which are not Away
    QList<Nearby> _nearby;
    QTimer _evaluateTimer;
};
