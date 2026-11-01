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
#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"
#include "GeoZoneAlertArbiter.h"
#include "GeoZoneAlertTracker.h"
#include "GeoZonePolicy.h"
#include "GeoZoneVehicleMonitor.h"

#include <functional>
#include <memory>
#include <optional>

Q_DECLARE_LOGGING_CATEGORY(GeoZoneMonitorLog)

Q_MOC_INCLUDE("GeoZoneGuidedCheck.h")
Q_MOC_INCLUDE("GeoZoneAlertHistory.h")
Q_MOC_INCLUDE("GeoZoneActiveVehicleView.h")

class GeoZoneActiveVehicleView;
class GeoZoneAlertHistory;
class GeoZoneAlertLog;
class GeoZoneGuidedCheck;
class GeoZoneManager;
class GeoZoneSource;
class Vehicle;

/// Alerts the user once when a vehicle approaches or enters an active restricted geozone or U-space airspace,
/// considering the zone altitude limits. Each connected vehicle has a GeoZoneVehicleMonitor, keyed by its Vehicle
/// object so that a vehicle reconnecting with the same system id starts clean. The monitor publishes the nearby zones,
/// vertical bands, predicted entry and fence status of the active vehicle with a GeoZoneActiveVehicleView, shows and
/// logs the alerts of all vehicles, and lets a GeoZoneAlertArbiter decide which are announced and repeated. The
/// announcements are spoken when spoken alerts are on. Alerts and the approach distances are set in GeoZoneSettings.
class GeoZoneMonitor : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool         monitoring          READ monitoring         NOTIFY monitoringChanged FINAL)       ///< Alerts are enabled and vehicles are monitored against loaded zones
    Q_PROPERTY(GeoZoneActiveVehicleView *activeVehicle READ activeView CONSTANT FINAL)                    ///< Geozone state of the active vehicle
    Q_PROPERTY(GeoZoneGuidedCheck *guidedCheck  READ guidedCheck        CONSTANT FINAL)                       ///< Checks of guided commands
    Q_PROPERTY(GeoZoneAlertHistory *alertHistory READ alertHistory      CONSTANT FINAL)                       ///< Alerts of the session and earlier today
    Q_PROPERTY(bool             alertsMuted         READ alertsMuted        NOTIFY alertsMutedChanged FINAL)     ///< Alerts are neither spoken nor repeated, see muteAlerts()
    Q_PROPERTY(int              defaultMuteSecs     READ defaultMuteSecs    CONSTANT FINAL)

