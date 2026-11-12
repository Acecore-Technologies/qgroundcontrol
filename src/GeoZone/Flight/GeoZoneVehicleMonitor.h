/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"
#include "GeoZoneAlertTracker.h"
#include "GeoZoneAwareness.h"
#include "GeoZonePolicy.h"
#include "GeoZoneVehicleThresholds.h"
#include "QGCMAVLink.h"

#include <functional>
#include <optional>

Q_MOC_INCLUDE("GeoZoneNearbyModel.h")
Q_MOC_INCLUDE("GeoZoneFenceWarning.h")

class GeoZoneCatalog;
class GeoZoneFenceWarning;
class GeoZoneNearbyModel;
class Vehicle;

/// Monitors one vehicle against the zones for GeoZoneMonitor: the messages of the vehicle giving its position, velocity
/// and fence status, the approach and entry states of the zones around it (GeoZoneAlertTracker), where it would enter
/// a zone at its velocity and the zones above and below it. A vehicle which reconnects gets a new monitor, so it
/// starts clean even with the same system id. QML shows the monitor of the active vehicle, GeoZoneMonitor.activeVehicle.
/// A connected vehicle which is disarmed is on the ground, which GeoZoneMonitor warns about inside restricted zones.
class GeoZoneVehicleMonitor : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int          vehicleId           READ vehicleId          CONSTANT FINAL)
    Q_PROPERTY(GeoZoneNearbyModel *nearbyModel  READ nearbyModel        CONSTANT FINAL)                       ///< nearby() as a list model
    Q_PROPERTY(int          alertRestriction    READ alertRestriction   NOTIFY nearbyChanged FINAL)           ///< Most restrictive GeoZone::Restriction nearby, -1 without
    Q_PROPERTY(bool         inside              READ inside             NOTIFY nearbyChanged FINAL)           ///< The vehicle is inside one of the nearby zones
    /// GeoZoneInfo of the nearby zones and of the predicted entry, which the map filters don't hide
    Q_PROPERTY(QVariantList keptZones           READ keptZones          NOTIFY keptZonesChanged FINAL)
    /// Volumes of the active zones above and below the vehicle, as maps with restriction, severity (see
    /// GeoZone::restrictionSeverity), name, limitsText, floor and ceiling (meters AMSL, NaN when the zone extends to the
    /// ground or has no ceiling, or the limit is unknown)
    Q_PROPERTY(QVariantList verticalBands       READ verticalBands      NOTIFY verticalBandsChanged FINAL)
    Q_PROPERTY(double       altitudeAMSL        READ altitudeAMSL       NOTIFY verticalBandsChanged FINAL)    ///< Of the bands, NaN when unknown
    Q_PROPERTY(double       terrainAMSL         READ terrainAMSL        NOTIFY verticalBandsChanged FINAL)    ///< Below the vehicle, NaN when unknown
    /// The vehicle has no current position: its alerts are paused and the nearby zones and bands are those of its last
    /// position, so they are stale
    Q_PROPERTY(bool         positionUnavailable READ positionUnavailable NOTIFY positionUnavailableChanged FINAL)
    Q_PROPERTY(bool         altitudeDegraded    READ altitudeDegraded   NOTIFY terrainSourceChanged FINAL)
    /// Where the vehicle enters a nearby zone first at its current velocity within the approach time: the path from the
    /// vehicle to the entry, the entry position, the seconds until then and the zone (GeoZoneInfo). Empty, invalid and
    /// -1 without such an entry.
    Q_PROPERTY(QVariantList     predictedPath       READ predictedPath      NOTIFY predictionChanged FINAL)
    Q_PROPERTY(QGeoCoordinate   predictedEntry      READ predictedEntry     NOTIFY predictionChanged FINAL)
    Q_PROPERTY(int              predictedEntryTime  READ predictedEntryTime NOTIFY predictionChanged FINAL)
    Q_PROPERTY(QVariant         predictedEntryZone  READ predictedEntryZone NOTIFY predictionChanged FINAL)
    /// The vehicle breaches its own fence, with the kind of breach and their count: as it reports it (FENCE_STATUS), or,
    /// for autopilots which don't like PX4, while its position is outside the fence QGroundControl holds for it, see
    /// GeoZoneFenceWarning::outside
    Q_PROPERTY(bool             fenceBreached       READ fenceBreached      NOTIFY fenceStatusChanged FINAL)
    Q_PROPERTY(QString          fenceBreachText     READ fenceBreachText    NOTIFY fenceStatusChanged FINAL)
    Q_PROPERTY(bool             fenceStatusKnown    READ fenceStatusKnown   NOTIFY fenceStatusChanged FINAL)     ///< The vehicle sends FENCE_STATUS
    Q_PROPERTY(bool             onGround            READ onGround           NOTIFY armedChanged FINAL)           ///< See onGround()
    /// Why the vehicle on the ground mustn't take off, or needs a permission, like "On the ground inside prohibited
    /// geozone P — take-off not allowed here", empty when it isn't on the ground inside a restricted zone
    Q_PROPERTY(QString          groundWarning       READ groundWarning      NOTIFY groundStatusChanged FINAL)
    Q_PROPERTY(int              groundLevel         READ groundLevel        NOTIFY groundStatusChanged FINAL)    ///< GeoZoneAlertLevel::Level of groundWarning
    /// Warns before the vehicle breaches its own fence, null for positions passed to evaluate()
    Q_PROPERTY(GeoZoneFenceWarning *fenceWarning    READ fenceWarning       CONSTANT FINAL)
    /// Alert thresholds of the last evaluation, see GeoZoneVehicleThresholds::toVariantMap, empty while the limits are fixed
    Q_PROPERTY(QVariantMap      alertThresholds     READ alertThresholds    NOTIFY alertThresholdsChanged FINAL)

