/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QList>

#include "GeoZone.h"

/// Which zones a check applies to, by restriction and schedule. The named policies keep the mission check, routes,
/// altitude profile, alerts and guided command checks in agreement.
class GeoZonePolicy
{
public:
    enum class Schedule {
        Any,        ///< Whatever their schedule
        Current,    ///< Active now, see GeoZone::active
        Planned,    ///< Active at some time between the start and the end, or at the start without an end
    };

    /// Accepts no zone
    GeoZonePolicy() = default;
    /// Accepts the restrictions whatever the schedule
    explicit GeoZonePolicy(const QList<GeoZone::Restriction> &restrictions);

    /// Zones the vehicles are alerted about and guided commands are checked against while they are active: the
    /// restricted zones and U-space airspace
    static GeoZonePolicy alerts();
    /// Zones the mission check lists during the planned flight: all of them, see isRestricted for warnings
    static GeoZonePolicy mission(const QDateTime &start, const QDateTime &end);
    /// Zones routes go around during the planned flight, the restricted ones
    static GeoZonePolicy routing(const QDateTime &start, const QDateTime &end);
    /// Zones drawn in the altitude profile during the planned flight, the restricted ones and U-space airspace
    static GeoZonePolicy profile(const QDateTime &start, const QDateTime &end);

    /// @return true for the restrictions the pilot is warned about before a flight: prohibited, authorisation
    ///         required, conditional and unknown restrictions
    static bool isRestricted(GeoZone::Restriction restriction);

    /// @return The policy for zones active now
    GeoZonePolicy whileActive() const;
    /// @return The policy for zones active at some time between start and end, or at start when end is invalid,
    ///         or whatever their schedule when start is invalid
    GeoZonePolicy activeDuring(const QDateTime &start, const QDateTime &end = QDateTime()) const;
    /// @return The policy for zones whatever their schedule
    GeoZonePolicy anyTime() const;

    bool acceptsRestriction(GeoZone::Restriction restriction) const { return (_restrictions & _bit(restriction)) != 0; }
    bool acceptsSchedule(const GeoZone *zone) const;
    bool accepts(const GeoZone *zone) const { return acceptsRestriction(zone->restriction()) && acceptsSchedule(zone); }

    Schedule schedule() const { return _schedule; }

private:
    static quint32 _bit(GeoZone::Restriction restriction) { return 1u << static_cast<int>(restriction); }

    quint32 _restrictions = 0;
    Schedule _schedule = Schedule::Any;
    QDateTime _start;
    QDateTime _end;
};
