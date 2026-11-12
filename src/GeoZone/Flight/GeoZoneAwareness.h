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
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QtNumeric>
#include <QtPositioning/QGeoCoordinate>

#include <optional>

#include "GeoZoneAlertLevel.h"
#include "GeoZoneAlertThresholds.h"

class GeoZoneManager;

/// Whether geo-awareness is available for a vehicle (EN 4709-003 §5.2.3): zones are loaded, current, updated and cover
/// the position the vehicle sends over its link with a good GNSS fix, and its alerts are logged. Each problem is a
/// reason the state is degraded or unavailable. GeoZoneMonitor publishes the status of the active vehicle, or of the data alone without one, and
/// alerts the changes of state of each flying vehicle, see Tracker.
class GeoZoneAwareness : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int          state       READ stateValue NOTIFY changed FINAL)    ///< State
    Q_PROPERTY(QString      stateText   READ stateText  NOTIFY changed FINAL)    ///< Like "Degraded"
    Q_PROPERTY(QStringList  reasons     READ reasons    NOTIFY changed FINAL)    ///< Why it isn't available, the worst first
    Q_PROPERTY(QString      summary     READ summary    NOTIFY changed FINAL)    ///< Like "Geo-awareness degraded: GNSS poor (2D fix)"

public:
    enum State {
        Available,
        Degraded,       ///< Zones are checked, but they or the vehicle's position may be wrong
        Unavailable,    ///< Zones can't be checked
    };
    Q_ENUM(State)

    enum class Reason {
        LinkLost,       ///< The telemetry link of the vehicle is lost
        NoPosition,     ///< The vehicle sends no current position
        GnssLost,       ///< The vehicle reports no GNSS fix
        NoData,         ///< No zones are loaded
        GnssPoor,       ///< The fix is below the GnssLimits
        DataExpired,    ///< Outdated data treated as invalid, see GeoZoneSourceRepository::dataInvalid
        DataOutdated,   ///< See GeoZoneSourceRepository::validityWarning
        RefreshFailed,  ///< The last download or load of a source failed
        NotCovered,     ///< No source has zones around the vehicle position
        LogNotWritten,  ///< The alerts can't be written to the alert log, see GeoZoneAlertHistory::logFailing
    };

    /// GNSS fix below which geo-awareness is degraded, set by the manufacturer in the alert profile of the vehicle
    /// class, see GeoZoneAlertThresholds::Profile
    struct GnssLimits {
        static constexpr int minFixType = 3;    ///< GPS_FIX_TYPE_3D_FIX
        double minSatellites = 6;
        double maxHdop = 5;                     ///< Worse than "moderate"
        double maxHorizontalAccuracy = 5;       ///< Meters, as reported by the receiver

        /// @return The limits of the profile
        static GnssLimits fromProfile(const GeoZoneAlertThresholds::Profile &profile)
        {
            return GnssLimits{ profile.minSatellites, profile.maxHdop, profile.maxHorizontalAccuracy };
        }
    };

    /// GNSS status the vehicle reports with GPS_RAW_INT
    struct Gnss {
        bool known = false;             ///< Reported recently
        int fixType = 0;                ///< GPS_FIX_TYPE
        int satellites = -1;            ///< -1 when unknown
        double hdop = qQNaN();
        double horizontalAccuracy = qQNaN();    ///< Meters, NaN when unknown
    };

    /// What the status is evaluated from
    struct Input {
        int zoneCount = 0;
        QString validityWarning;        ///< See GeoZoneSourceRepository::validityWarning
        bool dataInvalid = false;
        QStringList failedSources;      ///< Names of the sources whose last refresh failed
        bool vehicle = false;           ///< Of a vehicle, otherwise of the data alone
        bool linkLost = false;
        bool positionUnavailable = false;
        std::optional<bool> covered;    ///< A source covers the vehicle position, unknown without position or zones
        Gnss gnss;
        GnssLimits gnssLimits;          ///< Of the vehicle's profile
        bool logFailing = false;        ///< The alert log can't be written
    };

    struct Issue {
        Reason reason = Reason::NoData;
        State state = Unavailable;
        QString text;                   ///< Like "GNSS poor (2D fix)"

        friend bool operator==(const Issue &, const Issue &) = default;
    };

    struct Status {
        State state = Available;
        QList<Issue> issues;            ///< The worst first

        QStringList reasons() const;
        /// @return Like "Geo-awareness degraded: GNSS poor (2D fix); geozone data may be outdated"
        QString summary() const;
        bool has(Reason reason) const;

        friend bool operator==(const Status &, const Status &) = default;
    };

    /// Follows the state of a vehicle for alerts while it flies: a change of state lasting worseSettleMSecs, or
    /// betterSettleMSecs for a better state, is alerted once. On the ground the state is taken as available, so a
    /// vehicle arming without geo-awareness is alerted.
    class Tracker
    {
    public:
        static constexpr int worseSettleMSecs = 2000;
        static constexpr int betterSettleMSecs = 5000;

        struct Change {
            State from = Available;
            State to = Available;
            Status status;
        };

        /// @param inFlight The vehicle is armed
        /// @param msecs Monotonic milliseconds
        /// @return The change of state to alert, nothing otherwise
        std::optional<Change> update(const Status &status, bool inFlight, qint64 msecs);

    private:
        State _alerted = Available;
        std::optional<State> _pending;
        qint64 _pendingMSecs = 0;
    };

    explicit GeoZoneAwareness(QObject *parent = nullptr);

    /// @return The state and the reasons for the input
    static Status evaluate(const Input &input);
    /// @return Input of the manager's zones and sources, and whether a source has zones around the position when it is
    ///         valid, by the bounds of their zones or the area of the service
    static Input dataInput(GeoZoneManager *manager, const QGeoCoordinate &position = QGeoCoordinate());

    static QString stateText(State state);
    /// @return Code of the state in the alert log, like "degraded"
    static QString stateCode(State state);
    static State stateFromCode(const QString &code);
    /// @return Message of an alert about the vehicle reaching the state, like "Geo-awareness unavailable: telemetry
    ///         link lost", or "Geo-awareness available again"
    /// @param reasons Of the state, joined
    static QString alertMessage(State state, const QString &reasons);
    /// @return Level of the alert of a change of state: a warning when it becomes unavailable, a
    ///         caution when it is degraded, an advisory when it is available again
    static GeoZoneAlertLevel::Level alertLevel(State to);

    const Status &status() const { return _status; }
    /// Publishes the status, of the active vehicle
    void setStatus(const Status &status);

    int stateValue() const { return static_cast<int>(_status.state); }
    QString stateText() const { return stateText(_status.state); }
    QStringList reasons() const { return _status.reasons(); }
    QString summary() const { return _status.summary(); }

signals:
    void changed();

private:
    Status _status;
};
