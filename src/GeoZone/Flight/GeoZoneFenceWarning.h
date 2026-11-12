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
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include <functional>
#include <optional>

#include "GeoZoneAlertTracker.h"
#include "GeoZoneFencePrediction.h"
#include "QGCMAVLink.h"

class Vehicle;

/// Warns the pilot before the vehicle's own fence makes it act (EN 4709-003 6.2.2), and lets the pilot override the
/// fence action (6.2.1). While the vehicle's fence is enabled, each position the vehicle reports (GLOBAL_POSITION_INT)
/// is checked with its velocity against the fence the vehicle stores (its GeoFenceManager) and the circle around home
/// and altitude limits of its parameters, see GeoZoneFencePrediction: when the vehicle would breach it within
/// warningTime(), the warning is active with the manoeuvre the autopilot then takes (PX4 GF_ACTION, ArduPilot
/// FENCE_ACTION), the time and the distance to the fence along the velocity and where the vehicle reaches it.
/// GeoZoneMonitor shows, speaks and logs it. One per vehicle, see GeoZoneVehicleMonitor::fenceWarning.
///
/// Once the vehicle is past the fence nothing is predicted any more: outside() then tells that its position is outside
/// the fence, which GeoZoneVehicleMonitor takes as the breach of autopilots which don't report theirs, like PX4.
///
/// QGroundControl forgets the fence of the vehicle when its upload, download or removal fails, while the vehicle may
/// keep the fence it had. The fence is then unknown, see fenceUnknown: the warning keeps the fence it knew, and reads
/// the fence back from the vehicle once no plan transfer is in progress.
///
/// The pilot overrides the fence action by changing the flight mode, which takeControl() does with the mode the pilot
/// flies in for the vehicle type, or on ArduPilot by disabling the fence (MAV_CMD_DO_FENCE_ENABLE). A fence the vehicle
/// accepted to disable isn't warned about until it is enabled again, see fenceDisabled.
class GeoZoneFenceWarning : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool             active          READ active             NOTIFY warningChanged FINAL)    ///< A breach is predicted within warningTime()
    Q_PROPERTY(QString          text            READ text               NOTIFY warningChanged FINAL)    ///< Like "Geofence: RTL or land in 6 s, 42 m ahead", empty when inactive
    Q_PROPERTY(int              timeToFence     READ timeToFence        NOTIFY warningChanged FINAL)    ///< Seconds, -1 when inactive
    Q_PROPERTY(double           distanceToFence READ distanceToFence    NOTIFY warningChanged FINAL)    ///< Meters along the velocity, NaN when inactive
    Q_PROPERTY(QGeoCoordinate   intersection    READ intersection       NOTIFY warningChanged FINAL)    ///< Where the vehicle reaches the fence, invalid when inactive
    Q_PROPERTY(QString          intersectionText READ intersectionText  NOTIFY warningChanged FINAL)    ///< Its coordinate and altitude
    Q_PROPERTY(QVariantList     path            READ path               NOTIFY warningChanged FINAL)    ///< From the vehicle to the intersection, empty when inactive
    Q_PROPERTY(bool             outside         READ outside            NOTIFY outsideChanged FINAL)    ///< The vehicle is outside its fence
    Q_PROPERTY(bool             enabled         READ enabled            NOTIFY configChanged FINAL)     ///< The vehicle enforces its fence, which has boundaries
    Q_PROPERTY(bool             fenceUnknown    READ fenceUnknown       NOTIFY fenceUnknownChanged FINAL)   ///< The fence the vehicle stores couldn't be read
    Q_PROPERTY(QString          actionText      READ actionText         NOTIFY configChanged FINAL)     ///< Manoeuvre of the vehicle at a breach, like "RTL or land"
    Q_PROPERTY(QString          overrideText    READ overrideText       NOTIFY configChanged FINAL)     ///< How the pilot overrides it
    Q_PROPERTY(QString          takeControlMode READ takeControlMode    NOTIFY configChanged FINAL)     ///< Flight mode takeControl() switches to, empty when there is none
    Q_PROPERTY(bool             canTakeControl  READ canTakeControl     NOTIFY canTakeControlChanged FINAL) ///< The vehicle would switch to takeControlMode now
    Q_PROPERTY(bool             canDisableFence READ canDisableFence    NOTIFY configChanged FINAL)     ///< The autopilot accepts disableFence()
    Q_PROPERTY(QString          commandText     READ commandText        NOTIFY commandTextChanged FINAL)    ///< Result of the last override
    /// The vehicle accepted disableFence(): until it accepts enableFence(), reports its fence enabled again or is
    /// armed with FENCE_AUTOENABLE
    Q_PROPERTY(bool             fenceDisabled   READ fenceDisabled      NOTIFY fenceDisabledChanged FINAL)