public:
    using Velocity = GeoZoneAlertTracker::Velocity;
    using Nearby = GeoZoneAlertTracker::Nearby;
    using Entry = GeoZoneAlertTracker::Entry;
    /// @return Monotonic milliseconds
    using Clock = std::function<qint64()>;

    static constexpr int positionTimeoutMSecs = 5000;           ///< Positions are stale after this at least, see positionTimeout()
    static constexpr int maxPositionTimeoutMSecs = 60000;       ///< ... and after this at most, however slow the link
    static constexpr int positionTimeoutIntervals = 3;          ///< Usual intervals without a position after which it is stale
    static constexpr int positionIntervalCount = 5;             ///< Last intervals the usual one is the median of
    /// Velocities reported by the vehicle are used up to this age, or one and a half usual intervals of its positions
    static constexpr int reportedVelocityMaxAgeMSecs = 2000;
    /// The fence status the vehicle reported is used up to this age. ArduPilot stops sending it while its fence is disabled.
    static constexpr int fenceStatusTimeoutMSecs = 10000;

    /// Where the terrain elevation below the vehicle, and so its height above ground, comes from
    enum class TerrainSource {
        Unknown,        ///< Not known, limits above ground apply at any altitude
        Vehicle,        ///< The height above terrain the vehicle reports
        Ground,         ///< A disarmed vehicle is on the ground
        Database,       ///< The terrain data of QGroundControl, less accurate than the vehicle's own
    };

    /// What the vehicle is evaluated against, which GeoZoneMonitor keeps until the settings or the zones change
    struct Context {
        const GeoZoneCatalog *catalog = nullptr;
        GeoZonePolicy policy;
        GeoZoneAlertThresholds::Extras extras;              ///< Added to the thresholds of the vehicle
        std::optional<GeoZoneAlertTracker::Limits> limits;  ///< Instead of the thresholds of the vehicle, like tests use
        /// Brings GeoZone::active of the zones in the area up to date before the vehicle is evaluated against them, so
        /// that its alerts follow their schedules at each evaluation, see GeoZoneManager::updateActive
        std::function<void(const GeoZoneBox &area)> refreshActive;
    };

    /// @param vehicle Vehicle whose messages and position are followed, null for positions passed to evaluate()
    /// @param vehicleId System id of the vehicle, or GeoZoneMonitor::noVehicle
    /// @param key Unique for the session, see GeoZoneAlertArbiter::Alert::vehicleKey
    /// @param clock Time of the vehicle's messages
    GeoZoneVehicleMonitor(Vehicle *vehicle, int vehicleId, int key, const Clock &clock, QObject *parent = nullptr);

    /// Null for positions passed to evaluate() and once the vehicle is destroyed
    Vehicle *vehicle() const;
    /// The monitor follows a Vehicle object, even one destroyed since
    bool followsVehicle() const { return _followsVehicle; }
    int vehicleId() const { return _vehicleId; }
    int key() const { return _key; }

    /// Records when the vehicle sent the position its Vehicle shows, the velocity sent with it and the status of its fence
    /// @param msecs Time of the clock
    void handleMessage(const mavlink_message_t &message, qint64 msecs);

    /// @return Velocity the vehicle reported with its position when recent, otherwise estimated from its positions,
    ///         after adding the position to the estimate when a position message arrived since the last one added.
    ///         Not valid when neither is known, like on a link whose positions are too far apart to estimate from.
    Velocity velocity(qint64 msecs, const GeoZone::Position &position);

    /// @return true if the vehicle sent no position for longer than positionTimeout(). Only the messages the Vehicle
    ///         takes its coordinate from count: GLOBAL_POSITION_INT with a position, GPS_RAW_INT with a 3D fix until the
    ///         first of those, and the high latency messages. So a coordinate which stopped moving because its message
    ///         stopped is stale also while the vehicle sends other messages.
    bool positionStale(qint64 msecs) const;
    /// @return Milliseconds without a position after which it is stale: positionTimeoutIntervals usual intervals of
    ///         the position messages, for links like satellite ones which send them less often, between
    ///         positionTimeoutMSecs and maxPositionTimeoutMSecs. The usual interval is the median of the last
    ///         positionIntervalCount, so that one message after an outage doesn't stretch the timeout.
    qint64 positionTimeout() const;

    /// Evaluates the current position of the vehicle, unless it has none: its coordinate is invalid, its link is lost
    /// or its position stale. Then its alerts pause and positionUnavailable() is true. While its velocity isn't known,
    /// its thresholds are those at the speeds it reports or its class is assumed to fly at, see
    /// GeoZoneVehicleThresholds::speedsWithoutVelocity.
    /// @return Zones newly approached or entered, see GeoZoneAlertTracker::Update
    QList<GeoZoneAlertTracker::Alert> update(const Context &context);

    /// Evaluates a position the vehicle sent, at the time of the clock, with its velocity() then, like update() does
    /// with the coordinate of its Vehicle. Without a valid velocity, the thresholds are those of
    /// GeoZoneVehicleThresholds::speedsWithoutVelocity.
    QList<GeoZoneAlertTracker::Alert> evaluateAt(qint64 msecs, const GeoZone::Position &position, const Context &context);

    /// Evaluates a position of the vehicle. Unknown altitudes are inside all vertical limits. With a valid velocity,
    /// zones the vehicle would enter within the approach time are near too. The approach distances and time are the
    /// alert thresholds of the vehicle at its velocity, see thresholds(), and those at rest without a valid velocity:
    /// update() is what knows the speeds of a vehicle without one.
    QList<GeoZoneAlertTracker::Alert> evaluate(const GeoZone::Position &position, const Velocity &velocity, const Context &context);

    /// Alert thresholds of the vehicle, of its last evaluation
    const GeoZoneVehicleThresholds &thresholds() const { return _thresholds; }
    /// Profile class of the manufacturer's thresholds, see GeoZoneVehicleThresholds::vehicleClass. That of a vehicle
    /// follows its type.
    void setVehicleClass(const QString &vehicleClass) { _thresholds.setVehicleClass(vehicleClass); }
    QVariantMap alertThresholds() const { return _fixedLimits ? QVariantMap() : _thresholds.toVariantMap(); }

    /// Zones near or around the vehicle, the most restrictive first
    const QList<Nearby> &nearby() const { return _nearby; }
    GeoZoneNearbyModel *nearbyModel() const { return _nearbyModel; }
    int alertRestriction() const { return _nearby.isEmpty() ? -1 : static_cast<int>(_nearby.first().zone->restriction()); }
    bool inside() const;
    /// Keys of the nearby zones the vehicle is inside, see GeoZone::key
    QSet<QString> insideZoneKeys() const;
    /// Keys of the nearby zones, those the vehicle is in or near
    QSet<QString> nearbyZoneKeys() const;
    QVariantList keptZones() const;

    QVariantList verticalBands() const;
    /// Of the position the vertical bands were last updated for
    double altitudeAMSL() const { return _bandsPosition.altitudeAMSL; }
    double terrainAMSL() const { return _bandsPosition.terrainAMSL; }
    /// Last evaluated position
    const GeoZone::Position &position() const { return _position; }
    /// Of the last position of the vehicle, Unknown for positions passed to evaluate()
    TerrainSource terrainSource() const { return _terrainSource; }
    /// The height above ground of the vehicle, which limits above ground are compared with, comes from the terrain
    /// data of QGroundControl because the vehicle doesn't send its own, so it is less accurate
    bool altitudeDegraded() const { return _terrainSource == TerrainSource::Database; }

    /// First predicted entry from the last evaluated position, nothing while the position is unavailable
    std::optional<Entry> entry() const { return _unavailable ? std::nullopt : _entry; }
    QVariantList predictedPath() const;
    QGeoCoordinate predictedEntry() const;
    int predictedEntryTime() const;
    QVariant predictedEntryZone() const;

    bool positionUnavailable() const { return _unavailable; }
    /// @return GNSS status of the last GPS_RAW_INT of the vehicle: not known before the first, and not known and stale
    ///         once it is older than positionTimeoutMSecs, which degrades the geo-awareness
    /// @param msecs Time of the clock
    GeoZoneAwareness::Gnss gnss(qint64 msecs) const;
    void setPositionUnavailable(bool unavailable);

    GeoZoneFenceWarning *fenceWarning() const { return _fenceWarning; }
    bool fenceStatusKnown() const { return _fenceStatusKnown; }
    bool fenceBreached() const { return _fenceBreached; }
    /// Description of the current fence breach, empty without
    QString fenceBreachText() const;

    /// Forgets the zones of a source which are about to be deleted, keeping their alert states
    void forgetZones(const QString &sourceKey);
    /// Forgets the alert states of zones by GeoZone::key, like zones whose data changed, so that they alert again
    void forgetStates(const QSet<QString> &zoneKeys, bool asEntry = false) { _alerts.forget(zoneKeys, asEntry); }

    /// @return Position of the vehicle with its altitude above mean sea level and the terrain elevation from its
    ///         altitude above terrain or GeoZoneTerrain, which is unknown without terrain data
    /// @param terrainSource Set to where the terrain elevation comes from
    static GeoZone::Position vehiclePosition(Vehicle *vehicle, const QGeoCoordinate &coordinate, TerrainSource *terrainSource = nullptr);
    /// @return Position at the altitude above mean sea level, with the terrain elevation below the height above terrain
    ///         when it is known, at the altitude when the vehicle is disarmed, and otherwise from GeoZoneTerrain
    static GeoZone::Position vehiclePosition(const QGeoCoordinate &coordinate, double altitudeAMSL, double heightAboveTerrain, bool armed, TerrainSource *terrainSource = nullptr);

    /// @return Description of a FENCE_STATUS breach like "Boundary fence breached (3 times)"
    static QString fenceBreachText(int breachType, int breachCount);

    /// Whether the vehicle is armed: that of a connected vehicle, or as set for positions passed to evaluate(). Unknown
    /// for offline vehicles and positions without.
    std::optional<bool> armed() const;
    /// Sets whether the vehicle of the positions passed to evaluate() is armed, ignored for vehicles
    void setArmed(bool armed);
    /// The vehicle is known to be disarmed, so on the ground
    bool onGround() const { return armed() == false; }
    /// The vehicle is known to be armed, so flying or about to
    bool inFlight() const { return armed() == true; }

    QString groundWarning() const { return _groundWarning; }
    int groundLevel() const { return _groundLevel; }
    /// Set by GeoZoneMonitor after each evaluation
    void setGroundStatus(const QString &warning, int level);

