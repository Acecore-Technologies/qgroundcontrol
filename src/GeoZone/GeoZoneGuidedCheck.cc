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
#include "GeoZoneMonitor.h"
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
    QList<GeoZone*> zones;
    GeoZoneManager *const manager = _monitor->manager();
    for (const GeoZoneCatalog::PathHit &hit : manager->catalog().pathHits(path, GeoZonePolicy::alerts().excludingAuthorised(manager->authorisations()))) {
        // Zones the vehicle is already in were alerted when it entered them
        if ((hit.entry.segment > 0) || (hit.entry.fraction > 0)) {
            zones.append(hit.zone);
        }
    }
    if (zones.isEmpty()) {
        return QString();
    }
    std::stable_sort(zones.begin(), zones.end(), [](const GeoZone *a, const GeoZone *b) {
        return GeoZone::restrictionSeverity(a->restriction()) < GeoZone::restrictionSeverity(b->restriction());
    });

    constexpr qsizetype maxListed = 4;
    QStringList names;
    for (qsizetype i = 0; i < (std::min)(zones.count(), maxListed); i++) {
        names.append(QStringLiteral("%1 (%2)").arg(zones[i]->displayName(), zones[i]->restrictionText()));
    }
    QString text = names.join(QStringLiteral(", "));
    if (zones.count() > maxListed) {
        text += tr(" and %1 more").arg(zones.count() - maxListed);
    }
    return tr("The vehicle would enter geozones: %1").arg(text);
}

QString GeoZoneGuidedCheck::gotoWarning(QObject *vehicle, const QGeoCoordinate &coordinate, double altitudeAMSL) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid() || !coordinate.isValid()) {
        return QString();
    }
    const GeoZone::Position from = _monitor->vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    GeoZone::Position to{ coordinate, std::isnan(altitudeAMSL) ? from.altitudeAMSL : altitudeAMSL };
    return pathWarning(GeoZoneTerrain::withTerrain({ from, to }));
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
    return pathWarning(GeoZoneTerrain::withTerrain(path));
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
    GeoZone::Position to = from;
    to.altitudeAMSL = from.altitudeAMSL + (relativeAltitude - currentRelative);
    return pathWarning({ from, to });
}
