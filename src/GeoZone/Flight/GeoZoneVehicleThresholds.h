/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QString>
#include <QtCore/QVariantMap>
#include <QtCore/QtNumeric>

#include "GeoZoneAlertThresholds.h"
#include "QGCMAVLink.h"

/// Alert thresholds of one vehicle (GeoZoneAlertThresholds) from what it reports: its MAVLink vehicle type picks the
/// manufacturer's profile, the accuracy of its GNSS position (GPS_RAW_INT h_acc and v_acc, else its HDOP, or the
/// position error of HIGH_LATENCY2) widens them. GeoZoneVehicleMonitor updates them at each evaluation. While the
/// velocity of the vehicle isn't known, they are those at the ground speed and climb rate it reports (VFR_HUD, or the
/// high latency messages), or else at the speeds assumed for its class, see speedsWithoutVelocity().
class GeoZoneVehicleThresholds
{
public:
    static constexpr int accuracyMaxAgeMSecs = 5000;    ///< Accuracies reported earlier are unknown

    /// What the thresholds are computed at while the velocity isn't valid
    struct Speeds {
        GeoZoneAlertThresholds::SpeedSource source = GeoZoneAlertThresholds::SpeedSource::Rest;
        double ground = 0;      ///< Meters per second, for SpeedSource::Reported
        double vertical = 0;    ///< Meters per second up or down
    };

    /// @return Profile class of a MAVLink vehicle type: multirotor, fixedwing, vtol or default
    static QString vehicleClass(int mavType);
    /// @return Code of the source for QML and logs: "velocity", "reported", "assumed" or "rest"
    static QString speedSourceCode(GeoZoneAlertThresholds::SpeedSource source);

    /// Profile of the vehicle class, the default class until set
    void setVehicleClass(const QString &vehicleClass) { _vehicleClass = vehicleClass; }
    const QString &vehicleClass() const { return _vehicleClass; }

    /// Records the accuracy of the vehicle's position, and its ground speed and climb rate
    /// @param msecs Time of the clock of GeoZoneVehicleMonitor
    void handleMessage(const mavlink_message_t &message, qint64 msecs);

    /// @return What the thresholds of a vehicle without a valid velocity are computed at: nothing on the ground,
    ///         the ground speed and climb rate it reported within the maximum age, or else those assumed for its class
    /// @param onGround The vehicle is known to be disarmed
    Speeds speedsWithoutVelocity(qint64 msecs, bool onGround, qint64 maxAgeMSecs) const;

    /// Computes the thresholds for the velocity, with the accuracy reported within accuracyMaxAgeMSecs
    /// @param speeds Used while the velocity isn't valid
    const GeoZoneAlertThresholds::Result &update(qint64 msecs, const GeoZoneAlertTracker::Velocity &velocity, const GeoZoneAlertThresholds::Extras &extras,
                                                 const Speeds &speeds);
    const GeoZoneAlertThresholds::Result &update(qint64 msecs, const GeoZoneAlertTracker::Velocity &velocity, const GeoZoneAlertThresholds::Extras &extras)
    {
        return update(msecs, velocity, extras, Speeds());
    }
    /// Of the last update()
    const GeoZoneAlertThresholds::Result &result() const { return _result; }
    /// @return false before the first update()
    bool updated() const { return _updated; }
    /// @return The last result for QML: distance, verticalDistance and time thresholds, groundSpeed, verticalSpeed,
    ///         speedSource ("velocity", "reported", "assumed" or "rest", see GeoZoneAlertThresholds::SpeedSource),
    ///         positionError, reactionTime, extraTime, extraDistance and profileName, empty before the first update()
    QVariantMap toVariantMap() const;

private:
    QString _vehicleClass = QString::fromLatin1(GeoZoneAlertThresholds::defaultClass);
    double _horizontalAccuracy = qQNaN();
    double _verticalAccuracy = qQNaN();
    double _hdop = qQNaN();
    qint64 _accuracyMSecs = -1;     ///< Of the last accuracy, -1 without
    double _groundSpeed = qQNaN();  ///< Meters per second as reported, NaN when unknown
    double _climbRate = qQNaN();
    qint64 _speedMSecs = -1;        ///< Of the last speeds, -1 without
    GeoZoneAlertThresholds::Result _result;
    bool _updated = false;
};
