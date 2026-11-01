/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePolicy.h"
#include "GeoZoneAuthorisations.h"

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
    GeoZonePolicy policy = *this;
    policy._schedule = Schedule::Any;
    policy._start = QDateTime();
    policy._end = QDateTime();
    return policy;
}

GeoZonePolicy GeoZonePolicy::excludingAuthorised(const GeoZoneAuthorisations *authorisations) const
{
    GeoZonePolicy policy = *this;
    policy._authorisations = authorisations;
    return policy;
}

bool GeoZonePolicy::authorised(const GeoZone *zone) const
{
    const GeoZoneAuthorisation *const authorisation = _authorisations ? _authorisations->find(zone) : nullptr;
    if (!authorisation) {
        return false;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    switch (_schedule) {
    case Schedule::Any:
        return !authorisation->expired(now);
    case Schedule::Current:
        return authorisation->covers(now, now);
    case Schedule::Planned:
        return authorisation->covers(_start, _end.isValid() ? _end : _start);
    }
    return false;
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
