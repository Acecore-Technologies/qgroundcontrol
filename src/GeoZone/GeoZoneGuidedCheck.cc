/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneGuidedCheck.h"
#include "GeoZoneManager.h"
#include "GeoZonePlanning.h"
#include "GeoZoneMonitor.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZoneTerrain.h"
#include "Vehicle.h"

#include <algorithm>
#include <cmath>

GeoZoneGuidedCheck::GeoZoneGuidedCheck(GeoZoneMonitor *monitor)
    : QObject(monitor)
    , _monitor(monitor)
{
}

QString GeoZoneGuidedCheck::pathWarning(const QList<GeoZone::Position> &path) const
{
    return pathWarning(GeoZonePathEvaluator::Path::fromPositions(path));
}

QString GeoZoneGuidedCheck::pathWarning(const GeoZonePathEvaluator::Path &path) const
{
    GeoZoneManager *const manager = _monitor->manager();
    GeoZonePathEvaluator evaluator(manager->catalog(), GeoZonePolicy::alerts().excludingAuthorised(manager->planning()->authorisations()),
                                   QDateTime::currentDateTimeUtc());
    evaluator.setTerrainLookup(GeoZoneTerrain::elevationAt);
    QList<const GeoZone*> zones;
    for (const GeoZonePathEvaluator::ZoneResult &result : evaluator.evaluate({ path }).zones) {
        // Zones the vehicle is already in were alerted when it entered them, it may enter them again after leaving them
        const bool enters = std::any_of(result.crossings.cbegin(), result.crossings.cend(), [](const GeoZonePathEvaluator::Crossing &crossing) {
            return crossing.active && ((crossing.entry.segment > 0) || (crossing.entry.fraction > 0));
        });
        if (enters) {
            zones.append(result.zone);
        }
    }
    if (zones.isEmpty()) {
        return QString();
    }
    GeoZonePathEvaluator::sortBySeverity(zones);
    return tr("The vehicle would enter geozones: %1").arg(GeoZonePathEvaluator::zoneNames(zones, maxListedZones, true));
}

QString GeoZoneGuidedCheck::gotoWarning(QObject *vehicle, const QGeoCoordinate &coordinate, double altitudeAMSL) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid() || !coordinate.isValid()) {
        return QString();
    }
    const GeoZone::Position from = _monitor->vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    const GeoZone::Position to{ coordinate, std::isnan(altitudeAMSL) ? from.altitudeAMSL : altitudeAMSL };
    return pathWarning({ from, to });
}

QString GeoZoneGuidedCheck::orbitWarning(QObject *vehicle, const QGeoCoordinate &center, double radius, double altitudeAMSL) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid() || !center.isValid() || !(radius > 0)) {
        return QString();
    }
    const GeoZone::Position from = _monitor->vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    QList<GeoZone::Position> path = { from };
    constexpr int circlePoints = 36;
    const double startAzimuth = center.azimuthTo(from.coordinate);
    for (int i = 0; i <= circlePoints; i++) {
        path.append(GeoZone::Position{ center.atDistanceAndAzimuth(radius, startAzimuth + (360.0 * i / circlePoints)),
                                       std::isnan(altitudeAMSL) ? from.altitudeAMSL : altitudeAMSL });
    }
    return pathWarning(path);
}

QString GeoZoneGuidedCheck::altitudeWarning(QObject *vehicle, double relativeAltitude) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid() || std::isnan(relativeAltitude)) {
        return QString();
    }
    const GeoZone::Position from = _monitor->vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    const double currentRelative = guidedVehicle->altitudeRelative()->rawValue().toDouble();
    if (std::isnan(from.altitudeAMSL) || std::isnan(currentRelative)) {
        // Unknown altitudes are inside all limits, so only zones applying at any altitude would be named
        return pathWarning({ from, from });
    }
    // Climbing or descending in place relative to the home altitude
    GeoZonePathEvaluator::Path path;
    path.altitudeReference = GeoZonePathEvaluator::AltitudeReference::Relative;
    path.referenceAltitudeAMSL = from.altitudeAMSL - currentRelative;
    path.vertices = { GeoZonePathEvaluator::Vertex{ from.coordinate, currentRelative, from.terrainAMSL, QDateTime() },
                      GeoZonePathEvaluator::Vertex{ from.coordinate, relativeAltitude, from.terrainAMSL, QDateTime() } };
    return pathWarning(path);
}