signals:
    void nearbyChanged();
    void keptZonesChanged();
    void verticalBandsChanged();
    void predictionChanged();
    void positionUnavailableChanged();
    void terrainSourceChanged();
    void fenceStatusChanged();
    void alertThresholdsChanged();
    /// The vehicle started breaching its fence
    void fenceBreachStarted(const QString &text);
    void armedChanged();
    /// The vehicle on the ground refused an arm command. One it accepts shows as armedChanged, like arming with the
    /// remote control.
    void armingAttempted();
    void groundStatusChanged();

private:
    void _positionReceived(qint64 msecs);
    /// Starts, changes or ends the fence breach
    /// @param known The vehicle reports it
    void _setFenceStatus(bool known, bool breached, int breachType, int breachCount);
    /// Takes the fence breach from the vehicle's position, for a vehicle which doesn't report its fence status
    void _deriveFenceStatus();
    /// @return Milliseconds usually between two position messages, 0 before the second
    qint64 _usualPositionInterval() const;
    QList<GeoZoneAlertTracker::Alert> _evaluate(const GeoZone::Position &position, const Velocity &velocity, const Context &context,
                                                const GeoZoneVehicleThresholds::Speeds &speeds);

    QPointer<Vehicle> _vehicle;
    const bool _followsVehicle = false;
    const int _vehicleId;
    const int _key;
    const Clock _clock;

    GeoZoneAlertTracker _alerts;
    QList<Nearby> _nearby;
    GeoZoneNearbyModel *_nearbyModel = nullptr;
    QList<GeoZoneVerticalBand> _bands;
    GeoZone::Position _position;
    GeoZone::Position _bandsPosition;
    std::optional<Entry> _entry;
    GeoZoneVehicleThresholds _thresholds;
    bool _fixedLimits = false;              ///< Of the last evaluation

    qint64 _positionMSecs = -1;             ///< Last position message, -1 without
    QList<qint64> _positionIntervals;       ///< Milliseconds between the last position messages, at most positionIntervalCount
    bool _globalPositionReceived = false;   ///< A GLOBAL_POSITION_INT with a position was received
    qint64 _velocityMSecs = -1;
    Velocity _reported;
    GeoZoneVelocityEstimator _estimator;
    Velocity _estimated;                    ///< Of the estimator when a position was last added
    qint64 _estimatedMSecs = 0;             ///< When that was
    qint64 _sampledMSecs = -1;              ///< Position message last added to the estimator
    bool _unavailable = false;              ///< Alerts are paused for lack of a current position
    TerrainSource _terrainSource = TerrainSource::Unknown;

    GeoZoneFenceWarning *_fenceWarning = nullptr;
    bool _fenceStatusKnown = false;
    qint64 _fenceStatusMSecs = -1;          ///< Last FENCE_STATUS, -1 without
    bool _fenceBreached = false;
    int _fenceBreachType = FENCE_BREACH_NONE;
    int _fenceBreachCount = 0;

    std::optional<bool> _armed;             ///< Of positions passed to evaluate()
    GeoZoneAwareness::Gnss _gnss;
    qint64 _gnssMSecs = -1;                 ///< Last GPS_RAW_INT, -1 without
    QString _groundWarning;
    int _groundLevel = GeoZoneAlertLevel::Advisory;
};
