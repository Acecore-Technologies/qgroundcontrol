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

bool GeoZoneAreaOfInterest::setVehiclePosition(const QGeoCoordinate &position, double groundSpeed, double course)
{
    if (position.isValid() && (std::abs(position.latitude()) <= 90) && (std::abs(position.longitude()) <= 180) &&
        !((position.latitude() == 0) && (position.longitude() == 0))) {
        _vehiclePosition = QGeoCoordinate(position.latitude(), position.longitude());
        // An implausible speed, like one estimated from a position jump, must not request a country
        _vehicleSpeed = std::isfinite(groundSpeed) ? std::clamp(groundSpeed, 0.0, maxSpeed) : 0;
        _vehicleCourse = std::isfinite(course) ? course : qQNaN();
        return true;
    }
    _vehiclePosition = QGeoCoordinate();
    _vehicleSpeed = 0;
    _vehicleCourse = qQNaN();
    return false;
}

double GeoZoneAreaOfInterest::lookAheadMeters() const
{
    return (std::max)(minLookAheadMeters, lookAheadSecs * _vehicleSpeed);
}

bool GeoZoneAreaOfInterest::setViewArea(const QGeoRectangle &area)
{
    const QGeoRectangle view = cropped(area, maxViewMeters);
    if (view == _viewArea) {
        return false;
    }
    _viewArea = view;
    return true;
}

bool GeoZoneAreaOfInterest::setFollowView(bool follow)
{
    if (follow == _followView) {
        return false;
    }
    _followView = follow;
    return _viewArea.isValid();
}

QGeoRectangle GeoZoneAreaOfInterest::_missions() const
{
    QList<QGeoCoordinate> coordinates;
    for (const QGeoRectangle &area : _missionAreas) {
        coordinates << area.topLeft() << area.bottomRight();
    }
    return coordinates.isEmpty() ? QGeoRectangle() : QGeoRectangle(coordinates);
}

QGeoRectangle GeoZoneAreaOfInterest::missionsAndVehicle() const
{
    const QGeoRectangle missions = _missions();
    if (!_vehiclePosition.isValid()) {
        return missions;
    }
    const QGeoRectangle vehicle(_vehiclePosition, _vehiclePosition);
    return missions.isValid() ? missions.united(vehicle) : vehicle;
}

QGeoRectangle GeoZoneAreaOfInterest::_vehicleArea(double scale) const
{
    if (!_vehiclePosition.isValid()) {
        return QGeoRectangle();
    }
    QList<QGeoCoordinate> coordinates = { _vehiclePosition };
    if (!std::isnan(_vehicleCourse) && (_vehicleSpeed > 0)) {
        coordinates.append(_vehiclePosition.atDistanceAndAzimuth(scale * lookAheadSecs * _vehicleSpeed, _vehicleCourse));
    }
    return expanded(QGeoRectangle(coordinates), scale * lookAheadMeters());
}