public:
    using Velocity = GeoZoneAlertTracker::Velocity;
    /// @return Monotonic milliseconds
    using Clock = std::function<qint64()>;

    static constexpr double defaultWarningTime = 8; ///< Seconds, the default profile's pilot reaction time and fence warning margin
    static constexpr double endDelay = 2;           ///< Seconds after warningTime() an active warning ends, so it doesn't flicker
    static constexpr int minEvaluateIntervalMSecs = 250;    ///< Between the positions evaluated
    /// The vehicle counts as outside its fence until it has been back inside for this long, so that a position at the
    /// boundary isn't a new breach with every message
    static constexpr int outsideEndDelayMSecs = 2000;
    /// Times the fence is read back from the vehicle after a failed transfer, until a transfer succeeds
    static constexpr int maxFenceReloads = 3;
    /// The vehicle reports a flight mode it switched to with its next heartbeat
    static constexpr int modeSwitchTimeoutMSecs = 3000;
    /// Longer than the vehicle is asked again for the answer to a command, which without a link never comes
    static constexpr int fenceCommandTimeoutMSecs = 15000;

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

    /// The polygons and circles the vehicle stores
    struct StoredFence {
        QList<GeoZoneFencePrediction::Fence::Polygon> polygons;
        QList<GeoZoneFencePrediction::Fence::Circle> circles;
    };

    /// What the vehicle's fence is and does
    struct Config {
        bool enabled = false;       ///< The parameters of the vehicle enable the fence
        GeoZoneFencePrediction::Fence fence;
        Firmware firmware = Firmware::Other;
        VehicleClass vehicleClass = VehicleClass::Copter;
        int action = -1;            ///< GF_ACTION or FENCE_ACTION, -1 when unknown
        bool modeChangeBlocked = false; ///< ArduPilot FENCE_OPTIONS bit 0: no mode change until the breach clears
        /// A VTOL hovering. ArduPilot's has the fence actions of planes, and the pilot takes control in its hover modes.
        bool vtolHover = false;
        QString takeControlMode;    ///< See takeControlMode
    };

    /// @param vehicle Whose fence and positions are followed, null for a config set with setConfig only
    /// @param clock Time of the vehicle's messages
    explicit GeoZoneFenceWarning(Vehicle *vehicle, const Clock &clock = Clock(), QObject *parent = nullptr);

    /// @return The fence of the vehicle: its polygons and circles, the circle around home and the altitude limits
    ///         its parameters enable (ArduPilot FENCE_TYPE, FENCE_RADIUS, FENCE_ALT_MAX, FENCE_ALT_MIN, FENCE_MARGIN,
    ///         FENCE_OPTIONS; PX4 GF_MAX_HOR_DIST, GF_MAX_VER_DIST) and whether its parameters enable it (ArduPilot
    ///         FENCE_ENABLE, or FENCE_AUTOENABLE while armed; PX4 unless GF_ACTION is None)
    static Config vehicleConfig(Vehicle *vehicle);
    /// @param stored Instead of the polygons and circles of the vehicle's GeoFenceManager
    static Config vehicleConfig(Vehicle *vehicle, const StoredFence &stored);
    /// @return The polygons and circles of the vehicle's GeoFenceManager, as read from or sent to the vehicle last
    static StoredFence storedFence(Vehicle *vehicle);
    /// Uses the config instead of the vehicle's, like tests do
    void setConfig(const Config &config);
    const Config &config() const { return _config; }

    /// Seconds before the predicted breach the warning comes: the pilot reaction time of the vehicle's alert profile
    /// (EN 4709-003 Annex A) and its fence warning margin, see GeoZoneAlertThresholds::Profile
    double warningTime() const { return _warningTime; }
    /// Times which aren't positive, like those of a profile with both at 0, would end the warning: they are ignored
    void setWarningTime(double seconds);

    /// Evaluates the position of GLOBAL_POSITION_INT messages, at most every minEvaluateIntervalMSecs, with the
    /// velocity the vehicle reports or, when it reports none, estimated from its positions. Follows whether an
    /// ArduPilot vehicle reports its fence enabled (SYS_STATUS).
    void handleMessage(const mavlink_message_t &message, qint64 msecs);
    /// Evaluates a position with its velocity, at the time of the clock
    /// @param altitude Meters above home, NaN when unknown
    void evaluate(const QGeoCoordinate &coordinate, double altitude, const Velocity &velocity);

    bool active() const { return _breach.has_value(); }
    const std::optional<GeoZoneFencePrediction::Breach> &breach() const { return _breach; }
    /// The position evaluated last is outside the enabled fence, or was within outsideEndDelayMSecs
    bool outside() const { return _outside.has_value(); }
    /// The boundary the vehicle is beyond, nothing while it is inside its fence
    const std::optional<GeoZoneFencePrediction::Boundary> &outsideBoundary() const { return _outside; }
    QString text() const;
    int timeToFence() const;
    double distanceToFence() const { return _breach ? _breach->distance : qQNaN(); }
    QGeoCoordinate intersection() const { return _breach ? _breach->coordinate : QGeoCoordinate(); }
    QString intersectionText() const;
    QVariantList path() const;
    /// The fence has boundaries and is enabled by the parameters of the vehicle or reported enabled by it, like one
    /// enabled with an RC switch, unless the vehicle accepted disableFence()
    bool enabled() const { return (_config.enabled || _reportedEnabled) && !_fenceDisabled && !_config.fence.isEmpty(); }
    /// The last transfer of the fence failed, so the vehicle may store another fence than its GeoFenceManager has: the
    /// one it had before a rejected upload, or one which couldn't be read. Until a transfer succeeds.
    bool fenceUnknown() const { return _fenceUnknown; }
    QString actionText() const { return actionText(_config.firmware, actionClass(_config), _config.action); }
    QString overrideText() const { return overrideText(_config); }
    QString takeControlMode() const { return _config.takeControlMode; }
    /// There is a mode to take control in, and the vehicle accepts it: during a breach ArduPilot refuses mode changes
    /// with FENCE_OPTIONS bit 0, and PX4's flight termination can't be overridden
    bool canTakeControl() const;
    /// The vehicle breaches its fence: it reports so, see setBreachReported, or its position is outside()
    bool breached() const { return _breachReported || outside(); }
    /// Set by GeoZoneVehicleMonitor for a vehicle which reports the breaches of its fence (FENCE_STATUS)
    void setBreachReported(bool breached);
    bool canDisableFence() const { return _config.firmware == Firmware::ArduPilot; }
    QString commandText() const { return _commandText; }
    bool fenceDisabled() const { return _fenceDisabled; }
    /// Where the vehicle was when the warning was evaluated last
    QGeoCoordinate position() const { return _position; }

    /// Overrides the fence action by switching to takeControlMode
    Q_INVOKABLE void takeControl();
    /// Disables the vehicle's fence until enabled again or rebooted (MAV_CMD_DO_FENCE_ENABLE), on ArduPilot
    Q_INVOKABLE void disableFence();
    Q_INVOKABLE void enableFence();

    /// @return The manoeuvre of the action, like "RTL or land", "Report only (no manoeuvre)"
    static QString actionText(Firmware firmware, VehicleClass vehicleClass, int action);
    /// @return The vehicle class whose fence actions the autopilot has
    static VehicleClass actionClass(const Config &config);
    /// @return true if the action makes the vehicle act, rather than only report the breach
    static bool isManoeuvre(Firmware firmware, int action);
    /// @return How the pilot overrides the fence action
    static QString overrideText(const Config &config);
    /// @return The first of the flight modes the pilot flies the vehicle class in which the vehicle has
    /// @param vtolHover See Config::vtolHover
    static QString takeControlMode(Firmware firmware, VehicleClass vehicleClass, const QStringList &flightModes, bool vtolHover = false);
    /// @return Like "Geofence: RTL or land in 6 s, 42 m ahead", or "Geofence in 6 s, 42 m ahead: report only, no manoeuvre"
    ///         for actions which aren't manoeuvres
    static QString warningText(const QString &action, bool manoeuvre, double time, double distance);

