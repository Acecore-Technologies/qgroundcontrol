/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>

#include "GeoZone.h"

/// Level the alerts about a vehicle on the ground inside a zone, the geo-awareness status and the permits are meant to
/// have (EN 4709-003 Annex D): WARNING (red), CAUTION (amber) or ADVISORY (neutral). Their severity orders them among
/// the zone alerts, like the restriction of the zone does.
class GeoZoneIntendedLevel
{
    Q_GADGET

public:
    enum Level {
        Advisory,
        Caution,
        Warning,
    };
    Q_ENUM(Level)

    /// @return Severity of the alerts of the level, see GeoZone::restrictionSeverity
    static int severity(Level level)
    {
        switch (level) {
        case Warning:
            return GeoZone::restrictionSeverity(GeoZone::Prohibited);
        case Caution:
            return GeoZone::restrictionSeverity(GeoZone::ReqAuthorisation);
        case Advisory:
            break;
        }
        return GeoZone::restrictionSeverity(GeoZone::NoRestriction);
    }
};
