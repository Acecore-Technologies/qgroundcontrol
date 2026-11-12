/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QtNumeric>

#include "GeoZoneAlertTracker.h"

/// How far and how long before a zone a vehicle is warned, after EN 4709-003 Annex A, so that the pilot can still keep
/// it out of the zone after reacting. The thresholds are computed at each evaluation from the vehicle's ground speed v,
/// vertical speed w and position accuracy, with the manufacturer's Profile of its vehicle class:
///
///     D_h = max(minDistance, K·σ_h + v·(t_r + t_e) + s_m + W·(t_r + t_m) + margin) + extra distance + v·extra time
///     D_v = max(minVerticalDistance, K·σ_v + w·(t_r + t_e) + s_v + W_v·(t_r + t_v) + vertical margin) + w·extra time
///     T   = min(maxTime, t_r + max(t_m, t_v) + t_e + m / s) + extra time
///
/// with K·σ the position uncertainty (σ_h from the accuracy the vehicle reports, else HDOP·UERE, else a default; σ_v
/// from the vertical accuracy, else a factor of σ_h), t_r the reaction time (pilot or automatic), t_e the evaluation
/// interval, the manoeuvre span s_m and time t_m of braking (v²/2a, v/a) or turning (v²/(g·tan φ), π/2·v/(g·tan φ)),
/// s_v and t_v of stopping a climb or descent (w²/2a_v, w/a_v), W the wind, and m the horizontal and vertical margins
/// (K·σ + wind drift + safety margin) along the direction of travel at the speed s (at least 1 m/s).
/// The pilot can only make the alerts earlier, with the extra time and distance.
/// While the velocity of a flying vehicle isn't known, like at its first position or on links too slow to estimate it,
/// v and w are the ground speed and climb rate it reports, or else those assumed for its class
/// (Profile::assumedSpeed), rather than none: the thresholds never fall to their minimum for lack of a velocity.
class GeoZoneAlertThresholds
{
public:
    static constexpr double evaluationInterval = 1;     ///< Seconds between two evaluations of a vehicle, t_e
    static constexpr double maxTime = 120;              ///< Seconds, the longest time threshold before the extra time
    static constexpr double gravity = 9.80665;
    /// The profiles compiled in, and those of custom builds which replace them, see profiles()
    static constexpr const char *profilesResource = ":/geozone/GeoZoneAlertProfiles.json";
    static constexpr const char *customProfilesResource = ":/Custom/GeoZone/GeoZoneAlertProfiles.json";
    static constexpr const char *defaultClass = "default";

    /// Who reacts to the alert
    enum class Reaction {
        Pilot,          ///< A human pilot, after EN 4709-003 Annex A typically 5 s
        Automatic,      ///< The vehicle or the ground station, like an automatic fence warning, typically 3 s
    };

    /// How the vehicle avoids a zone ahead
    enum class Manoeuvre {
        Brake,          ///< Stops, like a multirotor
        Turn,           ///< Turns away, like a fixed wing aircraft
    };

    /// Thresholds the manufacturer defines for a vehicle class, see the class description
    struct Profile {
        QString vehicleClass = QString::fromLatin1(defaultClass);
        QString name;                           ///< Shown to the pilot, like "Multirotor"
        double positionErrorFactor = 2;         ///< K, times the 1σ position error
        double uere = 5;                        ///< Meters, user equivalent range error: σ_h = HDOP·UERE
        double defaultPositionError = 10;       ///< Meters, σ_h when the vehicle reports neither accuracy nor HDOP
        double verticalErrorFactor = 1.5;       ///< σ_v = factor·σ_h when the vehicle reports no vertical accuracy
        double pilotReactionTime = 5;           ///< Seconds
        double automaticReactionTime = 3;       ///< Seconds
        Manoeuvre manoeuvre = Manoeuvre::Brake;
        double deceleration = 2;                ///< Meters per second², braking horizontally
        double bankAngle = 30;                  ///< Degrees, turning
        double verticalDeceleration = 1.5;      ///< Meters per second², stopping a climb or descent
        double maxWind = 10;                    ///< Meters per second, horizontally
        double maxVerticalWind = 1;             ///< Meters per second, up- and downdrafts
        double safetyMargin = 20;               ///< Meters horizontally
        double verticalSafetyMargin = 10;       ///< Meters vertically
        double minDistance = 100;               ///< Meters, the smallest horizontal distance threshold
        double minVerticalDistance = 30;        ///< Meters, the smallest vertical distance threshold
        double fenceWarningMargin = 3;          ///< Seconds the geofence warning comes before the pilot reaction time
        /// Meters per second a vehicle of the class is taken to fly at, and to climb or descend at, while its velocity
        /// isn't known: its typical maximum, so that it is warned in time whatever it does
        double assumedSpeed = 20;
        double assumedVerticalSpeed = 5;
        // GNSS quality below which geo-awareness is degraded, see GeoZoneAwareness
        double minSatellites = 6;
        double maxHdop = 5;
        double maxHorizontalAccuracy = 5;       ///< Meters, as reported by the receiver
    };