signals:
    void warningChanged();
    /// A breach became predicted within warningTime()
    void warningStarted(const QString &text);
    void warningEnded();
    void outsideChanged();
    void configChanged();
    void canTakeControlChanged();
    void fenceUnknownChanged();
    void commandTextChanged();
    void fenceDisabledChanged();

private:
    void _connectVehicle();
    void _updateConfig();
    /// A transfer of the fence to or from the vehicle, or its removal, ended
    void _fenceTransferred(bool failed);
    /// Reads the fence back from the vehicle while it is unknown, once no plan transfer is in progress
    void _reloadFence();
    void _evaluate(const QGeoCoordinate &coordinate, double altitude, const Velocity &velocity, qint64 msecs);
    void _setBreach(const std::optional<GeoZoneFencePrediction::Breach> &breach);
    /// @param msecs Time of the position, negative to end it without delay
    void _setOutside(const std::optional<GeoZoneFencePrediction::Boundary> &boundary, qint64 msecs);
    void _setCommandText(const QString &text);
    void _sendFenceEnable(bool enable);
    /// The vehicle switched its flight mode, or the time for it is over
    void _modeSwitched(bool timedOut);
    /// Result of a command of the vehicle, see Vehicle::mavCommandResult
    void _commandResult(int command, int ackResult, int failureCode);
    void _fenceCommandTimedOut();
    void _setFenceDisabled(bool disabled);
    void _setReportedEnabled(bool enabled);
    /// Ends the warning and the breach of a fence which is no longer enabled
    void _enabledChanged();

    QPointer<Vehicle> _vehicle;
    Clock _clock;
    Config _config;
    double _warningTime = defaultWarningTime;
    bool _fixedConfig = false;
    QObject *_parameterConnections = nullptr;   ///< Receiver of the fence parameters' changes, replaced to disconnect them
    StoredFence _storedFence;                   ///< Of the last transfer which succeeded
    bool _fenceUnknown = false;
    bool _fenceTransferError = false;           ///< The fence transfer in progress reported an error
    int _fenceReloads = 0;                      ///< Since the last transfer which succeeded
    std::optional<GeoZoneFencePrediction::Breach> _breach;
    std::optional<GeoZoneFencePrediction::Boundary> _outside;
    qint64 _insideMSecs = -1;                   ///< Since when the vehicle is back inside while _outside lasts, -1 otherwise
    QGeoCoordinate _position;
    qint64 _evaluatedMSecs = -1;
    GeoZoneVelocityEstimator _estimator;
    QString _commandText;
    bool _fenceDisabled = false;
    bool _reportedEnabled = false;              ///< The ArduPilot vehicle reports its fence enabled (SYS_STATUS)
    bool _disableReported = false;              ///< The vehicle reported its fence disabled since it accepted to disable it
    bool _breachReported = false;
    QString _pendingMode;                       ///< Flight mode takeControl() asked for which the vehicle hasn't reported yet
    bool _pendingModeOverrides = false;         ///< The vehicle was taking its fence action when takeControl() asked for it
    QTimer _modeTimer;
    std::optional<bool> _pendingFenceEnable;    ///< Fence enable or disable command the vehicle hasn't answered yet
    std::optional<bool> _unansweredFenceEnable; ///< The last one, once its answer is overdue: it may still come
    QTimer _fenceCommandTimer;
};
