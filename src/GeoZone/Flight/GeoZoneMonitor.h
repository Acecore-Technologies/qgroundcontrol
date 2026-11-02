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
#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"
#include "GeoZoneAlertArbiter.h"
#include "GeoZoneAlertModel.h"
#include "GeoZoneAlertTracker.h"
#include "GeoZonePolicy.h"
#include "GeoZoneVehicleMonitor.h"

#include <functional>
#include <memory>
#include <optional>

Q_DECLARE_LOGGING_CATEGORY(GeoZoneMonitorLog)

Q_MOC_INCLUDE("GeoZoneGuidedCheck.h")
Q_MOC_INCLUDE("GeoZoneAlertHistory.h")

class GeoZoneAlertHistory;
class GeoZoneGuidedCheck;
class GeoZoneManager;
class GeoZoneSource;
class Vehicle;

/// Alerts the user once when a vehicle approaches or enters an active restricted geozone or U-space airspace,
/// considering the zone altitude limits. Each connected vehicle has a GeoZoneVehicleMonitor, keyed by its Vehicle
/// object so that a vehicle reconnecting with the same system id starts clean. The monitor publishes the monitor of
/// the active vehicle, with its nearby zones, vertical bands, predicted entry and fence status, shows and logs the
/// alerts of all vehicles, and lets a GeoZoneAlertArbiter decide which are announced and repeated. The
/// announcements are spoken when spoken alerts are on. Alerts and the approach distances are set in GeoZoneSettings.
/// The ADS-B and Remote ID traffic around the active vehicle is checked against the zones too, with a
/// GeoZoneAlertTracker per aircraft, so the pilot sees which aircraft are in or near zones.
/// The alerts shown to the pilot are kept in a GeoZoneAlertModel with what can be done about them. An alert of a
/// vehicle approaching or entering a restricted zone asks whether the pilot has the permission to fly there: confirming
/// it (approveAlert()) acknowledges the alert and ends the alerts and repeats about the zone until the vehicle leaves
/// the zone and its approach range.
class GeoZoneMonitor : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool         monitoring          READ monitoring         NOTIFY monitoringChanged FINAL)       ///< Alerts are enabled and vehicles are monitored against loaded zones
    Q_PROPERTY(GeoZoneVehicleMonitor *activeVehicle READ activeMonitor NOTIFY activeMonitorChanged FINAL)    ///< Geozone state of the active vehicle, null without
    Q_PROPERTY(GeoZoneGuidedCheck *guidedCheck  READ guidedCheck        CONSTANT FINAL)                       ///< Checks of guided commands
    Q_PROPERTY(GeoZoneAlertHistory *alertHistory READ alertHistory      CONSTANT FINAL)                       ///< Alerts of the session and earlier today
    Q_PROPERTY(bool             alertsMuted         READ alertsMuted        NOTIFY alertsMutedChanged FINAL)     ///< Alerts are neither spoken nor repeated, see muteAlerts()
    Q_PROPERTY(int              defaultMuteSecs     READ defaultMuteSecs    CONSTANT FINAL)
    Q_PROPERTY(QStringList      approvedZoneKeys    READ approvedZoneKeys   NOTIFY approvalsChanged FINAL)   ///< Zones approved for the active vehicle until it leaves them, see approveAlert()
    Q_PROPERTY(GeoZoneAlertModel *alerts            READ alertModel         CONSTANT FINAL)                  ///< Alerts shown by the alert popup
    /// Aircraft in or near zones while traffic is checked, the most restrictive zone and those inside first, as maps
    /// with callsign (or the ICAO address), zone (GeoZoneInfo of the most restrictive zone), inside, distance (meters to
    /// the zone) and altitude (meters AMSL, NaN when unknown)
    Q_PROPERTY(QVariantList     traffic             READ traffic            NOTIFY trafficChanged FINAL)
    Q_PROPERTY(double           trafficRadius       READ trafficRadius      CONSTANT FINAL)

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
    static constexpr double climbClearance = 30;            ///< Meters above the ceiling of a zone a climb out of it goes to, see GeoZoneAlertModel
    static constexpr double approachVerticalDistance = 30;  ///< Meters below the floor or above the ceiling of a zone within which it is near
    static constexpr int maxActionAlerts = 200;             ///< Latest alerts whose actions are kept
    static constexpr int maxMuteSecs = 24 * 60 * 60;        ///< Longest mute, see muteAlerts()
    static constexpr int trafficSeverity = 100;             ///< Added to the severity of traffic alerts, which are less urgent than those of vehicles
    static constexpr int maxTrafficAircraft = 100;          ///< Checked at a time, the closest to the active vehicle
    /// Meters around the active vehicle within which the traffic is checked
    static constexpr double trafficRadius() { return 10000; }

    /// An alert of a vehicle about a zone
    struct AlertEvent {
        enum Kind {
            Approach,               ///< The vehicle came near the zone
            Entry,                  ///< The vehicle entered the zone
            PositionLost,           ///< The vehicle lost its position near or inside the zone
            Traffic,                ///< An aircraft of the traffic entered the zone
        };

        int id = 0;                 ///< Unique for the session, 0 for alerts read from a log
        QDateTime time;             ///< UTC
        int vehicleId = noVehicle;
        GeoZone::Position position; ///< The last known position for PositionLost
        Kind kind = Approach;
        bool inside = false;        ///< The vehicle is inside the zone: entered it, or lost its position inside it
        double distance = 0;        ///< Meters to the zone, 0 inside
        QString zoneIdentifier;
        QString zoneName;
        GeoZone::Restriction restriction = GeoZone::Unknown;
        QString sourceText;
        QString message;            ///< As shown to the user
        QString zoneKey;            ///< See GeoZone::key, empty for alerts read from a log
        double timeToEntry = qQNaN();   ///< Seconds until the predicted entry of an approach alert, NaN when not predicted
        QString traffic;            ///< Callsign or ICAO address of the aircraft of a Traffic alert
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
        bool trafficEnabled = false;

        /// @return The settings of GeoZoneSettings::instance()
        static Settings fromApplication();
    };

    /// An aircraft of the ADS-B or Remote ID traffic
    struct Aircraft {
        quint32 address = 0;                ///< ICAO address, identifies the aircraft
        QString callsign;
        QGeoCoordinate coordinate;          ///< With the altitude AMSL when known
        double heading = qQNaN();           ///< Degrees
        double speed = qQNaN();             ///< Meters per second over ground
        double verticalSpeed = 0;           ///< Meters per second, up positive

        /// @return Callsign, or the ICAO address without
        QString label() const;
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
    /// Vehicle whose monitor is published as activeMonitor()
    void setActiveVehicle(Vehicle *vehicle);

    /// Evaluates the monitored vehicles at their current positions and announces the alerts and repeats due, which the
    /// monitor does every evaluateIntervalMSecs. A vehicle without a current position (see
    /// GeoZoneVehicleMonitor::update) is alerted once that its position is lost and its breaches aren't repeated
    /// until the position is back. The application monitor then checks the traffic, see evaluateTraffic().
    void evaluateVehicles();

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
    /// The position of the vehicle id is lost until the next evaluate(), like that of a vehicle which stops sending it:
    /// it is alerted once that its position is lost near or inside the most restrictive zone around its last position,
    /// and its breaches aren't repeated meanwhile
    void positionLost(int vehicleId);
    /// Checks the aircraft within trafficRadius() of the active vehicle against the zones, forgetting those beyond it
    /// or gone, like evaluateVehicles() does with the traffic of ADSBVehicleManager while traffic is checked, and shows
    /// and logs the alerts of those entering a zone, which aren't spoken. The terrain below the vehicle is used for
    /// aircraft without terrain data below them.
    void evaluateTraffic(const QList<Aircraft> &aircraft);
    /// Publishes the positions passed to evaluate() for the vehicle id, noVehicle by default
    void setActiveVehicleId(int vehicleId);
    int activeVehicleId() const { return _activeVehicleId; }

    /// @return Position of the vehicle, see GeoZoneVehicleMonitor::vehiclePosition
    static GeoZone::Position vehiclePosition(Vehicle *vehicle, const QGeoCoordinate &coordinate) { return GeoZoneVehicleMonitor::vehiclePosition(vehicle, coordinate); }
    /// @return Description of a FENCE_STATUS breach like "Boundary fence breached (3 times)"
    static QString fenceBreachText(int breachType, int breachCount) { return GeoZoneVehicleMonitor::fenceBreachText(breachType, breachCount); }
    /// @return Message of the alert about the zone, without the vehicle, like "Entered geozone P (Prohibited)"
    static QString alertMessage(const AlertEvent &event, const QString &zoneName, const QString &restrictionText);

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
    void acknowledgeAlert(int alertId);
    /// The pilot confirmed the permission to fly in the zone of the alert: acknowledges the alerts about the zone and
    /// approves the zone for the vehicle, whose alerts about it are neither shown, spoken nor repeated until it leaves
    /// the zone and its approach range. They are still logged.
    Q_INVOKABLE void approveAlert(int alertId);
    /// The pilot dismissed a shown alert: acknowledges it and removes it from the shown alerts
    Q_INVOKABLE void dismissAlert(int alertId);
    /// The pilot closed the alert popup: removes the shown alerts, unacknowledged
    Q_INVOKABLE void closeAlerts();
    /// Mutes the alerts for the seconds, defaultMuteSecs when 0 and at most maxMuteSecs: they are shown but neither
    /// spoken nor repeated, except new entries into prohibited zones
    Q_INVOKABLE void muteAlerts(int seconds = 0);
    Q_INVOKABLE void unmuteAlerts();

    /// @return true when the alerts of a vehicle approaching or entering a zone of the restriction ask whether the pilot
    ///         has the permission to fly there
    static bool asksPermission(GeoZone::Restriction restriction);

    GeoZoneManager *manager() const { return _manager; }
    /// Monitor of the active vehicle, or of the positions of activeVehicleId() without, null when not monitored
    GeoZoneVehicleMonitor *activeMonitor() const { return _activeMonitor; }
    QVariantList traffic() const;
    GeoZoneGuidedCheck *guidedCheck() const { return _guidedCheck; }
    /// Alerts of the session, which only the application monitor logs
    GeoZoneAlertHistory *alertHistory() const { return _alertHistory; }
    /// Alerts shown to the pilot, with their actions: whether the alert asks for permission (see approveAlert()), is
    /// about the active vehicle, whether the vehicle is still inside the zone and how far it climbs to be climbClearance
    /// above the zone's ceiling where it is (NaN when the zone has no known ceiling there or the vehicle's position or
    /// altitude are unknown)
    GeoZoneAlertModel *alertModel() const { return _alertModel; }

    bool monitoring() const;
    bool alertsMuted() const { return _arbiter.muted(); }
    static int defaultMuteSecs() { return GeoZoneAlertArbiter::defaultMuteMSecs / 1000; }
    QStringList approvedZoneKeys() const;

signals:
    /// Emitted once when a zone is approached and once when it is entered. The message names the vehicle when
    /// several vehicles are monitored. The alerts of an evaluation are emitted most severe first.
    /// @param severity GeoZone::restrictionSeverity of the zone, plus trafficSeverity for traffic
    /// @param alertId See AlertEvent::id and acknowledgeAlert()
    void alert(const QString &message, int severity, int alertId);
    /// An unacknowledged entry into a prohibited zone was announced again, the vehicle is still inside. Also when the
    /// alerts aren't spoken.
    void alertRepeated(const QString &message, int severity, int alertId);
    void alertEvent(const GeoZoneMonitor::AlertEvent &event);
    void monitoringChanged();
    /// Another vehicle became active, or none
    void activeMonitorChanged();
    /// A vehicle started breaching its fence. The vehicle announces it, so it isn't spoken.
    void fenceBreach(const QString &message);
    void alertsMutedChanged();
    void trafficChanged();
    void approvalsChanged();

private slots:
    void _updateEvaluateTimer();
    void _forgetZones(GeoZoneSource *source);
    void _zonesChanged(GeoZoneSource *source);
    void _applicationSettingsChanged();
    void _updateMuted();

private:
    GeoZoneVehicleMonitor *_createMonitor(Vehicle *vehicle, int vehicleId);
    void _removeMonitor(GeoZoneVehicleMonitor *monitor);
    /// Finds the monitor of the active vehicle again, publishing it when it changed
    void _resolveActive();

    /// @return What the vehicles are evaluated against, kept until the settings or zones change
    GeoZoneVehicleMonitor::Context _context() const;
    void _evaluate(GeoZoneVehicleMonitor *monitor, const GeoZone::Position &position, const Velocity &velocity);
    /// Shows, logs and submits the alerts of a vehicle to the arbiter
    void _raise(GeoZoneVehicleMonitor *monitor, const QList<GeoZoneAlertTracker::Alert> &alerts);
    void _submit(GeoZoneVehicleMonitor *monitor, const AlertEvent &event, const GeoZone *zone);
    /// Suspends the breaches of a vehicle whose position is lost and alerts it once, or resumes them
    void _positionAvailabilityChanged(GeoZoneVehicleMonitor *monitor);
    /// Announces the alerts raised since the last time and the repeats due, as the arbiter decides
    void _announce();
    /// Adds an alert to the shown alerts
    void _show(int alertId, const QString &message, int severity);
    /// Sets the actions of a shown alert from the state of its vehicle
    void _setAlertActions(GeoZoneAlertModel::Alert &alert) const;
    void _updateAlertActions();
    void _settingsChanged(const Settings &previous);
    void _resetMonitors();

    AlertEvent _alertEvent(const GeoZone *zone, AlertEvent::Kind kind, bool inside, double distance, double timeToEntry, const GeoZone::Position &position, int vehicleId, const QString &traffic = QString());
    /// Aircraft in or near a zone, with the most restrictive one
    struct ZoneTraffic {
        Aircraft aircraft;
        GeoZoneAlertTracker::Nearby nearby;
    };
    void _evaluateADSBTraffic();
    void _setTraffic(const QList<ZoneTraffic> &traffic);
    /// Forgets the aircraft and their states
    void _clearTraffic();
    /// @return Meters to climb above the zone by climbClearance, NaN when it has no known ceiling at the vehicle
    static double _climbHeight(const GeoZoneVehicleMonitor *monitor, const GeoZone *zone);
    GeoZoneVehicleMonitor *_monitorByKey(int key) const;
    /// Notifies approvalsChanged when the count changed since it was taken
    void _approvalsChanged(int previousCount);
    /// @return The message naming the vehicle while several are monitored
    QString _labelled(int vehicleId, const QString &message) const;
    qint64 _now() const { return _clock ? _clock() : _elapsed.elapsed(); }

    GeoZoneManager *_manager = nullptr;
    QList<GeoZoneVehicleMonitor*> _monitors;    ///< Children: of vehicles, and of positions by vehicle id
    QPointer<Vehicle> _activeVehicle;
    int _activeVehicleId = noVehicle;
    int _nextVehicleKey = 1;
    int _nextAlertId = 1;
    struct AlertSubject {
        int vehicleKey = 0;         ///< 0 for traffic
        AlertEvent event;
        bool asksPermission = false;
    };
    QHash<int, AlertSubject> _alertSubjects;    ///< Of the latest maxActionAlerts alerts by id
    QTimer _evaluateTimer;
    QElapsedTimer _elapsed;
    Clock _clock;                               ///< _elapsed when empty
    bool _adsbTraffic = false;                  ///< Checks the traffic of ADSBVehicleManager
    GeoZoneAlertArbiter _arbiter;
    std::shared_ptr<AudioSink> _audioSink;
    QTimer _muteTimer;                          ///< Notifies the end of the mute
    bool _alertsMuted = false;                  ///< As last notified
    std::optional<Settings> _fixedSettings;     ///< Set by setSettings()
    mutable std::optional<Settings> _settings;  ///< Of GeoZoneSettings, until they change
    mutable std::optional<GeoZonePolicy> _policy;   ///< Until the zones change
    QPointer<GeoZoneVehicleMonitor> _activeMonitor;
    QHash<quint32, GeoZoneAlertTracker> _trafficTrackers;  ///< By ICAO address
    QList<ZoneTraffic> _traffic;
    GeoZoneGuidedCheck *_guidedCheck = nullptr;
    GeoZoneAlertHistory *_alertHistory = nullptr;
    GeoZoneAlertModel *_alertModel = nullptr;
};
