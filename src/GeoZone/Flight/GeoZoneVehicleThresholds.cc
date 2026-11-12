/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneVehicleThresholds.h"

#include <cmath>
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
        // Fifths of a meter per second, and decimeters per second
        _groundSpeed = highLatency.groundspeed / 5.0;
        _climbRate = highLatency.climb_rate / 10.0;
        _speedMSecs = msecs;
        break;
    }
    case MAVLINK_MSG_ID_HIGH_LATENCY: {
        mavlink_high_latency_t highLatency{};
        mavlink_msg_high_latency_decode(&message, &highLatency);
        _groundSpeed = highLatency.groundspeed;
        _climbRate = highLatency.climb_rate;
        _speedMSecs = msecs;
        break;
    }
    case MAVLINK_MSG_ID_VFR_HUD: {
        mavlink_vfr_hud_t hud{};
        mavlink_msg_vfr_hud_decode(&message, &hud);
        _groundSpeed = hud.groundspeed;
        _climbRate = hud.climb;
        _speedMSecs = msecs;
        break;
    }
    default:
        break;
    }
}

GeoZoneVehicleThresholds::Speeds GeoZoneVehicleThresholds::speedsWithoutVelocity(qint64 msecs, bool onGround, qint64 maxAgeMSecs) const
{
    using Source = GeoZoneAlertThresholds::SpeedSource;
    if (onGround) {
        return Speeds{ Source::Rest };
    }
    const bool reported = (_speedMSecs >= 0) && ((msecs - _speedMSecs) <= maxAgeMSecs) && std::isfinite(_groundSpeed) && std::isfinite(_climbRate);
    // Unknown, the vehicle may fly as fast as its class does
    return reported ? Speeds{ Source::Reported, std::abs(_groundSpeed), std::abs(_climbRate) } : Speeds{ Source::Assumed };
}

const GeoZoneAlertThresholds::Result &GeoZoneVehicleThresholds::update(qint64 msecs, const GeoZoneAlertTracker::Velocity &velocity, const GeoZoneAlertThresholds::Extras &extras,
                                                                        const Speeds &speeds)
{
    GeoZoneAlertThresholds::Input input;
    input.velocity = velocity;
    input.speedSource = speeds.source;
    input.groundSpeed = speeds.ground;
    input.verticalSpeed = speeds.vertical;
    if ((_accuracyMSecs >= 0) && ((msecs - _accuracyMSecs) <= accuracyMaxAgeMSecs)) {
        input.horizontalAccuracy = _horizontalAccuracy;
        input.verticalAccuracy = _verticalAccuracy;
        input.hdop = _hdop;
    }
    _result = GeoZoneAlertThresholds::compute(GeoZoneAlertThresholds::profile(_vehicleClass), input, extras);
    _updated = true;
    return _result;
}

QString GeoZoneVehicleThresholds::speedSourceCode(GeoZoneAlertThresholds::SpeedSource source)
{
    switch (source) {
    case GeoZoneAlertThresholds::SpeedSource::Velocity:
        return QStringLiteral("velocity");
    case GeoZoneAlertThresholds::SpeedSource::Reported:
        return QStringLiteral("reported");
    case GeoZoneAlertThresholds::SpeedSource::Assumed:
        return QStringLiteral("assumed");
    case GeoZoneAlertThresholds::SpeedSource::Rest:
        break;
    }
    return QStringLiteral("rest");
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
        { QStringLiteral("speedSource"), speedSourceCode(_result.speedSource) },
        { QStringLiteral("positionError"), _result.positionError },
        { QStringLiteral("reactionTime"), _result.reactionTime },
        { QStringLiteral("extraTime"), _result.extraTime },
        { QStringLiteral("extraDistance"), _result.extraDistance },
        { QStringLiteral("profileName"), _result.profileName },
    };
}
