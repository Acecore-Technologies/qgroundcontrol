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

/// Great circles (geodesics) of geozone edges and of the legs of paths, see GeoZoneGeometry.h
namespace GeoZoneGeometry {

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
