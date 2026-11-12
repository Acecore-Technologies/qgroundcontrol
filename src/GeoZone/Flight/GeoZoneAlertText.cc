/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAlertText.h"
#include "GeoZoneAwareness.h"

#include <cmath>

QString GeoZoneAlertText::message(const AlertEvent &event, const QString &zoneName, const QString &restrictionText)
{
    // Alerts about a zone the pilot acknowledged a permit for, see GeoZonePermits
    const bool permitted = (event.circumstance == AlertEvent::Circumstance::Permit);
    switch (event.kind) {
    case AlertEvent::Entry:
        return permitted ? GeoZoneMonitor::tr("Entered geozone %1 (%2) \u2014 permit acknowledged").arg(zoneName, restrictionText) : GeoZoneMonitor::tr("Entered geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::OnGround: {
        if (permitted) {
            return GeoZoneMonitor::tr("On the ground inside geozone %1 (%2) \u2014 permit acknowledged").arg(zoneName, restrictionText);
        }
        const bool arming = (event.circumstance == AlertEvent::Circumstance::Arming);
        switch (event.restriction) {
        case GeoZone::Prohibited:
            return arming ? GeoZoneMonitor::tr("Arming inside prohibited geozone %1 — take-off not allowed here").arg(zoneName) :
                            GeoZoneMonitor::tr("On the ground inside prohibited geozone %1 — take-off not allowed here").arg(zoneName);
        case GeoZone::ReqAuthorisation:
            return arming ? GeoZoneMonitor::tr("Arming inside geozone %1 (%2) — take-off only with an authorisation").arg(zoneName, restrictionText) :
                            GeoZoneMonitor::tr("On the ground inside geozone %1 (%2) — take-off only with an authorisation").arg(zoneName, restrictionText);
        case GeoZone::Conditional:
        case GeoZone::Unknown:
            return arming ? GeoZoneMonitor::tr("Arming inside geozone %1 (%2) — check its conditions before take-off").arg(zoneName, restrictionText) :
                            GeoZoneMonitor::tr("On the ground inside geozone %1 (%2) — check its conditions before take-off").arg(zoneName, restrictionText);
        case GeoZone::NoRestriction:
        case GeoZone::USpace:
            break;
        }
        return arming ? GeoZoneMonitor::tr("Arming inside geozone %1 (%2)").arg(zoneName, restrictionText) : GeoZoneMonitor::tr("On the ground inside geozone %1 (%2)").arg(zoneName, restrictionText);
    }
    case AlertEvent::PositionLost:
        return event.inside ? GeoZoneMonitor::tr("Position lost inside geozone %1 (%2)").arg(zoneName, restrictionText) :
                              GeoZoneMonitor::tr("Position lost near geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Traffic:
        return GeoZoneMonitor::tr("Traffic %1 entered geozone %2 (%3)").arg(event.traffic, zoneName, restrictionText);
    case AlertEvent::NonConformance:
        return zoneName.isEmpty() ? GeoZoneMonitor::tr("Outside planned volume") : GeoZoneMonitor::tr("Outside planned volume: %1").arg(zoneName);
    case AlertEvent::Awareness:
        return GeoZoneAwareness::alertMessage(GeoZoneAwareness::stateFromCode(event.zoneIdentifier), event.zoneName);
    case AlertEvent::FenceWarning:
        return GeoZoneMonitor::tr("Geofence ahead: %1").arg(zoneName);
    case AlertEvent::FenceBreach:
        return zoneName;
    case AlertEvent::Activated:
        return GeoZoneMonitor::tr("Geozone %1 (%2) is now active \u2014 you are inside it. Leave or land as soon as possible.").arg(zoneName, restrictionText);
    case AlertEvent::ActivationSoon: {
        // Minutes rounded up, so the zone never applies before the time said
        const qint64 secs = std::isnan(event.timeToEntry) ? 0 : static_cast<qint64>(std::ceil(event.timeToEntry));
        const QString left = (secs >= 60) ? GeoZoneMonitor::tr("%n min", nullptr, static_cast<int>((secs + 59) / 60)) : GeoZoneMonitor::tr("%n s", nullptr, static_cast<int>(secs));
        return event.inside ? GeoZoneMonitor::tr("Geozone %1 (%2) becomes active in %3, you are inside it").arg(zoneName, restrictionText, left) :
                              GeoZoneMonitor::tr("Geozone %1 (%2) nearby becomes active in %3").arg(zoneName, restrictionText, left);
    }
    case AlertEvent::Notified:
        return GeoZoneMonitor::tr("Pilot notified about geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Approved:
        return GeoZoneMonitor::tr("Pilot confirmed the permission to fly in geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Dismissed:
        return GeoZoneMonitor::tr("Pilot dismissed the alert about geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Muted:
        return GeoZoneMonitor::tr("Pilot muted the alerts about geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Permit:
        return GeoZoneMonitor::tr("Pilot acknowledged the permit to enter geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::PermitEnded:
        return GeoZoneMonitor::tr("Permit to enter geozone %1 (%2) ended").arg(zoneName, restrictionText);
    case AlertEvent::SourceOff:
        return GeoZoneMonitor::tr("Geozone source %1 turned off, its zones are no longer checked").arg(zoneName);
    case AlertEvent::SourceOn:
        return GeoZoneMonitor::tr("Geozone source %1 turned on").arg(zoneName);
    case AlertEvent::Authorised:
        return GeoZoneMonitor::tr("Authorisation recorded for geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::AuthorisationRemoved:
        return GeoZoneMonitor::tr("Authorisation removed for geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Approach:
        break;
    }
    if (permitted) {
        return GeoZoneMonitor::tr("Approaching geozone %1 (%2) \u2014 permit acknowledged").arg(zoneName, restrictionText);
    }
    if (!std::isnan(event.timeToEntry)) {
        return GeoZoneMonitor::tr("Approaching geozone %1 (%2), entry in about %3 s").arg(zoneName, restrictionText).arg(std::lround(event.timeToEntry));
    }
    return GeoZoneMonitor::tr("Approaching geozone %1 (%2)").arg(zoneName, restrictionText);
}
