/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include <functional>
#include <optional>

#include "GeoZoneAlertTracker.h"
#include "GeoZoneFencePrediction.h"
#include "QGCMAVLink.h"

class Vehicle;

/// Warns the pilot before the vehicle's own fence makes it act (EN 4709-003 6.2.2). While the vehicle's fence is enabled, each position the vehicle reports (GLOBAL_POSITION_INT)
/// is checked with its velocity against the fence the vehicle stores (its GeoFenceManager) and the circle around home
/// and altitude limits of its parameters, see GeoZoneFencePrediction: when the vehicle would breach it within
/// warningTime, the warning is active with the manoeuvre the autopilot then takes (PX4 GF_ACTION, ArduPilot
/// FENCE_ACTION), the time and the distance to the fence along the velocity and where the vehicle reaches it.
/// GeoZoneMonitor shows, speaks and logs it. One per vehicle, see GeoZoneVehicleMonitor::fenceWarning.
class GeoZoneFenceWarning : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool             active          READ active             NOTIFY warningChanged FINAL)    ///< A breach is predicted within warningTime
    Q_PROPERTY(QString          text            READ text               NOTIFY warningChanged FINAL)    ///< Like "Geofence: RTL or land in 6 s, 42 m ahead", empty when inactive
    Q_PROPERTY(int              timeToFence     READ timeToFence        NOTIFY warningChanged FINAL)    ///< Seconds, -1 when inactive
    Q_PROPERTY(double           distanceToFence READ distanceToFence    NOTIFY warningChanged FINAL)    ///< Meters along the velocity, NaN when inactive
    Q_PROPERTY(QGeoCoordinate   intersection    READ intersection       NOTIFY warningChanged FINAL)    ///< Where the vehicle reaches the fence, invalid when inactive
    Q_PROPERTY(QString          intersectionText READ intersectionText  NOTIFY warningChanged FINAL)    ///< Its coordinate and altitude
    Q_PROPERTY(QVariantList     path            READ path               NOTIFY warningChanged FINAL)    ///< From the vehicle to the intersection, empty when inactive
    Q_PROPERTY(bool             enabled         READ enabled            NOTIFY configChanged FINAL)     ///< The vehicle's fence is enabled and has boundaries
    Q_PROPERTY(QString          actionText      READ actionText         NOTIFY configChanged FINAL)     ///< Manoeuvre of the vehicle at a breach, like "RTL or land"

public:
    using Velocity = GeoZoneAlertTracker::Velocity;
    /// @return Monotonic milliseconds
    using Clock = std::function<qint64()>;

    static constexpr double reactionTime = 5;       ///< Seconds a pilot takes to react, EN 4709-003 Annex A
    static constexpr double warningMargin = 3;      ///< Seconds the pilot has more to act on the warning
    static constexpr double warningTime = reactionTime + warningMargin;    ///< Before the predicted breach
    static constexpr double endDelay = 2;           ///< Seconds after warningTime an active warning ends, so it doesn't flicker
    static constexpr int minEvaluateIntervalMSecs = 250;    ///< Between the positions evaluated

    enum class Firmware {
        Other,
        PX4,
        ArduPilot,
    };

    /// Type of vehicle, whose autopilot has its own fence actions and flight modes
    enum class VehicleClass {
        Copter,     ///< Multirotors, helicopters and VTOLs hovering
        Plane,      ///< Fixed wings and VTOLs in forward flight
        Rover,      ///< Rovers and boats
        Sub,
    };

    /// What the vehicle's fence is and does
    struct Config {
        bool enabled = false;       ///< The vehicle enforces the fence
        GeoZoneFencePrediction::Fence fence;
        Firmware firmware = Firmware::Other;
        VehicleClass vehicleClass = VehicleClass::Copter;
        int action = -1;            ///< GF_ACTION or FENCE_ACTION, -1 when unknown
    };

    /// @param vehicle Whose fence and positions are followed, null for a config set with setConfig only
    /// @param clock Time of the vehicle's messages
    explicit GeoZoneFenceWarning(Vehicle *vehicle, const Clock &clock = Clock(), QObject *parent = nullptr);

    /// @return The fence of the vehicle: its polygons and circles, the circle around home and the altitude limits
    ///         its parameters enable (ArduPilot FENCE_TYPE, FENCE_RADIUS, FENCE_ALT_MAX, FENCE_ALT_MIN, FENCE_MARGIN,
    ///         FENCE_OPTIONS; PX4 GF_MAX_HOR_DIST, GF_MAX_VER_DIST) and whether it is enabled (ArduPilot FENCE_ENABLE,
    ///         or FENCE_AUTOENABLE while armed; PX4 unless GF_ACTION is None)
    static Config vehicleConfig(Vehicle *vehicle);
    /// Uses the config instead of the vehicle's, like tests do
    void setConfig(const Config &config);
    const Config &config() const { return _config; }

    /// Evaluates the position of GLOBAL_POSITION_INT messages, at most every minEvaluateIntervalMSecs, with the
    /// velocity the vehicle reports or, when it reports none, estimated from its positions
    void handleMessage(const mavlink_message_t &message, qint64 msecs);
    /// Evaluates a position with its velocity
    /// @param altitude Meters above home, NaN when unknown
    void evaluate(const QGeoCoordinate &coordinate, double altitude, const Velocity &velocity);

    bool active() const { return _breach.has_value(); }
    const std::optional<GeoZoneFencePrediction::Breach> &breach() const { return _breach; }
    QString text() const;
    int timeToFence() const;
    double distanceToFence() const { return _breach ? _breach->distance : qQNaN(); }
    QGeoCoordinate intersection() const { return _breach ? _breach->coordinate : QGeoCoordinate(); }
    QString intersectionText() const;
    QVariantList path() const;
    bool enabled() const { return _config.enabled && !_config.fence.isEmpty(); }
    QString actionText() const { return actionText(_config.firmware, _config.vehicleClass, _config.action); }
    /// Where the vehicle was when the warning was evaluated last
    QGeoCoordinate position() const { return _position; }

    /// @return The manoeuvre of the action, like "RTL or land", "Report only (no manoeuvre)"
    static QString actionText(Firmware firmware, VehicleClass vehicleClass, int action);
    /// @return true if the action makes the vehicle act, rather than only report the breach
    static bool isManoeuvre(Firmware firmware, int action);
    /// @return Like "Geofence: RTL or land in 6 s, 42 m ahead"
    static QString warningText(const QString &action, double time, double distance);

signals:
    void warningChanged();
    /// A breach became predicted within warningTime
    void warningStarted(const QString &text);
    void warningEnded();
    void configChanged();

private:
    void _connectVehicle();
    void _updateConfig();
    void _setBreach(const std::optional<GeoZoneFencePrediction::Breach> &breach);

    QPointer<Vehicle> _vehicle;
    Clock _clock;
    Config _config;
    bool _fixedConfig = false;
    QObject *_parameterConnections = nullptr;   ///< Receiver of the fence parameters' changes, replaced to disconnect them
    std::optional<GeoZoneFencePrediction::Breach> _breach;
    QGeoCoordinate _position;
    qint64 _evaluatedMSecs = -1;
    GeoZoneVelocityEstimator _estimator;
};