public:
    /// Monitors the vehicles of MultiVehicleManager against the zones of GeoZoneManager::instance(), logs the alerts
    /// and speaks them with AudioOutput
    explicit GeoZoneMonitor(QObject *parent = nullptr);

    /// Monitors the vehicles added and the positions passed to evaluate() against the zones of the manager. It speaks
    /// nothing until given an audio sink.
    explicit GeoZoneMonitor(GeoZoneManager *manager, QObject *parent = nullptr);

    static GeoZoneMonitor *instance();

    static constexpr int evaluateIntervalMSecs = 1000;
    static constexpr int noVehicle = -1;                    ///< Vehicle id of positions which don't belong to a vehicle

    /// An approach or entry alert
    struct AlertEvent {
        int id = 0;                 ///< Unique for the session, 0 for alerts read from a log
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
        QString zoneKey;            ///< See GeoZone::key, empty for alerts read from a log
        double timeToEntry = qQNaN();   ///< Seconds until the predicted entry of an approach alert, NaN when not predicted
    };

    /// Speaks the announcements of the alerts, like AudioOutput
    class AudioSink
    {
    public:
        virtual ~AudioSink() = default;
        virtual void say(const QString &text) = 0;
    };

    /// What the monitor uses of GeoZoneSettings
    struct Settings {
        bool alertsEnabled = true;
        bool audioAlerts = true;
        GeoZoneAlertTracker::Limits limits;

        /// @return The settings of GeoZoneSettings::instance()
        static Settings fromApplication();
    };

    using Velocity = GeoZoneAlertTracker::Velocity;
    /// @return Monotonic milliseconds
    using Clock = std::function<qint64()>;

    /// Monitors a vehicle, as done for those of MultiVehicleManager, until it is removed or destroyed
    /// @return Its monitor, also when it was monitored already
    GeoZoneVehicleMonitor *addVehicle(Vehicle *vehicle);
    /// Stops monitoring the vehicle, forgetting its state
    void removeVehicle(Vehicle *vehicle);
    /// @return Monitor of the vehicle, null when it isn't monitored
    GeoZoneVehicleMonitor *vehicleMonitor(const Vehicle *vehicle) const;
    /// Vehicle whose nearby zones, bands, prediction and fence status are published by activeView()
    void setActiveVehicle(Vehicle *vehicle);

    /// Evaluates a position of a monitored vehicle, like the monitor does every evaluateIntervalMSecs with the
    /// position the vehicle reports, and announces its alerts
    void evaluate(Vehicle *vehicle, const GeoZone::Position &position, const Velocity &velocity = Velocity());

    /// Evaluates a position which doesn't come from a Vehicle object, like those of tests and tools, monitored by
    /// vehicle id, and announces its alerts. Unknown altitudes are inside all vertical limits. With a valid velocity,
    /// the vehicle is also alerted when it would enter a zone within the approach time.
    void evaluate(const GeoZone::Position &position, int vehicleId, const Velocity &velocity);
    void evaluate(const GeoZone::Position &position, int vehicleId = noVehicle) { evaluate(position, vehicleId, Velocity()); }
    /// @return Monitor of the positions passed to evaluate() for the vehicle id, null before the first
    GeoZoneVehicleMonitor *positionMonitor(int vehicleId) const;
    /// Forgets the positions of the vehicle id, so that its alerts repeat
    void removePositionMonitor(int vehicleId);
    /// Publishes the positions passed to evaluate() for the vehicle id, noVehicle by default
    void setActiveVehicleId(int vehicleId);
    int activeVehicleId() const { return _activeVehicleId; }

    /// @return Position of the vehicle, see GeoZoneVehicleMonitor::vehiclePosition
    static GeoZone::Position vehiclePosition(Vehicle *vehicle, const QGeoCoordinate &coordinate) { return GeoZoneVehicleMonitor::vehiclePosition(vehicle, coordinate); }
    /// @return Description of a FENCE_STATUS breach like "Boundary fence breached (3 times)"
    static QString fenceBreachText(int breachType, int breachCount) { return GeoZoneVehicleMonitor::fenceBreachText(breachType, breachCount); }

    /// Replaces the clock of the vehicles' messages and of the spoken alerts, like tests do
    void setClock(const Clock &clock) { _clock = clock; }
    /// Replaces how the announcements are spoken while spoken alerts are on, nothing is spoken when null
    void setAudioSink(const std::shared_ptr<AudioSink> &sink) { _audioSink = sink; }
    /// Uses the settings instead of following GeoZoneSettings, like tests do
    void setSettings(const Settings &settings);
    /// The settings in use, read from GeoZoneSettings again after they change
    const Settings &settings() const;
    const GeoZoneAlertArbiter &arbiter() const { return _arbiter; }

    /// Acknowledges an alert in the history and stops repeating it
    Q_INVOKABLE void acknowledgeAlert(int alertId);
    /// Mutes the alerts for the seconds, defaultMuteSecs when 0: they are shown but neither spoken nor repeated, except
    /// new entries into prohibited zones
    Q_INVOKABLE void muteAlerts(int seconds = 0);
    Q_INVOKABLE void unmuteAlerts();

    GeoZoneManager *manager() const { return _manager; }
    GeoZoneActiveVehicleView *activeView() const { return _activeView; }
    GeoZoneGuidedCheck *guidedCheck() const { return _guidedCheck; }
    /// Log of the alerts of vehicles, null for monitors constructed with a manager
    GeoZoneAlertLog *alertLog() const { return _alertLog; }
    GeoZoneAlertHistory *alertHistory() const { return _alertHistory; }

    bool monitoring() const;
    bool alertsMuted() const { return _arbiter.muted(); }
    static int defaultMuteSecs() { return GeoZoneAlertArbiter::defaultMuteMSecs / 1000; }

