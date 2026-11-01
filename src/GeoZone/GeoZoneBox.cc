/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneBox.h"
#include "GeoZoneGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

/// Enough to compare boxes and longitudes with continuous longitudes from -180 up to 540 degrees
constexpr double kLongitudeShifts[] = { -360.0, 0.0, 360.0 };

} // namespace

GeoZoneBox::GeoZoneBox(double south, double west, double north, double east)
{
    if (!std::isfinite(south) || !std::isfinite(west) || !std::isfinite(north) || !std::isfinite(east) || (south > north)) {
        return;
    }
    _south = south;
    _north = north;
    if (west > east) {
        east += 360;
    }
    if ((east - west) >= 360) {
        _west = -180;
        _east = 180;
        return;
    }
    const double shift = 360.0 * std::floor((west + 180.0) / 360.0);
    _west = west - shift;
    _east = east - shift;
}

GeoZoneBox::GeoZoneBox(const QGeoRectangle &rectangle)
    : GeoZoneBox(rectangle.isValid() ? GeoZoneBox(rectangle.bottomRight().latitude(), rectangle.topLeft().longitude(), rectangle.topLeft().latitude(), rectangle.bottomRight().longitude())
                                     : GeoZoneBox())
{
}

GeoZoneBox GeoZoneBox::fromPath(const QList<QGeoCoordinate> &path)
{
    QList<QPointF> lonLats;
    lonLats.reserve(path.count());
    for (const QGeoCoordinate &coordinate : path) {
        if (coordinate.isValid()) {
            lonLats.append(QPointF(coordinate.longitude(), coordinate.latitude()));
        }
    }
    return fromLonLatPath(lonLats);
}

GeoZoneBox GeoZoneBox::fromLonLatPath(const QList<QPointF> &path)
{
    double south = std::numeric_limits<double>::max();
    double north = std::numeric_limits<double>::lowest();
    double west = std::numeric_limits<double>::max();
    double east = std::numeric_limits<double>::lowest();
    double previous = qQNaN();
    for (const QPointF &point : path) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            continue;
        }
        double longitude = point.x();
        if (!qIsNaN(previous)) {
            longitude += 360.0 * std::round((previous - longitude) / 360.0);
        }
        previous = longitude;
        south = (std::min)(south, point.y());
        north = (std::max)(north, point.y());
        west = (std::min)(west, longitude);
        east = (std::max)(east, longitude);
    }
    if (qIsNaN(previous)) {
        return GeoZoneBox();
    }
    return GeoZoneBox(south, west, north, east);
}

std::pair<double, double> GeoZoneBox::longitudeSpan(double longitude1, double longitude2)
{
    const double west = (std::min)(longitude1, longitude2);
    const double east = (std::max)(longitude1, longitude2);
    return ((east - west) > 180) ? std::make_pair(east, west) : std::make_pair(west, east);
}

QGeoCoordinate GeoZoneBox::center() const
{
    if (!isValid()) {
        return QGeoCoordinate();
    }
    return QGeoCoordinate((_south + _north) / 2, std::remainder((_west + _east) / 2, 360.0));
}

bool GeoZoneBox::contains(const QGeoCoordinate &coordinate) const
{
    if (!isValid() || !coordinate.isValid() || (coordinate.latitude() < _south) || (coordinate.latitude() > _north)) {
        return false;
    }
    return longitudeDistance(coordinate.longitude()) == 0;
}

bool GeoZoneBox::intersects(const GeoZoneBox &other) const
{
    if (!isValid() || !other.isValid() || (_south > other._north) || (other._south > _north)) {
        return false;
    }
    return std::any_of(std::cbegin(kLongitudeShifts), std::cend(kLongitudeShifts), [this, &other](double shift) {
        return (_west <= (other._east + shift)) && ((other._west + shift) <= _east);
    });
}

GeoZoneBox GeoZoneBox::united(const GeoZoneBox &other) const
{
    if (!isValid()) {
        return other;
    }
    if (!other.isValid()) {
        return *this;
    }
    double west = 0;
    double east = std::numeric_limits<double>::max();
    for (const double shift : kLongitudeShifts) {
        const double unitedWest = (std::min)(_west, other._west + shift);
        const double unitedEast = (std::max)(_east, other._east + shift);
        if ((unitedEast - unitedWest) < (east - west)) {
            west = unitedWest;
            east = unitedEast;
        }
    }
    return GeoZoneBox((std::min)(_south, other._south), west, (std::max)(_north, other._north), east);
}

double GeoZoneBox::longitudeDistance(double longitude) const
{
    if (!isValid() || !std::isfinite(longitude)) {
        return qInf();
    }
    longitude = std::remainder(longitude, 360.0);
    double distance = qInf();
    for (const double shift : kLongitudeShifts) {
        const double shifted = longitude + shift;
        if ((shifted >= _west) && (shifted <= _east)) {
            return 0;
        }
        distance = (std::min)({ distance, std::abs(shifted - _west), std::abs(shifted - _east) });
    }
    return distance;
}

double GeoZoneBox::distanceTo(const QGeoCoordinate &coordinate) const
{
    if (!isValid() || !coordinate.isValid()) {
        return qInf();
    }
    const double latitude = coordinate.latitude();
    const double latitudeDistance = (std::max)({ _south - latitude, latitude - _north, 0.0 });
    // At most 180 degrees of longitude, so the offset isn't wrapped around the world
    const GeoZoneGeometry::LocalProjection projection(coordinate);
    const QPointF offset = projection.lonLatToLocal(QPointF(coordinate.longitude() + longitudeDistance(coordinate.longitude()), latitude + latitudeDistance));
    return std::hypot(offset.x(), offset.y());
}

QGeoRectangle GeoZoneBox::toRectangle() const
{
    if (!isValid()) {
        return QGeoRectangle();
    }
    if (width() >= 360) {
        return QGeoRectangle(QGeoCoordinate(_north, -180), QGeoCoordinate(_south, 180));
    }
    return QGeoRectangle(QGeoCoordinate(_north, _west), QGeoCoordinate(_south, std::remainder(_east, 360.0)));
}
