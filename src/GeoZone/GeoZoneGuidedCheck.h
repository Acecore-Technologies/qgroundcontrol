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
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"
#include "GeoZonePathEvaluator.h"

class GeoZoneMonitor;

/// Checks of the guided commands of the Fly View, each returning a warning naming the zones accepted by
/// GeoZonePolicy::alerts and active now which the vehicle would enter, or an empty string. Zones authorised now and
/// zones the vehicle is in are left out, unless it would leave and enter them again. See GeoZonePathEvaluator.
/// Limits above ground use the terrain along the path from GeoZoneTerrain where it is loaded, and the terrain below
/// the vehicle elsewhere, see GeoZoneMonitor::vehiclePosition.
class GeoZoneGuidedCheck : public QObject
{
    Q_OBJECT

public:
    explicit GeoZoneGuidedCheck(GeoZoneMonitor *monitor);

    /// @param vehicle Vehicle object, the checks return an empty string without a position
    /// @param altitudeAMSL Target altitude in meters above mean sea level, NaN to keep the vehicle's altitude
    Q_INVOKABLE QString gotoWarning(QObject *vehicle, const QGeoCoordinate &coordinate, double altitudeAMSL = qQNaN()) const;
    /// Flying to and around the circle
    Q_INVOKABLE QString orbitWarning(QObject *vehicle, const QGeoCoordinate &center, double radius, double altitudeAMSL) const;
    /// Climbing or descending at the vehicle's position, also for takeoffs
    /// @param relativeAltitude Target altitude in meters above the vehicle's home
    Q_INVOKABLE QString altitudeWarning(QObject *vehicle, double relativeAltitude) const;

    static constexpr int maxListedZones = 4;

    /// @return Warning naming the zones the path enters after its start, empty without
    QString pathWarning(const QList<GeoZone::Position> &path) const;
    QString pathWarning(const GeoZonePathEvaluator::Path &path) const;

private:
    GeoZoneMonitor *_monitor = nullptr;
};
