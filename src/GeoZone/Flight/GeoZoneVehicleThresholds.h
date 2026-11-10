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
/// position error of HIGH_LATENCY2) widens them. GeoZoneVehicleMonitor updates them at each evaluation.
class GeoZoneVehicleThresholds
{
public:
    static constexpr int accuracyMaxAgeMSecs = 5000;    ///< Accuracies reported earlier are unknown

    /// @return Profile class of a MAVLink vehicle type: multirotor, fixedwing, vtol or default
    static QString vehicleClass(int mavType);

    /// Profile of the vehicle class, the default class until set
    void setVehicleClass(const QString &vehicleClass) { _vehicleClass = vehicleClass; }
    const QString &vehicleClass() const { return _vehicleClass; }

    /// Records the accuracy of the vehicle's position
    /// @param msecs Time of the clock of GeoZoneVehicleMonitor
    void handleMessage(const mavlink_message_t &message, qint64 msecs);

    /// Computes the thresholds for the velocity, with the accuracy reported within accuracyMaxAgeMSecs
    const GeoZoneAlertThresholds::Result &update(qint64 msecs, const GeoZoneAlertTracker::Velocity &velocity, const GeoZoneAlertThresholds::Extras &extras);
    /// Of the last update()
    const GeoZoneAlertThresholds::Result &result() const { return _result; }
    /// @return The last result for QML: distance, verticalDistance and time thresholds, groundSpeed, verticalSpeed,
    ///         positionError, reactionTime, extraTime, extraDistance and profileName, empty before the first update()
    QVariantMap toVariantMap() const;

private:
    QString _vehicleClass = QString::fromLatin1(GeoZoneAlertThresholds::defaultClass);
    double _horizontalAccuracy = qQNaN();
    double _verticalAccuracy = qQNaN();
    double _hdop = qQNaN();
    qint64 _accuracyMSecs = -1;     ///< Of the last accuracy, -1 without
    GeoZoneAlertThresholds::Result _result;
    bool _updated = false;
};
