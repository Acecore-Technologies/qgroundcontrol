/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePolicy.h"

namespace {

const QList<GeoZone::Restriction> kRestricted = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::Unknown };

} // namespace

GeoZonePolicy::GeoZonePolicy(const QList<GeoZone::Restriction> &restrictions)
{
    for (const GeoZone::Restriction restriction : restrictions) {
        _restrictions |= _bit(restriction);
    }
}

GeoZonePolicy GeoZonePolicy::alerts()
{
    return GeoZonePolicy(kRestricted + QList<GeoZone::Restriction>{ GeoZone::USpace }).whileActive();
}

GeoZonePolicy GeoZonePolicy::mission(const QDateTime &start, const QDateTime &end)
{
    return GeoZonePolicy(kRestricted + QList<GeoZone::Restriction>{ GeoZone::USpace, GeoZone::NoRestriction }).activeDuring(start, end);
}

GeoZonePolicy GeoZonePolicy::routing(const QDateTime &start, const QDateTime &end)
{
    return GeoZonePolicy(kRestricted).activeDuring(start, end);
}

GeoZonePolicy GeoZonePolicy::profile(const QDateTime &start, const QDateTime &end)
{
    return GeoZonePolicy(kRestricted + QList<GeoZone::Restriction>{ GeoZone::USpace }).activeDuring(start, end);
}

bool GeoZonePolicy::isRestricted(GeoZone::Restriction restriction)
{
    return kRestricted.contains(restriction);
}

GeoZonePolicy GeoZonePolicy::whileActive() const
{
    GeoZonePolicy policy = anyTime();
    policy._schedule = Schedule::Current;
    return policy;
}

GeoZonePolicy GeoZonePolicy::activeDuring(const QDateTime &start, const QDateTime &end) const
{
    GeoZonePolicy policy = anyTime();
    if (start.isValid()) {
        policy._schedule = Schedule::Planned;
        policy._start = start;
        policy._end = end;
    }
    return policy;
}

GeoZonePolicy GeoZonePolicy::anyTime() const
{
    GeoZonePolicy policy;
    policy._restrictions = _restrictions;
    return policy;
}

bool GeoZonePolicy::acceptsSchedule(const GeoZone *zone) const
{
    switch (_schedule) {
    case Schedule::Any:
        return true;
    case Schedule::Current:
        return zone->active();
    case Schedule::Planned:
        return _end.isValid() ? zone->isActiveDuring(_start, _end) : zone->isActiveAt(_start);
    }
    return true;
}