GeoZoneAreaOfInterest::Plan GeoZoneAreaOfInterest::_plan(double maxAreaKm2) const
{
    // A little under the limit of the service
    const double limitKm2 = 0.95 * maxAreaKm2;
    const auto fits = [limitKm2](const QGeoRectangle &area) { return (limitKm2 <= 0) || (GeoZoneService::areaKm2(area) <= limitKm2); };
    const auto united = [](const QGeoRectangle &a, const QGeoRectangle &b) { return !a.isValid() ? b : (b.isValid() ? a.united(b) : a); };
    // The margin and the look-ahead of the vehicle shrink together by the scale, around the fixed areas
    const auto interestAt = [this, &united](const QGeoRectangle &fixed, double scale) { return united(fixed, _vehicleArea(scale)); };
    const auto planAt = [&interestAt](const QGeoRectangle &fixed, double scale) {
        Plan plan;
        plan.interest = interestAt(fixed, scale);
        plan.request = expanded(plan.interest, scale * margin);
        return plan;
    };
    // The largest scale at which the request fits the service, negative when the fixed areas don't
    const auto largestScale = [&planAt, &fits](const QGeoRectangle &fixed) {
        if (fits(planAt(fixed, 1).request)) {
            return 1.0;
        }
        if (!fits(planAt(fixed, 0).request)) {
            return -1.0;
        }
        double low = 0;
        double high = 1;
        for (int i = 0; i < 24; i++) {
            const double candidate = (low + high) / 2;
            if (fits(planAt(fixed, candidate).request)) {
                low = candidate;
            } else {
                high = candidate;
            }
        }
        return low;
    };

    const QGeoRectangle missions = _missions();
    const QGeoRectangle view = _followView ? _viewArea : QGeoRectangle();
    const bool vehicle = _vehiclePosition.isValid();
    if (!missions.isValid() && !vehicle) {
        if (!view.isValid()) {
            return Plan();
        }
        // Services answering smaller areas get the center of the view
        const QGeoRectangle fixed = fits(view) ? view : cropped(view, std::sqrt(limitKm2) * 1000);
        return planAt(fixed, (std::max)(0.0, largestScale(fixed)));
    }

    // Without vehicle the areas don't move, any margin will do. With one, a margin and a look-ahead too small would
    // download its surroundings again every few meters.
    const double minScale = vehicle ? minVehicleScale : 0;
    if (view.isValid()) {
        const QGeoRectangle fixed = united(missions, view);
        if ((spanMeters(interestAt(fixed, 1)) <= maxViewMeters) && fits(interestAt(fixed, 1))) {
            if (const double scale = largestScale(fixed); scale >= minScale) {
                return planAt(fixed, scale);
            }
        }
    }
    if (const double scale = largestScale(missions); scale >= minScale) {
        return planAt(missions, scale);
    }

    // The service can't answer the missions with the vehicle: the zones around the vehicle come first
    Plan plan;
    if (vehicle) {
        plan = planAt(QGeoRectangle(), (std::max)(0.0, largestScale(QGeoRectangle())));
    } else {
        plan = planAt(cropped(missions, std::sqrt(limitKm2) * 1000), 0);
    }
    plan.missionsCut = missions.isValid();
    return plan;
}

QGeoRectangle GeoZoneAreaOfInterest::interest(double maxAreaKm2) const
{
    return _plan(maxAreaKm2).interest;
}

bool GeoZoneAreaOfInterest::missionsCut(double maxAreaKm2) const
{
    return _plan(maxAreaKm2).missionsCut;
}

QGeoRectangle GeoZoneAreaOfInterest::requestArea(const QGeoRectangle &downloaded, double maxAreaKm2) const
{
    const Plan plan = _plan(maxAreaKm2);
    if (!plan.interest.isValid() || (downloaded.isValid() && (!_follow || downloaded.contains(plan.interest)))) {
        return downloaded;
    }
    return plan.request;
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

QGeoRectangle GeoZoneAreaOfInterest::cropped(const QGeoRectangle &area, double maxMeters)
{
    if (!area.isValid()) {
        return QGeoRectangle();
    }
    constexpr double metersPerDegree = 111320.0;
    const QGeoCoordinate center = area.center();
    const double maxHeight = maxMeters / metersPerDegree;
    const double maxWidth = maxMeters / (metersPerDegree * (std::max)(0.01, std::cos(qDegreesToRadians(center.latitude()))));
    if ((area.height() <= maxHeight) && (area.width() <= maxWidth)) {
        return area;
    }
    return QGeoRectangle(center, (std::min)(area.width(), maxWidth), (std::min)(area.height(), maxHeight));
}

double GeoZoneAreaOfInterest::spanMeters(const QGeoRectangle &area)
{
    if (!area.isValid()) {
        return 0;
    }
    constexpr double metersPerDegree = 111320.0;
    const double width = area.width() * metersPerDegree * (std::max)(0.01, std::cos(qDegreesToRadians(area.center().latitude())));
    return (std::max)(area.height() * metersPerDegree, width);
}