signals:
    /// Emitted once when a zone is approached and once when it is entered. The message names the vehicle when
    /// several vehicles are monitored. The alerts of an evaluation are emitted most severe first.
    /// @param severity GeoZone::restrictionSeverity of the zone
    /// @param alertId See AlertEvent::id and acknowledgeAlert()
    void alert(const QString &message, int severity, int alertId);
    /// An unacknowledged entry into a prohibited zone was announced again, the vehicle is still inside. Also when the
    /// alerts aren't spoken.
    void alertRepeated(const QString &message, int severity, int alertId);
    void alertEvent(const GeoZoneMonitor::AlertEvent &event);
    void monitoringChanged();
    /// A vehicle started breaching its fence. The vehicle announces it, so it isn't spoken.
    void fenceBreach(const QString &message);
    void alertsMutedChanged();

private slots:
    void _evaluateVehicles();
    void _updateEvaluateTimer();
    void _forgetZones(GeoZoneSource *source);
    void _zonesChanged(GeoZoneSource *source);
    void _applicationSettingsChanged();
    void _updateMuted();

private:
    GeoZoneVehicleMonitor *_createMonitor(Vehicle *vehicle, int vehicleId);
    void _removeMonitor(GeoZoneVehicleMonitor *monitor);
    /// Finds the monitor of the active vehicle again, publishing its state when it changed
    void _resolveActive();

    /// @return What the vehicles are evaluated against, kept until the settings or zones change
    GeoZoneVehicleMonitor::Context _context() const;
    void _evaluate(GeoZoneVehicleMonitor *monitor, const GeoZone::Position &position, const Velocity &velocity);
    /// Shows, logs and submits the alerts of a vehicle to the arbiter
    void _raise(GeoZoneVehicleMonitor *monitor, const QList<GeoZoneAlertTracker::Alert> &alerts);
    /// Announces the alerts raised since the last time and the repeats due, as the arbiter decides
    void _announce();
    void _settingsChanged(const Settings &previous);
    void _resetMonitors();

    AlertEvent _alertEvent(const GeoZoneAlertTracker::Alert &alert, const GeoZone::Position &position, int vehicleId);
    /// @return The message naming the vehicle while several are monitored
    QString _labelled(int vehicleId, const QString &message) const;
    qint64 _now() const { return _clock ? _clock() : _elapsed.elapsed(); }

    GeoZoneManager *_manager = nullptr;
    QList<GeoZoneVehicleMonitor*> _monitors;    ///< Children: of vehicles, and of positions by vehicle id
    QPointer<Vehicle> _activeVehicle;
    int _activeVehicleId = noVehicle;
    int _nextVehicleKey = 1;
    int _nextAlertId = 1;
    QTimer _evaluateTimer;
    QElapsedTimer _elapsed;
    Clock _clock;                               ///< _elapsed when empty
    GeoZoneAlertArbiter _arbiter;
    std::shared_ptr<AudioSink> _audioSink;
    QTimer _muteTimer;                          ///< Notifies the end of the mute
    bool _alertsMuted = false;                  ///< As last notified
    std::optional<Settings> _fixedSettings;     ///< Set by setSettings()
    mutable std::optional<Settings> _settings;  ///< Of GeoZoneSettings, until they change
    mutable std::optional<GeoZonePolicy> _policy;   ///< Until the zones change
    GeoZoneAlertLog *_alertLog = nullptr;
    GeoZoneActiveVehicleView *_activeView = nullptr;   ///< Of _activeVehicle, or of the positions of _activeVehicleId without
    GeoZoneGuidedCheck *_guidedCheck = nullptr;
    GeoZoneAlertHistory *_alertHistory = nullptr;
};
