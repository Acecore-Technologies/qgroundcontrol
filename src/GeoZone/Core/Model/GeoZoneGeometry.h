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

/// Largest distance in meters between the great circle (geodesic) a polygon edge stands for and the edge straight in
/// latitude and longitude, which the queries use, before densifyGreatCircles adds points to the edge. Also the largest
/// distance between a published curve and the edges which replace it, like the arcs of AIXM.
inline constexpr double kGreatCircleToleranceMeters = 1.0;
/// Times densifyGreatCircles halves an edge at most, adding up to 2^10 - 1 points to it
inline constexpr int kMaxGreatCircleSubdivisions = 10;

/// @return The point at the fraction of the great circle from one point to another, x longitude and y latitude in
///         degrees. Its longitude continues from the start's the shorter way, so it may be beyond 180 degrees.
QPointF greatCircleInterpolate(const QPointF &from, const QPointF &to, double fraction);

/// @return Largest distance in meters between the great circle between the points and the edge straight in latitude
///         and longitude the shorter way around, sampled at a quarter, half and three quarters of the great circle
double greatCircleDeviation(const QPointF &from, const QPointF &to);

/// Largest distance in meters between the great circle a vehicle flies between two positions of a path and the pieces
/// straight in latitude and longitude which the checks use for it, see greatCircleFractions
inline constexpr double kPathGreatCircleToleranceMeters = 1.0;

/// @return Fractions of the great circle from one point to another, x longitude and y latitude in degrees, from 0 to 1
///         ascending, such that the edges straight in latitude and longitude between the points of the great circle
///         at consecutive fractions are within the tolerance of it. Just 0 and 1 when the straight edge already is.
///         The great circle is halved up to kMaxGreatCircleSubdivisions times.
/// @param exceeded Set when a piece is still farther than the tolerance from the great circle
QList<double> greatCircleFractions(const QPointF &from, const QPointF &to, double toleranceMeters = kPathGreatCircleToleranceMeters,
                                   bool *exceeded = nullptr);

/// Adds points on the great circles between the vertices of the unclosed ring, x longitude and y latitude in degrees,
/// for formats whose edges are geodesics rather than straight in latitude and longitude, where an edge deviates from
/// its great circle by more than the tolerance. Such edges are halved until each part is within the tolerance or up to
/// kMaxGreatCircleSubdivisions times. Edges are taken the shorter way around. Short edges stay as they are, and the
/// ring doesn't grow beyond maxVertices. Added longitudes are between -180 and 180 degrees.
/// @param greatCircleEdges Whether each edge, from a vertex to the next and from the last to the first, is a great
///                         circle. All edges are when empty.
/// @return The ring, shared when no point is added
QList<QPointF> densifyGreatCircles(const QList<QPointF> &ring, qsizetype maxVertices, const QList<bool> &greatCircleEdges = {},
                                   double toleranceMeters = kGreatCircleToleranceMeters);

} // namespace GeoZoneGeometry
