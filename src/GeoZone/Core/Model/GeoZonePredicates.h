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
#include <QtCore/qnamespace.h>
#include <QtPositioning/QGeoCoordinate>

#include <optional>
#include <utility>

/// Containment, intersection, distance and interval tests of the geozone geometry, on points of a local projection,
/// see GeoZoneGeometry.h
namespace GeoZoneGeometry {

/// Points closer than this to an edge of a ring, in the units of the points, are on its boundary
inline constexpr double kBoundaryTolerance = 1e-6;

/// @return true if the point is inside the ring or on its boundary
/// @param ring     Unclosed polygon ring
/// @param fillRule Qt::WindingFill counts every area the ring winds around as inside, also those a ring crossing itself
///                 encloses twice, Qt::OddEvenFill only those it encloses an odd number of times
bool ringContains(const QList<QPointF> &ring, const QPointF &point, Qt::FillRule fillRule = Qt::WindingFill);

/// @return true if the point is inside the outer ring, rings[0], with the fill rule, and not inside a hole, or on a
///         boundary. Holes only exclude what they enclose an odd number of times, so that what a ring crossing itself
///         encloses is conservatively inside the polygon.
bool polygonContains(const QList<QList<QPointF>> &rings, const QPointF &point, Qt::FillRule fillRule = Qt::WindingFill);

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

/// @return Fractions of the segment from a to b inside the polygon, rings[0] less the holes, ordered by their start,
///         see polygonContains. The boundary is inside, so a segment touching it has an empty interval there.
QList<Interval> segmentPolygonIntervals(const QPointF &a, const QPointF &b, const QList<QList<QPointF>> &rings, Qt::FillRule fillRule = Qt::WindingFill);

/// @return Fractions of the segment from a to b within the radius of the origin, nothing when it stays outside
std::optional<Interval> segmentCircleInterval(const QPointF &a, const QPointF &b, double radius);

/// @return true if no two edges of the unclosed ring intersect other than adjacent edges at their shared vertex
bool isSimple(const QList<QPointF> &ring);

} // namespace GeoZoneGeometry
