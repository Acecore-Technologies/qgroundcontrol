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

#include "GeoZoneMonitor.h"

/// The messages of the flight alerts, as shown to the pilot, spoken and read back from the alert log. They are
/// translated in the context of GeoZoneMonitor, which they were part of.
class GeoZoneAlertText
{
public:
    using AlertEvent = GeoZoneMonitor::AlertEvent;

    /// @return Message of the alert about the zone, without the vehicle, like "Entered geozone P (Prohibited)", or of
    ///         what the pilot was shown or answered about it
    /// @param zoneName Name of the zone, or what stands for it: the boundary of a fence alert, the reasons of a
    ///                 geo-awareness alert, the source of a source switch
    static QString message(const AlertEvent &event, const QString &zoneName, const QString &restrictionText);
};
