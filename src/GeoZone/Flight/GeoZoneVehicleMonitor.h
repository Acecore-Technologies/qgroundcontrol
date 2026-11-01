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
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"
#include "GeoZoneAlertTracker.h"
#include "GeoZonePolicy.h"
#include "QGCMAVLink.h"

#include <functional>
#include <optional>

class GeoZoneCatalog;
class Vehicle;

/// Monitors one vehicle against the zones for GeoZoneMonitor: the messages of the vehicle giving its position, velocity
/// and fence status, the approach and entry states of the zones around it (GeoZoneAlertTracker), where it would enter
/// a zone at its velocity and the zones above and below it. A vehicle which reconnects gets a new monitor, so it
/// starts clean even with the same system id.
class GeoZoneVehicleMonitor : public QObject
{
    Q_OBJECT

public:
    using Velocity = GeoZoneAlertTracker::Velocity;
    using Nearby = GeoZoneAlertTracker::Nearby;
    using Entry = GeoZoneAlertTracker::Entry;
    /// @return Monotonic milliseconds
    using Clock = std::function<qint64()>;

    static constexpr int positionTimeoutMSecs = 5000;           ///< Positions are stale after this, or three times their usual interval
    static constexpr int reportedVelocityMaxAgeMSecs = 2000;    ///< Velocities reported by the vehicle are used up to this age

    /// What the vehicle is evaluated against, which GeoZoneMonitor keeps until the settings or the zones change
    struct Context {
        const GeoZoneCatalog *catalog = nullptr;
        GeoZonePolicy policy;
        GeoZoneAlertTracker::Limits limits;
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

    /// Records when the vehicle sent its position, the velocity sent with it and the status of its fence
    /// @param msecs Time of the clock
    void handleMessage(const mavlink_message_t &message, qint64 msecs);

    /// @return Velocity the vehicle reported with its position when recent, otherwise estimated from its positions,
    ///         after adding the position to the estimate
    Velocity velocity(qint64 msecs, const GeoZone::Position &position);

    /// @return true if the vehicle sent no position for longer than its timeout
    bool positionStale(qint64 msecs) const;

    /// Evaluates the current position of the vehicle, unless it has none: its coordinate is invalid, its link is lost
    /// or its position stale. Then its alerts pause and positionUnavailable() is true.
    /// @return Zones newly approached or entered, see GeoZoneAlertTracker::Update
    QList<GeoZoneAlertTracker::Alert> update(const Context &context);

    /// Evaluates a position of the vehicle. Unknown altitudes are inside all vertical limits. With a valid velocity,
    /// zones the vehicle would enter within the approach time are near too.
    QList<GeoZoneAlertTracker::Alert> evaluate(const GeoZone::Position &position, const Velocity &velocity, const Context &context);

    /// Zones near or around the vehicle, the most restrictive first
    const QList<Nearby> &nearby() const { return _nearby; }
    /// Keys of the nearby zones the vehicle is inside, see GeoZone::key
    QSet<QString> insideZoneKeys() const;

    /// Volumes of the zones above and below the vehicle
    const QList<GeoZoneVerticalBand> &verticalBands() const { return _bands; }
    /// Position the vertical bands were last updated for
    const GeoZone::Position &bandsPosition() const { return _bandsPosition; }
    /// Last evaluated position
    const GeoZone::Position &position() const { return _position; }

    /// First predicted entry from the last evaluated position, nothing while the position is unavailable
    std::optional<Entry> entry() const { return _unavailable ? std::nullopt : _entry; }
    /// Position the entry is predicted from
    QGeoCoordinate entryFrom() const { return _entryFrom; }

    bool positionUnavailable() const { return _unavailable; }
    void setPositionUnavailable(bool unavailable);

    bool fenceStatusKnown() const { return _fenceStatusKnown; }
    bool fenceBreached() const { return _fenceBreached; }
    /// Description of the current fence breach, empty without
    QString fenceBreachText() const;

    /// Forgets the alert states, nearby zones, bands and entry, so that the alerts repeat
    void reset();
    /// Forgets the zones of a source which are about to be deleted, keeping their alert states
    void forgetZones(const QString &sourceKey);
    /// Forgets the alert states of zones by GeoZone::key, like zones whose data changed, so that they alert again
    void forgetStates(const QSet<QString> &zoneKeys) { _alerts.forget(zoneKeys); }

    /// @return Position of the vehicle with its altitude above mean sea level and the terrain elevation from its
    ///         altitude above terrain or GeoZoneTerrain, which is unknown without terrain data
    static GeoZone::Position vehiclePosition(Vehicle *vehicle, const QGeoCoordinate &coordinate);

    /// @return Description of a FENCE_STATUS breach like "Boundary fence breached (3 times)"
    static QString fenceBreachText(int breachType, int breachCount);

signals:
    void nearbyChanged();
    void verticalBandsChanged();
    void predictionChanged();
    void positionUnavailableChanged();
    void fenceStatusChanged();
    /// The vehicle started breaching its fence
    void fenceBreachStarted(const QString &text);

private:
    void _positionReceived(qint64 msecs);

    QPointer<Vehicle> _vehicle;
    const bool _followsVehicle = false;
    const int _vehicleId;
    const int _key;
    const Clock _clock;

    GeoZoneAlertTracker _alerts;
    QList<Nearby> _nearby;
    QList<GeoZoneVerticalBand> _bands;
    GeoZone::Position _position;
    GeoZone::Position _bandsPosition;
    std::optional<Entry> _entry;
    QGeoCoordinate _entryFrom;

    qint64 _positionMSecs = -1;             ///< Last position message, -1 without
    qint64 _positionIntervalMSecs = 0;      ///< Between the last two position messages
    qint64 _velocityMSecs = -1;
    Velocity _reported;
    GeoZoneVelocityEstimator _estimator;
    bool _unavailable = false;              ///< Alerts are paused for lack of a current position

    bool _fenceStatusKnown = false;
    bool _fenceBreached = false;
    int _fenceBreachType = FENCE_BREACH_NONE;
    int _fenceBreachCount = 0;
};
