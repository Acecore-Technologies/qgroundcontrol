/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneVehicleThresholds.h"

#include <limits>

QString GeoZoneVehicleThresholds::vehicleClass(int mavType)
{
    switch (QGCMAVLink::vehicleClass(static_cast<MAV_TYPE>(mavType))) {
    case QGCMAVLink::VehicleClassMultiRotor:
        return QStringLiteral("multirotor");
    case QGCMAVLink::VehicleClassFixedWing:
        return QStringLiteral("fixedwing");
    case QGCMAVLink::VehicleClassVTOL:
        return QStringLiteral("vtol");
    default:
        return QString::fromLatin1(GeoZoneAlertThresholds::defaultClass);
    }
}

void GeoZoneVehicleThresholds::handleMessage(const mavlink_message_t &message, qint64 msecs)
{
    switch (message.msgid) {
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
        mavlink_gps_raw_int_t gps{};
        mavlink_msg_gps_raw_int_decode(&message, &gps);
        const bool fix = gps.fix_type >= GPS_FIX_TYPE_2D_FIX;
        // Millimeters, 0 when the receiver doesn't report them
        const auto accuracy = [fix](uint32_t mm) {
            return (fix && (mm > 0) && (mm != std::numeric_limits<uint32_t>::max())) ? (mm / 1000.0) : qQNaN();
        };
        _horizontalAccuracy = accuracy(gps.h_acc);
        _verticalAccuracy = accuracy(gps.v_acc);
        _hdop = (fix && (gps.eph != UINT16_MAX)) ? (gps.eph / 100.0) : qQNaN();
        _accuracyMSecs = msecs;
        break;
    }
    case MAVLINK_MSG_ID_HIGH_LATENCY2: {
        mavlink_high_latency2_t highLatency{};
        mavlink_msg_high_latency2_decode(&message, &highLatency);
        // Decimeters, the largest error since the last message
        _horizontalAccuracy = (highLatency.eph > 0) ? (highLatency.eph / 10.0) : qQNaN();
        _verticalAccuracy = (highLatency.epv > 0) ? (highLatency.epv / 10.0) : qQNaN();
        _hdop = qQNaN();
        _accuracyMSecs = msecs;
        break;
    }
    default:
        break;
    }
}

const GeoZoneAlertThresholds::Result &GeoZoneVehicleThresholds::update(qint64 msecs, const GeoZoneAlertTracker::Velocity &velocity, const GeoZoneAlertThresholds::Extras &extras)
{
    GeoZoneAlertThresholds::Input input;
    input.velocity = velocity;
    if ((_accuracyMSecs >= 0) && ((msecs - _accuracyMSecs) <= accuracyMaxAgeMSecs)) {
        input.horizontalAccuracy = _horizontalAccuracy;
        input.verticalAccuracy = _verticalAccuracy;
        input.hdop = _hdop;
    }
    _result = GeoZoneAlertThresholds::compute(GeoZoneAlertThresholds::profile(_vehicleClass), input, extras);
    _updated = true;
    return _result;
}

QVariantMap GeoZoneVehicleThresholds::toVariantMap() const
{
    if (!_updated) {
        return QVariantMap();
    }
    return QVariantMap{
        { QStringLiteral("distance"), _result.distance },
        { QStringLiteral("verticalDistance"), _result.verticalDistance },
        { QStringLiteral("time"), _result.time },
        { QStringLiteral("groundSpeed"), _result.groundSpeed },
        { QStringLiteral("verticalSpeed"), _result.verticalSpeed },
        { QStringLiteral("positionError"), _result.positionError },
        { QStringLiteral("reactionTime"), _result.reactionTime },
        { QStringLiteral("extraTime"), _result.extraTime },
        { QStringLiteral("extraDistance"), _result.extraDistance },
        { QStringLiteral("profileName"), _result.profileName },
    };
}
