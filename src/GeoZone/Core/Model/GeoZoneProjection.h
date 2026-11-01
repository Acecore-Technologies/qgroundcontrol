/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QPointF>
#include <QtPositioning/QGeoCoordinate>

/// Local projection of the geozone geometry, see GeoZoneGeometry.h
namespace GeoZoneGeometry {

/// Mean radius of the spherical earth of the projection and the great circles, in meters
inline constexpr double kEarthRadiusMeters = 6371008.8;

/// Equirectangular projection to meters around an origin. It is affine in latitude and longitude, so edges
/// which are straight in latitude/longitude stay straight and containment tests are exact. Distances are
/// accurate near the origin.
/// Single points are projected within 180 degrees of longitude of the origin. Paths and rings continue from their
/// previous point, so that edges take the shorter way across the antimeridian and polygons spanning more than 180
/// degrees of longitude stay continuous.
class LocalProjection
{
public:
    explicit LocalProjection(const QGeoCoordinate &origin);

    QPointF toLocal(const QGeoCoordinate &coordinate) const;
    /// @return The point of the coordinate within 180 degrees of longitude of the previous point
    QPointF toLocal(const QGeoCoordinate &coordinate, const QPointF &previous) const;
    /// @return Points of the path or ring, each continuing from the previous one
    QList<QPointF> toLocal(const QList<QGeoCoordinate> &coordinates) const;
    /// @param lonLat x is longitude and y latitude in degrees
    QPointF lonLatToLocal(const QPointF &lonLat) const;
    QPointF lonLatToLocal(const QPointF &lonLat, const QPointF &previous) const;
    QList<QPointF> lonLatToLocal(const QList<QPointF> &lonLats) const;
    QGeoCoordinate toGeo(const QPointF &point) const;
    QGeoCoordinate origin() const { return QGeoCoordinate(_originLatitude, _originLongitude); }
    /// @return Factor converting east distances of this projection to those of a projection around the latitude
    double eastScaleAt(double latitude) const;

private:
    double _originLatitude = 0;
    double _originLongitude = 0;
    double _metersPerDegreeLatitude = 0;
    double _metersPerDegreeLongitude = 0;
};

} // namespace GeoZoneGeometry
