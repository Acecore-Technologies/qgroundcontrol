/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>

#include "GeoZone.h"

#include <cmath>
#include <optional>

class GeoZoneCatalog;
class GeoZonePolicy;

/// Approach and entry states of the zones around one vehicle. Each zone alerts once
/// when the vehicle comes near it, by distance or by its predicted entry, and once when the vehicle enters it, with
/// hysteresis so that alerts don't repeat while the vehicle stays at a limit. States are kept by GeoZone::key, so
/// reloading a source doesn't repeat alerts.
class GeoZoneAlertTracker
{
public:
    static constexpr double hysteresisDistance = 20;        ///< Meters to move away before an alert can repeat
    static constexpr double hysteresisTime = 5;             ///< Seconds added to the approach time while a zone is near
    static constexpr double minPredictionSpeed = 0.5;       ///< Meters per second below which entries aren't predicted

    /// Velocity over ground and vertical speed
    struct Velocity {
        double north = 0;   ///< Meters per second
        double east = 0;
        double up = 0;      ///< Meters per second, 0 when unknown
        bool valid = false;

        double groundSpeed() const { return std::hypot(north, east); }
    };

    /// When a zone is near, see GeoZoneSettings
    struct Limits {
        double distance = 0;            ///< Meters horizontally
        double verticalDistance = 0;    ///< Meters vertically
        double time = 0;                ///< Seconds until the predicted entry, 0 to not predict entries
    };

    /// Zone near or around the vehicle
    struct Nearby {
        GeoZone *zone = nullptr;
        bool inside = false;
        int distance = 0;       ///< Rounded meters
        int timeToEntry = -1;   ///< Rounded seconds until the vehicle enters at its current velocity, -1 when it doesn't

        friend bool operator==(const Nearby &, const Nearby &) = default;
    };

    /// Zone newly approached or entered
    struct Alert {
        GeoZone *zone = nullptr;
        bool inside = false;            ///< Entered, otherwise approached
        double distance = 0;            ///< Meters to the zone, 0 inside
        double timeToEntry = qQNaN();   ///< Seconds until the predicted entry of an approach, NaN when not predicted
    };

    /// Where and when the vehicle enters a zone at its current velocity
    struct Entry {
        const GeoZone *zone = nullptr;
        double time = qQNaN();          ///< Seconds
        GeoZone::Position position;
    };

    struct Update {
        QList<Nearby> nearby;   ///< The most restrictive first, then those the vehicle is in, then the closest
        QList<Alert> alerts;    ///< The most restrictive first, entries before approaches
        std::optional<Entry> firstEntry;    ///< Earliest predicted entry into a nearby zone within the approach time
    };

    /// Updates the states of the zones accepted by the policy, usually GeoZonePolicy::alerts, for a position of the
    /// vehicle. Unknown altitudes are inside all vertical limits. With a valid velocity, zones the vehicle would enter
    /// within the approach time are near too.
    Update update(const GeoZoneCatalog &catalog, const GeoZonePolicy &policy, const GeoZone::Position &position, const Velocity &velocity, const Limits &limits);

    /// Forgets the states, so that the alerts repeat
    void reset() { _states.clear(); }
    /// Forgets the states of zones by GeoZone::key, like zones whose data changed, so that they alert again
    void forget(const QSet<QString> &zoneKeys);

    /// @return Straight path from the position at the velocity for the seconds, with the terrain along it from
    ///         GeoZoneTerrain where it is loaded and the terrain below the vehicle elsewhere
    static QList<GeoZone::Position> predictedPath(const GeoZone::Position &position, const Velocity &velocity, double seconds);
    /// @return Seconds until the predicted path covering the horizon at a constant speed enters the zone, NaN if it doesn't
    static double timeToEntry(const GeoZone *zone, const QList<GeoZone::Position> &path, double horizon);
    /// @return Time and position where the predicted path covering the horizon at a constant speed enters the zone
    static std::optional<Entry> predictedEntry(const GeoZone *zone, const QList<GeoZone::Position> &path, double horizon);

private:
    enum class State {
        Away,
        Near,
        Inside,
    };

    QHash<QString, State> _states;  ///< Zones which are not Away by GeoZone::key
};

/// Velocity of a vehicle estimated from its recent positions
class GeoZoneVelocityEstimator
{
public:
    static constexpr int windowMSecs = 3000;    ///< Positions over which the velocity is estimated

    /// @return Velocity from the positions of the last windowMSecs, after adding the position
    GeoZoneAlertTracker::Velocity add(qint64 msecs, const GeoZone::Position &position);

private:
    struct Sample {
        qint64 msecs = 0;
        GeoZone::Position position;
    };

    QList<Sample> _samples;     ///< Oldest first
};

/// Volume of a zone above or below a position
struct GeoZoneVerticalBand
{
    GeoZone *zone = nullptr;
    double floor = qQNaN();     ///< Meters AMSL, NaN when the zone extends to the ground or the limit is unknown
    double ceiling = qQNaN();   ///< Meters AMSL, NaN without a ceiling or when the limit is unknown

    friend bool operator==(const GeoZoneVerticalBand &a, const GeoZoneVerticalBand &b)
    {
        const auto same = [](double x, double y) { return (x == y) || (std::isnan(x) && std::isnan(y)); };
        return (a.zone == b.zone) && same(a.floor, b.floor) && same(a.ceiling, b.ceiling);
    }

    /// @return Volumes of the zones accepted by the policy whose horizontal projection contains the position
    static QList<GeoZoneVerticalBand> at(const GeoZoneCatalog &catalog, const GeoZonePolicy &policy, const GeoZone::Position &position);
};
