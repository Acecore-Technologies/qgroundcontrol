/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtPositioning/QGeoCoordinate>

Q_DECLARE_LOGGING_CATEGORY(GeoZoneMonitorLog)

class GeoZone;
class GeoZoneManager;
class Vehicle;

/// Alerts the user once when the active vehicle approaches or enters an active restricted geozone.
/// Altitude is not considered.
class GeoZoneMonitor : public QObject
{
    Q_OBJECT

public:
    /// Monitors the active vehicle against the zones of GeoZoneManager::instance()
    explicit GeoZoneMonitor(QObject *parent = nullptr);

    /// Monitors coordinates passed to evaluate() against the zones of the manager
    explicit GeoZoneMonitor(GeoZoneManager *manager, QObject *parent = nullptr);

    static GeoZoneMonitor *instance();

    static constexpr double approachDistance = 100;     ///< Meters
    static constexpr double hysteresisDistance = 20;    ///< Meters to move away before an alert can repeat
    static constexpr int minEvaluateIntervalMSecs = 1000;

    /// Updates the alert state of the zones for the vehicle position
    void evaluate(const QGeoCoordinate &coordinate);

signals:
    /// Emitted once when a zone is approached and once when it is entered
    void alert(const QString &message);

private slots:
    void _activeVehicleChanged(Vehicle *vehicle);
    void _vehicleCoordinateChanged(const QGeoCoordinate &coordinate);
    void _reset();

private:
    enum class State {
        Away,
        Near,
        Inside
    };

    static void _showAlert(const QString &message);

    GeoZoneManager *_manager = nullptr;
    QPointer<Vehicle> _vehicle;
    QHash<const GeoZone*, State> _states;   ///< Zones which are not Away
    QElapsedTimer _lastEvaluate;
};
