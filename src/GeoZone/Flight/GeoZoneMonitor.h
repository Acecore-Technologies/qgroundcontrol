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
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"
#include "GeoZoneAlertArbiter.h"
#include "GeoZoneAlertLevel.h"
#include "GeoZoneActivationTracker.h"
#include "GeoZoneAlertModel.h"
#include "GeoZoneAlertThresholds.h"
#include "GeoZoneAlertTracker.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneAwareness.h"
#include "GeoZonePermits.h"
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
/// announcements are spoken when spoken alerts are on. A vehicle comes near a zone within its alert thresholds, which
/// the manufacturer's profile of its vehicle class defines at its speed and position accuracy (GeoZoneAlertThresholds)
/// and the pilot can only widen in GeoZoneSettings.
/// The ADS-B and Remote ID traffic around the active vehicle is checked against the zones too, with a
/// GeoZoneAlertTracker per aircraft, so the pilot sees which aircraft are in or near zones.
/// The alerts shown to the pilot are kept in a GeoZoneAlertModel with what can be done about them. An alert of a
/// vehicle approaching or entering a restricted zone asks whether the pilot has the permission to fly there: confirming
/// it (approveAlert()) acknowledges the alert and ends the alerts and repeats about the zone until the vehicle leaves
/// the zone and its approach range, or its flight ends where the permits end: when it disarms after having been armed,
/// or disconnects. So a vehicle which landed inside an approved zone is warned again before its next flight.
/// A vehicle on the ground (see GeoZoneVehicleMonitor::onGround) inside a restricted zone is alerted as such, like by a
/// pre-arm check, and again when it tries to arm there, see AlertEvent::OnGround.
/// Whether geo-awareness is available for the active vehicle is published as awareness(), and the changes of state of
/// flying vehicles are alerted, see AlertEvent::Awareness.
/// What the pilot changes about the zones the alerts come from is logged too: a source turned off or on, and an
/// authorisation recorded, changed or removed, see AlertEvent::SourceOff and AlertEvent::Authorised.
/// Zones requiring an authorisation or a notification take a permit for the flight instead of an approval, see
/// permits(): acknowledged before the flight or by answering the question, it stops their alerts for its vehicle until
/// the flight ends.
class GeoZoneMonitor : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool         monitoring          READ monitoring         NOTIFY monitoringChanged FINAL)       ///< Vehicles are monitored against loaded zones
    Q_PROPERTY(GeoZoneVehicleMonitor *activeVehicle READ activeMonitor NOTIFY activeMonitorChanged FINAL)    ///< Geozone state of the active vehicle, null without
    Q_PROPERTY(GeoZoneGuidedCheck *guidedCheck  READ guidedCheck        CONSTANT FINAL)                       ///< Checks of guided commands
    Q_PROPERTY(GeoZoneAlertHistory *alertHistory READ alertHistory      CONSTANT FINAL)                       ///< Alerts of the session and earlier today
    Q_PROPERTY(bool             alertsMuted         READ alertsMuted        NOTIFY alertsMutedChanged FINAL)     ///< The speech of the alerts is silenced, see muteAlerts()
    Q_PROPERTY(int              defaultMuteSecs     READ defaultMuteSecs    CONSTANT FINAL)
    Q_PROPERTY(QStringList      approvedZoneKeys    READ approvedZoneKeys   NOTIFY approvalsChanged FINAL)   ///< Zones approved for the active vehicle until it leaves them or its flight ends, see approveAlert()
    Q_PROPERTY(GeoZoneAlertModel *alerts            READ alertModel         CONSTANT FINAL)                  ///< Alerts shown by the alert popup
    /// Aircraft in or near zones while traffic is checked, the most restrictive zone and those inside first, as maps
    /// with callsign (or the ICAO address), zone (GeoZoneInfo of the most restrictive zone), inside, distance (meters to
    /// the zone) and altitude (meters AMSL, NaN when unknown)
    Q_PROPERTY(QVariantList     traffic             READ traffic            NOTIFY trafficChanged FINAL)
    Q_PROPERTY(double           trafficRadius       READ trafficRadius      CONSTANT FINAL)
    Q_PROPERTY(GeoZoneAwareness *awareness          READ awareness          CONSTANT FINAL)    ///< Of the active vehicle, or of the data without
    Q_PROPERTY(GeoZonePermits   *permits            READ permits            CONSTANT FINAL)    ///< Permits to enter zones for this flight
    /// GeoZoneAlertLevel of the active vehicle where it is now, -1 without zones near it: see zoneLevel(), Warning for
    /// a breach of its fence
    Q_PROPERTY(int              activeLevel         READ activeLevel        NOTIFY activeLevelChanged FINAL)
    /// GeoZoneAlertLevel of the predicted entry of the active vehicle, -1 without
    Q_PROPERTY(int              predictionLevel     READ predictionLevel    NOTIFY activeLevelChanged FINAL)
    /// Why the alert thresholds aren't the manufacturer's as written, empty when they are, see
    /// GeoZoneAlertThresholds::profilesWarning
    Q_PROPERTY(QString          profilesWarning     READ profilesWarning    CONSTANT FINAL)

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
    static constexpr int maxActionAlerts = 200;             ///< Latest alert ids whose alerts keep their actions
    static constexpr int maxMuteSecs = 24 * 60 * 60;        ///< Longest mute, see muteAlerts()
    static constexpr int trafficSeverity = 100;             ///< Added to the severity of traffic alerts, which are less urgent than those of vehicles
    static constexpr const char *permissionQuestion = "Do you have legal permission to fly here?";    ///< As logged, see approveAlert()
    static constexpr int maxTrafficAircraft = 100;          ///< Checked at a time, the closest to the active vehicle
    static constexpr int armingAlertIntervalMSecs = 10000;  ///< Arming attempts within this are alerted once, see OnGround
    /// Meters around the active vehicle within which the traffic is checked
    static constexpr double trafficRadius() { return 10000; }

    /// An alert of a vehicle about a zone, or what the pilot was shown and answered about an alert
    struct AlertEvent {
        enum Kind {
            Approach,               ///< The vehicle came near the zone
            Entry,                  ///< The vehicle entered the zone
            PositionLost,           ///< The vehicle lost its position near or inside the zone
            Traffic,                ///< An aircraft of the traffic entered the zone
            NonConformance,         ///< The vehicle left the volumes of its operational intent, see GeoZoneConformance: the zone
                                    ///< identifier is the GeoZoneConformance::stateCode, the name tells how far
            OnGround,               ///< The vehicle is on the ground inside the zone, or tried to arm there, see Circumstance
            Awareness,              ///< The geo-awareness of the flying vehicle changed state: the zone identifier is the
                                    ///< GeoZoneAwareness::stateCode, the name has the reasons
            FenceWarning,           ///< The vehicle is about to breach its own fence, see GeoZoneFenceWarning: the zone name is the
                                    ///< boundary, the detail the fence action
            FenceBreach,            ///< The vehicle reports a breach of its own fence (FENCE_STATUS)
            Activated,              ///< The zone became active, or was loaded, while the vehicle was inside it
            ActivationSoon,         ///< The zone the vehicle is in or near becomes active soon: timeToEntry has the seconds left
            // What the pilot was shown and answered, only logged. Keep them last, see pilotEvent().
            Notified,               ///< The alert popup showed the alert, asking for the permission when detail has the question
            Approved,               ///< The pilot confirmed the permission to fly in the zone, see approveAlert()
            Dismissed,              ///< The pilot dismissed the alert, or closed the popup
            Muted,                  ///< The pilot muted the alerts while the alert was shown
            Permit,                 ///< The pilot acknowledged the permit to enter the zone, detail tells where, see GeoZonePermits
            PermitEnded,            ///< The permit ended, detail tells why, see GeoZonePermits::endReasonText
            SourceOff,              ///< A source was turned off, so its zones are no longer checked: the zone name is the source's
            SourceOn,               ///< A source was turned on
            Authorised,             ///< An authorisation or notification of the zone was recorded or changed, which stops its
                                    ///< alerts while it is valid: detail has its kind, reference and validity
            AuthorisationRemoved,   ///< An authorisation or notification of the zone was removed
        };

        /// What an alert about a zone is raised in, beyond its kind. Logged in the detail column, see circumstanceCode().
        enum class Circumstance {
            None,
            Permit,                 ///< The vehicle has a permit for the zone, see GeoZonePermits: the alert is an advisory
            Arming,                 ///< OnGround: the vehicle tried to arm, or armed
        };

        /// @return true for what the pilot was shown and answered
        bool pilotEvent() const { return kind >= Notified; }
        /// @return The circumstance as logged: "permit", "arming", or empty without
        static QString circumstanceCode(Circumstance circumstance);
        /// @return The circumstance logged as the detail of an alert of the kind, None for other texts and kinds
        static Circumstance circumstanceFromCode(Kind kind, const QString &code);

        int id = 0;                 ///< Unique for the session, that of the alert for pilot events, 0 when read from a log
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
        QString detail;             ///< Of pilot events: the question asked, or the button pressed. Of fence alerts: the action.
        Circumstance circumstance = Circumstance::None;     ///< Of alerts about zones
        GeoZoneAlertLevel::Level level = GeoZoneAlertLevel::Caution;   ///< Of the alert, also for its pilot events
        /// Alert thresholds of the vehicle at the alert, of any kind, NaN when not known, like for pilot events and
        /// while the limits are fixed, see GeoZoneAlertThresholds::Result
        double warningDistance = qQNaN();
        double warningVerticalDistance = qQNaN();
        double warningTime = qQNaN();
        double groundSpeed = qQNaN();
        double verticalSpeed = qQNaN();
    };

    /// Speaks the announcements of the alerts, like AudioOutput
    class AudioSink
    {
    public:
        virtual ~AudioSink() = default;
        virtual void say(const QString &text) = 0;
    };

    /// What the monitor uses of GeoZoneSettings. The alerts and their log are always on, after EN 4709-003.
    struct Settings {
        bool audioAlerts = true;
        GeoZoneAlertThresholds::Extras extras;              ///< The pilot's extra warning time and distance
        std::optional<GeoZoneAlertTracker::Limits> limits;  ///< Instead of the thresholds of the vehicles, like tests use
        int activationCautionSecs = 600;                    ///< Before a zone becomes active, see GeoZoneActivationTracker
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
    /// @return Alerts whose actions are kept: those among the latest maxActionAlerts alert ids
    int alertSubjectCount() const { return static_cast<int>(_alertSubjects.count()); }

    /// Shows in the alert popup, logs and announces an alert of a vehicle which isn't about a zone of the catalog,
    /// like the NonConformance alerts of GeoZoneConformance, as urgent as a zone requiring an authorisation at its
    /// level. It isn't repeated.
    /// @param identifier,name Of the zone column of the log, the name also ends the message
    /// @return The alert
    AlertEvent raiseVehicleAlert(AlertEvent::Kind kind, int vehicleId, const GeoZone::Position &position, double distance, const QString &identifier,
                                 const QString &name, GeoZoneAlertLevel::Level level = GeoZoneAlertLevel::Caution);

    /// Acknowledges an alert in the history and stops repeating it
    void acknowledgeAlert(int alertId);
    /// The pilot confirmed the permission to fly in the zone of the alert, and of the alerts it replaced (see
    /// GeoZoneAlertModel::show()): acknowledges the alerts about the zones and approves them for the vehicle, whose
    /// alerts about them are neither shown, spoken nor repeated until it leaves them and their approach range, or its
    /// flight ends. They are still logged. Zones requiring an authorisation or a notification get a permit for the
    /// flight instead.
    Q_INVOKABLE void approveAlert(int alertId);
    /// The pilot dismissed a shown alert: acknowledges it and removes it from the shown alerts
    Q_INVOKABLE void dismissAlert(int alertId);
    /// The pilot closed the alert popup: removes the shown alerts, unacknowledged
    Q_INVOKABLE void closeAlerts();
    /// Silences the speech of the alerts for the seconds, defaultMuteSecs when 0 and at most maxMuteSecs: they are
    /// still shown and repeated but not spoken, except new entries into prohibited zones. Muting never hides an alert.
    Q_INVOKABLE void muteAlerts(int seconds = 0);
    Q_INVOKABLE void unmuteAlerts();

    /// @return true when the alerts of a vehicle approaching or entering a zone of the restriction ask whether the pilot
    ///         has the permission to fly there
    static bool asksPermission(GeoZone::Restriction restriction);
    /// @return Level of an alert about a vehicle on the ground inside a zone of the restriction:
    ///         a warning for prohibited zones, a caution for the other restricted zones, and an advisory once the pilot
    ///         confirmed the permission or for U-space airspace
    static GeoZoneAlertLevel::Level groundLevel(GeoZone::Restriction restriction, bool permitted);
    /// @return Level of a vehicle in or near a zone, after EN 4709-003 Annex D: Warning inside a zone asking for the
    ///         permission (asksPermission() except conditional zones) or entering it imminently, Caution near it and in
    ///         or near U-space airspace, Advisory for conditional zones and zones approved or authorised for the vehicle
    static GeoZoneAlertLevel::Level zoneLevel(GeoZone::Restriction restriction, bool inside, bool imminent, bool approved = false);
    /// @return Level of an alert of a vehicle about a zone: see zoneLevel(), Caution for lost positions, Advisory for
    ///         traffic
    static GeoZoneAlertLevel::Level alertLevel(const AlertEvent &event, bool imminent = false);

    GeoZoneManager *manager() const { return _manager; }
    GeoZoneAwareness *awareness() const { return _awareness; }
    GeoZonePermits *permits() const { return _permits; }
    /// @return Geo-awareness of the vehicle, of the data alone without
    GeoZoneAwareness::Status awarenessStatus(const GeoZoneVehicleMonitor *monitor) const;
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
    int activeLevel() const { return _activeLevel; }
    int predictionLevel() const { return _predictionLevel; }
    static int defaultMuteSecs() { return GeoZoneAlertArbiter::defaultMuteMSecs / 1000; }
    static QString profilesWarning() { return GeoZoneAlertThresholds::profilesWarning(); }
    QStringList approvedZoneKeys() const;

signals:
    /// Emitted once when a zone is approached and once when it is entered. The message names the vehicle when
    /// several vehicles are monitored. The alerts of an evaluation are emitted most severe first.
    /// @param severity GeoZoneAlertLevel::severity of the level and zone, plus trafficSeverity for traffic
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
    /// A vehicle is about to breach its fence, see GeoZoneFenceWarning. Shown, spoken and logged.
    void fenceWarning(const QString &message);
    void alertsMutedChanged();
    void trafficChanged();
    void approvalsChanged();
    void activeLevelChanged();

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
    /// Shows, logs and submits the alerts of a vehicle to the arbiter, with those of the zones becoming active soon
    void _raise(GeoZoneVehicleMonitor *monitor, const QList<GeoZoneAlertTracker::Alert> &alerts);
    void _raiseActivations(GeoZoneVehicleMonitor *monitor);
    void _submit(GeoZoneVehicleMonitor *monitor, const AlertEvent &event, const GeoZone *zone);
    /// Sets the alert thresholds of the vehicle in its alert, which the log records with every kind of alert
    void _setThresholds(AlertEvent &event, const GeoZoneVehicleMonitor *monitor) const;
    static void _setThresholds(AlertEvent &event, const GeoZoneAlertThresholds::Result &thresholds);
    /// Suspends the breaches of a vehicle whose position is lost and alerts it once, or resumes them
    void _positionAvailabilityChanged(GeoZoneVehicleMonitor *monitor);
    /// A vehicle on the ground was refused to arm, or armed: alerts the restricted zones it is in again, once per attempt
    void _armingAttempted(GeoZoneVehicleMonitor *monitor);
    /// Sets the ground warning of the vehicle from the restricted zones it is in while on the ground
    void _updateGroundStatus(GeoZoneVehicleMonitor *monitor);
    /// Publishes the geo-awareness of the active vehicle and alerts the changes of state of the flying vehicles
    void _updateAwareness();
    void _raiseAwarenessAlert(GeoZoneVehicleMonitor *monitor, const GeoZoneAwareness::Tracker::Change &change);
    /// Logs a permit acknowledged or ended
    /// @param detail Where it was acknowledged, or why it ended, as logged
    void _logPermit(const GeoZonePermits::Permit &permit, AlertEvent::Kind kind, const QString &detail);
    /// Updates the questions, ground warnings and level after a permit was acknowledged or ended, and alerts its
    /// vehicle about the zones of a withdrawn permit again
    void _permitsChanged(const GeoZonePermits::Permit &permit, bool withdrawn);
    /// Announces the alerts raised since the last time and the repeats due, as the arbiter decides
    void _announce();
    /// Adds an alert to the shown alerts, logging that the pilot was notified
    void _show(int alertId, const QString &message, int severity, GeoZoneAlertLevel::Level level);
    /// Updates activeLevel() and predictionLevel()
    void _updateActiveLevel();
    /// Logs what the pilot was shown or answered about a geozone alert, with where its vehicle is now
    void _logPilotEvent(int alertId, AlertEvent::Kind kind, const QString &detail);
    /// @param monitor Of the vehicle of the alert, null when it is gone or the alert has none
    void _logPilotEvent(const AlertEvent &alert, const GeoZoneVehicleMonitor *monitor, AlertEvent::Kind kind, const QString &detail);
    /// Logs that the pilot acknowledged the alert in the alert history
    void _logAcknowledgement(const AlertEvent &alert, const QString &detail);
    /// Logs what the pilot changed about the data the alerts come from, with where the active vehicle is
    void _logDataEvent(AlertEvent event);
    /// Logs a source turned off or on since it was last seen
    void _noteSourceSwitch(GeoZoneSource *source);
    /// Logs the authorisations recorded, changed or removed since they were last seen
    void _authorisationsChanged();
    /// Acknowledges an alert and removes it from the shown alerts
    void _dismiss(int alertId);
    /// Sets the actions of a shown alert from the state of its vehicle
    void _setAlertActions(GeoZoneAlertModel::Alert &alert) const;
    void _updateAlertActions();
    void _settingsChanged();
    /// Shows, updates or ends the fence warning of the vehicle, see GeoZoneFenceWarning
    void _updateFenceWarning(GeoZoneVehicleMonitor *monitor);
    /// Logs a fence warning or breach of the vehicle
    void _logFenceEvent(GeoZoneVehicleMonitor *monitor, AlertEvent::Kind kind, int alertId, const QString &message);

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
        QString permitKey;          ///< GeoZonePermits::key of a zone a permit applies to, empty for others
    };
    /// Keeps the subject of an alert for its actions, forgetting those more than maxActionAlerts ids before it
    void _keepSubject(const AlertSubject &subject);
    QHash<int, AlertSubject> _alertSubjects;    ///< Of the alerts among the latest maxActionAlerts ids, by id
    QHash<int, qint64> _armingAlerts;           ///< When the arming attempts were alerted by vehicle key
    GeoZoneAwareness *_awareness = nullptr;
    QHash<int, GeoZoneAwareness::Tracker> _awarenessTrackers;  ///< By vehicle key
    QTimer _awarenessTimer;                     ///< Updates the geo-awareness while the vehicles aren't evaluated
    GeoZonePermits *_permits = nullptr;
    QSet<int> _flying;                          ///< Keys of the vehicles armed since they were on the ground
    QHash<int, int> _fenceWarningAlerts;        ///< Alert ids of the active fence warnings by vehicle key
    QHash<QString, bool> _sourcesEnabled;       ///< Whether the sources were turned on when last seen, by key
    QList<GeoZoneAuthorisation> _authorisations;    ///< As last seen
    QTimer _evaluateTimer;
    QElapsedTimer _elapsed;
    Clock _clock;                               ///< _elapsed when empty
    bool _adsbTraffic = false;                  ///< Checks the traffic of ADSBVehicleManager
    GeoZoneAlertArbiter _arbiter;
    std::shared_ptr<AudioSink> _audioSink;
    int _activeLevel = -1;
    int _predictionLevel = -1;
    QTimer _muteTimer;                          ///< Notifies the end of the mute
    bool _alertsMuted = false;                  ///< As last notified
    std::optional<Settings> _fixedSettings;     ///< Set by setSettings()
    mutable std::optional<Settings> _settings;  ///< Of GeoZoneSettings, until they change
    mutable std::optional<GeoZonePolicy> _policy;   ///< Until the zones change
    QPointer<GeoZoneVehicleMonitor> _activeMonitor;
    QHash<quint32, GeoZoneAlertTracker> _trafficTrackers;  ///< By ICAO address
    QHash<int, GeoZoneActivationTracker> _activationTrackers;   ///< By GeoZoneVehicleMonitor::key
    QList<ZoneTraffic> _traffic;
    GeoZoneGuidedCheck *_guidedCheck = nullptr;
    GeoZoneAlertHistory *_alertHistory = nullptr;
    GeoZoneAlertModel *_alertModel = nullptr;
};
