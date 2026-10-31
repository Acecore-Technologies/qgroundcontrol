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

/// Planar geometry helpers for geozone and fence polygons
namespace GeoZoneGeometry {

/// Equirectangular projection to meters around an origin. It is affine in latitude and longitude, so edges
/// which are straight in latitude/longitude stay straight and containment tests are exact. Distances are
/// accurate near the origin.
class LocalProjection
{
public:
    explicit LocalProjection(const QGeoCoordinate &origin);

    QPointF toLocal(const QGeoCoordinate &coordinate) const;
    QList<QPointF> toLocal(const QList<QGeoCoordinate> &coordinates) const;
    QGeoCoordinate toGeo(const QPointF &point) const;

private:
    double _originLatitude = 0;
    double _originLongitude = 0;
    double _metersPerDegreeLatitude = 0;
    double _metersPerDegreeLongitude = 0;
};

/// @param ring Unclosed polygon ring
bool ringContains(const QList<QPointF> &ring, const QPointF &point);

bool segmentsIntersect(const QPointF &a1, const QPointF &a2, const QPointF &b1, const QPointF &b2);

double distanceToSegment(const QPointF &point, const QPointF &a, const QPointF &b);

/// @return Distance to the closest edge of the unclosed ring
double distanceToRing(const QPointF &point, const QList<QPointF> &ring);

/// @return true if a segment intersects an edge of the unclosed ring
bool segmentIntersectsRing(const QPointF &a, const QPointF &b, const QList<QPointF> &ring);

/// @return true if no two edges of the unclosed ring intersect other than adjacent edges at their shared vertex
bool isSimple(const QList<QPointF> &ring);

/// Reduces the vertex count of a polygon ring while keeping the whole original polygon inside the result.
/// The ring is simplified with the tolerance, then each edge is moved outward past the vertices it replaced,
/// or by the tolerance when that is not enough, so the result extends up to twice the tolerance beyond the
/// original boundary.
/// @param ring Unclosed polygon ring
/// @return The simplified ring, or the original ring if it can't be reduced safely
QList<QGeoCoordinate> simplifyOutward(const QList<QGeoCoordinate> &ring, double toleranceMeters);

} // namespace GeoZoneGeometry
