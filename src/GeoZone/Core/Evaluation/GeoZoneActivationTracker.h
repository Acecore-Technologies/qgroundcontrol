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
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QString>

#include "GeoZone.h"
#include "GeoZoneAlertTracker.h"

class GeoZoneCatalog;
class GeoZonePolicy;

/// Zones around one vehicle which aren't active now but become active soon, so that the pilot is cautioned before a
/// time-limited zone applies where the vehicle is. Each activation of a zone alerts once, while the vehicle is inside
/// the zone or within the approach distances of GeoZoneAlertTracker::Limits.
class GeoZoneActivationTracker
{
public:
    static constexpr int recheckSecs = 60;  ///< A zone found inactive for the whole caution time is checked again after this

    struct Alert {
        const GeoZone *zone = nullptr;
        bool inside = false;
        double distance = 0;    ///< Meters to the zone, 0 inside
        QDateTime activation;   ///< UTC, when the zone becomes active
    };

    /// @param policy Zones the vehicle is alerted about whatever their schedule, like GeoZonePolicy::alerts().anyTime()
    /// @param now UTC
    /// @param cautionSecs How long before their activation zones alert
    /// @return Zones near or around the position which become active within the caution time and weren't alerted for
    ///         that activation yet, the soonest first
    QList<Alert> update(const GeoZoneCatalog &catalog, const GeoZonePolicy &policy, const GeoZone::Position &position, const GeoZoneAlertTracker::Limits &limits,
                        const QDateTime &now, int cautionSecs);

    /// Forgets the activations alerted, so that they alert again
    void reset();

    /// @return When the zone becomes active after now within the seconds, to the second, invalid when it doesn't or is
    ///         active now
    static QDateTime nextActivation(const GeoZone *zone, const QDateTime &now, int secs);

private:
    struct Pending {
        const GeoZone *zone = nullptr;
        QDateTime checked;      ///< When the activation was looked for
        QDateTime activation;   ///< Invalid when none was found within the caution time
        int cautionSecs = 0;
    };

    QHash<QString, Pending> _pending;       ///< Zones near the vehicle by GeoZone::key
    QHash<QString, QDateTime> _alerted;     ///< Activations alerted by GeoZone::key
};
