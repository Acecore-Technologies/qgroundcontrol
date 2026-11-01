/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneProjection.h"

#include <cmath>

namespace GeoZoneGeometry {

namespace {

constexpr double kDegreesToRadians = M_PI / 180.0;

} // namespace

LocalProjection::LocalProjection(const QGeoCoordinate &origin)
    : _originLatitude(origin.latitude())
    , _originLongitude(origin.longitude())
    , _metersPerDegreeLatitude(kEarthRadiusMeters * kDegreesToRadians)
    , _metersPerDegreeLongitude(kEarthRadiusMeters * kDegreesToRadians * std::cos(origin.latitude() * kDegreesToRadians))
{
}

QPointF LocalProjection::toLocal(const QGeoCoordinate &coordinate) const
{
    return lonLatToLocal(QPointF(coordinate.longitude(), coordinate.latitude()));
}

QPointF LocalProjection::lonLatToLocal(const QPointF &lonLat) const
{
    double longitudeDelta = lonLat.x() - _originLongitude;
    if (longitudeDelta > 180) {
        longitudeDelta -= 360;
    } else if (longitudeDelta < -180) {
        longitudeDelta += 360;
    }
    return QPointF(longitudeDelta * _metersPerDegreeLongitude, (lonLat.y() - _originLatitude) * _metersPerDegreeLatitude);
}

QPointF LocalProjection::lonLatToLocal(const QPointF &lonLat, const QPointF &previous) const
{
    const double previousDelta = (_metersPerDegreeLongitude != 0) ? (previous.x() / _metersPerDegreeLongitude) : 0;
    double longitudeDelta = lonLat.x() - _originLongitude;
    // Whole turns, so that unwrapped longitudes are projected exactly like single points
    longitudeDelta += 360.0 * std::round((previousDelta - longitudeDelta) / 360.0);
    return QPointF(longitudeDelta * _metersPerDegreeLongitude, (lonLat.y() - _originLatitude) * _metersPerDegreeLatitude);
}

QList<QPointF> LocalProjection::lonLatToLocal(const QList<QPointF> &lonLats) const
{
    QList<QPointF> points;
    points.reserve(lonLats.count());
    for (const QPointF &lonLat : lonLats) {
        points.append(points.isEmpty() ? lonLatToLocal(lonLat) : lonLatToLocal(lonLat, points.constLast()));
    }
    return points;
}

double LocalProjection::eastScaleAt(double latitude) const
{
    const double originCos = std::cos(_originLatitude * kDegreesToRadians);
    return (originCos > 1e-9) ? (std::cos(latitude * kDegreesToRadians) / originCos) : 1.0;
}

QPointF LocalProjection::toLocal(const QGeoCoordinate &coordinate, const QPointF &previous) const
{
    return lonLatToLocal(QPointF(coordinate.longitude(), coordinate.latitude()), previous);
}

QList<QPointF> LocalProjection::toLocal(const QList<QGeoCoordinate> &coordinates) const
{
    QList<QPointF> points;
    points.reserve(coordinates.count());
    for (const QGeoCoordinate &coordinate : coordinates) {
        points.append(points.isEmpty() ? toLocal(coordinate) : toLocal(coordinate, points.constLast()));
    }
    return points;
}

QGeoCoordinate LocalProjection::toGeo(const QPointF &point) const
{
    double longitude = _originLongitude + (point.x() / _metersPerDegreeLongitude);
    if (longitude > 180) {
        longitude -= 360;
    } else if (longitude < -180) {
        longitude += 360;
    }
    return QGeoCoordinate(_originLatitude + (point.y() / _metersPerDegreeLatitude), longitude);
}

} // namespace GeoZoneGeometry