    /// Where the speeds of the thresholds come from
    enum class SpeedSource {
        Velocity,       ///< The velocity of the vehicle
        Reported,       ///< The ground speed and climb rate the vehicle reports, while its velocity isn't known
        Assumed,        ///< Those assumed for its class, see Profile::assumedSpeed, while it reports none either
        Rest,           ///< None: the vehicle is on the ground, or a position evaluated without a velocity
    };

    /// What the vehicle reports
    struct Input {
        GeoZoneAlertTracker::Velocity velocity;
        double horizontalAccuracy = qQNaN();    ///< Meters, 1σ, NaN when unknown
        double verticalAccuracy = qQNaN();      ///< Meters, 1σ, NaN when unknown
        double hdop = qQNaN();                  ///< NaN when unknown
        /// While the velocity isn't valid: where the speeds come from instead, with the ground speed and the climb or
        /// descent rate the vehicle reports for SpeedSource::Reported. The monitor of a flying vehicle sets them, so
        /// that a vehicle whose velocity isn't known is not taken to hover.
        SpeedSource speedSource = SpeedSource::Rest;
        double groundSpeed = 0;                 ///< Meters per second
        double verticalSpeed = 0;               ///< Meters per second, up or down
    };

    /// What the pilot adds to the manufacturer's thresholds, never less than 0
    struct Extras {
        double time = 0;        ///< Seconds
        double distance = 0;    ///< Meters horizontally
    };

    struct Result {
        double distance = 0;                ///< D_h, meters horizontally from a zone
        double verticalDistance = 0;        ///< D_v, meters below the floor or above the ceiling of a zone
        double time = 0;                    ///< T, seconds before the predicted entry into a zone
        double groundSpeed = 0;             ///< Meters per second the thresholds are for, see speedSource
        double verticalSpeed = 0;           ///< Meters per second, up positive with a velocity, otherwise the rate either way
        SpeedSource speedSource = SpeedSource::Rest;    ///< Where the speeds come from
        double positionError = 0;           ///< K·σ_h, meters
        double verticalPositionError = 0;   ///< K·σ_v, meters
        double reactionTime = 0;            ///< t_r, seconds
        double manoeuvreTime = 0;           ///< max(t_m, t_v), seconds
        double manoeuvreSpan = 0;           ///< s_m, meters
        double windDrift = 0;               ///< W·(t_r + t_m), meters
        double extraTime = 0;               ///< Seconds the pilot added
        double extraDistance = 0;           ///< Meters the pilot added
        QString vehicleClass;               ///< Of the profile
        QString profileName;

        /// @return The limits of the tracker, whose predicted entries within the reaction time and the evaluation
        ///         interval are imminent
        GeoZoneAlertTracker::Limits limits() const { return GeoZoneAlertTracker::Limits{ distance, verticalDistance, time, reactionTime + evaluationInterval }; }
        /// @return Seconds of warning at least before a predicted entry: the reaction and manoeuvre times and the
        ///         evaluation interval
        double minWarningTime() const { return reactionTime + manoeuvreTime + evaluationInterval; }
    };

    /// @return The thresholds of the profile for what the vehicle reports
    static Result compute(const Profile &profile, const Input &input, const Extras &extras, Reaction reaction = Reaction::Pilot);
    /// @return The thresholds of the profile for a pilot without extras
    static Result compute(const Profile &profile, const Input &input) { return compute(profile, input, Extras()); }

    /// @return Profiles of a JSON document by vehicle class, their missing values from the default class or the
    ///         defaults of Profile, empty when the document can't be read
    static QHash<QString, Profile> parseProfiles(const QByteArray &json, QString *errorString = nullptr);
    /// @return The manufacturer's profiles: those of customProfilesResource when a custom build has it, otherwise those of
    ///         profilesResource, loaded once
    static const QHash<QString, Profile> &profiles();
    /// @return The manufacturer's profile of the vehicle class (multirotor, fixedwing, vtol), that of the default class
    ///         for other classes
    static Profile profile(const QString &vehicleClass);
};
