/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAreaOfInterest.h"
#include "GeoZoneService.h"

#include <QtCore/QtMath>

#include <algorithm>
#include <cmath>

bool GeoZoneAreaOfInterest::setMissionArea(const void *owner, const QGeoRectangle &area)
{
    if (!area.isValid()) {
        (void) _missionAreas.remove(owner);
        return false;
    }
    if (_missionAreas.value(owner) == area) {
        return false;
    }
    _missionAreas.insert(owner, area);
    return true;
}

bool GeoZoneAreaOfInterest::setVehiclePosition(const QGeoCoordinate &position)
{
    if (position.isValid() && (std::abs(position.latitude()) <= 90) && (std::abs(position.longitude()) <= 180) &&
        !((position.latitude() == 0) && (position.longitude() == 0))) {
        _vehiclePosition = QGeoCoordinate(position.latitude(), position.longitude());
        return true;
    }
    _vehiclePosition = QGeoCoordinate();
    return false;
}

QGeoRectangle GeoZoneAreaOfInterest::area() const
{
    QList<QGeoCoordinate> coordinates;
    for (const QGeoRectangle &area : _missionAreas) {
        coordinates << area.topLeft() << area.bottomRight();
    }
    if (_vehiclePosition.isValid()) {
        coordinates.append(_vehiclePosition);
    }
    return coordinates.isEmpty() ? QGeoRectangle() : QGeoRectangle(coordinates);
}

QGeoRectangle GeoZoneAreaOfInterest::requestArea(const QGeoRectangle &downloaded, double maxAreaKm2) const
{
    const QGeoRectangle interest = area();
    if (!interest.isValid() || (downloaded.isValid() && (!_follow || downloaded.contains(interest)))) {
        return downloaded;
    }
    // Services answering areas up to a size get a smaller margin, a little under their limit
    const QGeoRectangle full = expanded(interest, margin);
    if ((maxAreaKm2 <= 0) || (GeoZoneService::areaKm2(full) <= (0.95 * maxAreaKm2))) {
        return full;
    }
    double low = 0;
    double high = margin;
    for (int i = 0; i < 20; i++) {
        const double candidate = (low + high) / 2;
        if (GeoZoneService::areaKm2(expanded(interest, candidate)) <= (0.95 * maxAreaKm2)) {
            low = candidate;
        } else {
            high = candidate;
        }
    }
    return expanded(interest, low);
}

QGeoRectangle GeoZoneAreaOfInterest::expanded(const QGeoRectangle &area, double meters)
{
    if (!area.isValid()) {
        return QGeoRectangle();
    }
    constexpr double metersPerDegree = 111320.0;
    const double latitudeMargin = meters / metersPerDegree;
    const double maxLatitude = (std::max)(std::abs(area.topLeft().latitude()), std::abs(area.bottomRight().latitude()));
    const double longitudeMargin = meters / (metersPerDegree * (std::max)(0.01, std::cos(qDegreesToRadians((std::min)(maxLatitude + latitudeMargin, 89.0)))));
    const double north = (std::min)(90.0, area.topLeft().latitude() + latitudeMargin);
    const double south = (std::max)(-90.0, area.bottomRight().latitude() - latitudeMargin);
    double west = area.topLeft().longitude() - longitudeMargin;
    double east = area.bottomRight().longitude() + longitudeMargin;
    if ((area.width() + (2 * longitudeMargin)) >= 360) {
        west = -180;
        east = 180;
    } else {
        // Areas reaching across the antimeridian continue on the other side
        west = (west < -180) ? (west + 360) : west;
        east = (east > 180) ? (east - 360) : east;
    }
    return QGeoRectangle(QGeoCoordinate(north, west), QGeoCoordinate(south, east));
}
