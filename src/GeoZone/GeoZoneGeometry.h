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

#include <optional>
#include <utility>

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
    /// @param lonLat x is longitude and y latitude in degrees
    QPointF lonLatToLocal(const QPointF &lonLat) const;
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

/// @param ring Unclosed polygon ring
bool ringContains(const QList<QPointF> &ring, const QPointF &point);

/// @return true if the point is inside the outer ring, rings[0], and outside the holes
bool polygonContains(const QList<QList<QPointF>> &rings, const QPointF &point);

/// @return Area of the unclosed ring, positive when counterclockwise
double signedArea(const QList<QPointF> &ring);
/// @return Signed area of the ring in square degrees, with longitude as x, positive when counterclockwise
double signedArea(const QList<QGeoCoordinate> &ring);

bool segmentsIntersect(const QPointF &a1, const QPointF &a2, const QPointF &b1, const QPointF &b2);

double distanceToSegment(const QPointF &point, const QPointF &a, const QPointF &b);

/// @return Distance to the closest edge of the unclosed ring
/// @param xScale Applied to x offsets from the point, like LocalProjection::eastScaleAt
double distanceToRing(const QPointF &point, const QList<QPointF> &ring, double xScale = 1.0);

/// @return true if a segment intersects an edge of the unclosed ring
bool segmentIntersectsRing(const QPointF &a, const QPointF &b, const QList<QPointF> &ring);

/// Range of fractions along a segment, 0 at its start and 1 at its end
using Interval = std::pair<double, double>;

/// @return Fractions of the segment from a to b inside the polygon, rings[0] less the holes, ordered by their start.
///         The boundary is inside, so a segment touching it has an empty interval there.
QList<Interval> segmentPolygonIntervals(const QPointF &a, const QPointF &b, const QList<QList<QPointF>> &rings);

/// @return Fractions of the segment from a to b within the radius of the origin, nothing when it stays outside
std::optional<Interval> segmentCircleInterval(const QPointF &a, const QPointF &b, double radius);

/// @return true if no two edges of the unclosed ring intersect other than adjacent edges at their shared vertex
bool isSimple(const QList<QPointF> &ring);

/// Reduces the vertex count of a polygon ring while keeping the whole original polygon inside the result.
/// The ring is simplified with the tolerance, then each edge is moved outward past the vertices it replaced,
/// or by the tolerance when that is not enough, so the result extends up to twice the tolerance beyond the
/// original boundary.
/// @param ring Unclosed polygon ring
/// @return The simplified ring, or the original ring if it can't be reduced safely
QList<QGeoCoordinate> simplifyOutward(const QList<QGeoCoordinate> &ring, double toleranceMeters);

/// @return Height of the EGM96 geoid, approximately mean sea level, above the WGS84 ellipsoid in meters.
///         NaN for an invalid coordinate.
double geoidHeight(const QGeoCoordinate &coordinate);

} // namespace GeoZoneGeometry
