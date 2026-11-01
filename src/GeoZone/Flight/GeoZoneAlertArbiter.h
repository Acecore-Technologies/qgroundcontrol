/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>

#include <functional>
#include <limits>

/// Decides which geozone alerts are announced and when, so that several alerts don't produce a burst of messages and a
/// vehicle inside a prohibited zone isn't forgotten. It keeps the state of the alerts (pending, repeated, acknowledged
/// and muted) and has no output of its own: GeoZoneMonitor calls tick() after each evaluation of the vehicles, every
/// second while it monitors them, shows the repeats of the announcement and speaks its text when spoken alerts are on.
/// So repeats, acknowledgements and muting work the same with the audio turned off.
/// - The alerts submitted since the last tick are announced as one message, the most urgent first: the most severe
///   restriction, then entries before repeats before approaches. The message names maxSpokenAlerts of them and
///   counts the others ("and 2 more").
/// - Messages are at least minIntervalMSecs apart. Alerts arriving sooner wait for a later tick, unless they are more
///   urgent than the last message, which they preempt.
/// - Entering a prohibited zone (a breach) is repeated every repeatIntervalMSecs until its alert is acknowledged or
///   the vehicle leaves the zone. While the position of the vehicle is lost, its breaches are suspended: kept, since
///   it may still be inside, but not repeated.
/// - Muting silences the alerts and repeats for a while, except the first announcement of a new breach. Alerts
///   raised while muted are dropped, not announced later.
class GeoZoneAlertArbiter
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneAlertArbiter)

public:
    /// @return Monotonic milliseconds
    using Clock = std::function<qint64()>;

    static constexpr int minIntervalMSecs = 5000;       ///< Between two messages, unless the second is more urgent
    static constexpr int repeatIntervalMSecs = 30000;   ///< Between the announcements of an unacknowledged breach
    static constexpr int defaultMuteMSecs = 60000;
    static constexpr int maxSpokenAlerts = 2;           ///< Alerts named in a message, the others are counted

    struct Alert {
        int id = 0;             ///< Unique for the session, see acknowledge()
        int vehicleKey = 0;     ///< Unique for each monitored vehicle, a vehicle which reconnects gets a new one
        QString zoneKey;        ///< GeoZone::key
        int severity = 0;       ///< GeoZone::restrictionSeverity, the most severe is 0
        bool inside = false;    ///< Entered, otherwise approached
        bool breach = false;    ///< Entered a prohibited zone: repeated until acknowledged or left, and never muted
        QString message;        ///< Announced
        QString repeatMessage;  ///< Announced when a breach is repeated
    };

    /// What a tick announces
    struct Announcement {
        QString text;               ///< Empty when nothing is announced
        QList<Alert> alerts;        ///< Named or counted, the most urgent first
        QList<Alert> repeated;      ///< Breaches among them which were announced before
    };

    /// @param clock A timer started now when empty
    explicit GeoZoneAlertArbiter(const Clock &clock = Clock());

    void setClock(const Clock &clock);

    /// Adds an alert to announce at the next tick. Breaches are announced again until acknowledged or left.
    void submit(const Alert &alert);

    /// Ends the breaches of the vehicle in zones other than those it is inside, by GeoZone::key
    void setInsideZones(int vehicleKey, const QSet<QString> &zoneKeys);
    /// Suspends the repeats of the breaches of a vehicle whose position is lost, or resumes them
    void setSuspended(int vehicleKey, bool suspended);
    bool suspended(int vehicleKey) const { return _suspended.contains(vehicleKey); }
    /// Forgets the alerts, breaches and suspension of a vehicle no longer monitored
    void removeVehicle(int vehicleKey);
    /// Forgets all alerts and breaches, keeping the mute
    void clear();

    /// Stops repeating the breach of the alert, and drops the alert if it wasn't spoken yet
    void acknowledge(int alertId);
    void acknowledgeAll();

    /// Silences the alerts for the milliseconds, see the class description
    void mute(qint64 msecs = defaultMuteMSecs);
    void unmute() { _mutedUntilMSecs = -1; }
    bool muted() const { return (_mutedUntilMSecs >= 0) && (_now() < _mutedUntilMSecs); }
    /// @return Milliseconds of the clock until which the alerts are muted, -1 when they aren't
    qint64 mutedUntil() const { return muted() ? _mutedUntilMSecs : -1; }

    /// @return Breaches not acknowledged, which are repeated
    int breachCount() const { return static_cast<int>(_breaches.count()); }
    int pendingCount() const { return static_cast<int>(_pending.count()); }

    /// Announces the alerts submitted since the last tick and the breaches due to be repeated, as the rules allow
    Announcement tick();

private:
    struct Breach {
        Alert alert;
        qint64 announcedMSecs = -1;     ///< Last announcement, -1 before the first
    };

    struct Candidate {
        Alert alert;
        bool repeat = false;
    };

    /// @return Lower for more urgent alerts: by severity, then entries, repeats and approaches
    static int _urgency(const Candidate &candidate);
    qint64 _now() const { return _clock(); }

    Clock _clock;
    QList<Alert> _pending;          ///< Submitted, not spoken yet, in submission order
    QList<Breach> _breaches;
    QSet<int> _suspended;           ///< Vehicle keys
    qint64 _lastSpokenMSecs = -1;   ///< -1 before the first message
    int _lastSpokenUrgency = std::numeric_limits<int>::max();
    qint64 _mutedUntilMSecs = -1;
};
