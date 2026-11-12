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

#include "GeoZonePredicates.h"

/// Simplification of geozone polygons, see GeoZoneGeometry.h
namespace GeoZoneGeometry {

/// How far a vertex is from the chord replacing it: distanceToSegment, or distanceToSegmentSquared which is cheaper
using ChainMeasure = double (*)(const QPointF &point, const QPointF &a, const QPointF &b);

/// The Douglas-Peucker step of the simplifications, on the part of the closed ring from the vertex first to the vertex
/// last, which may exceed the vertex count to wrap around. Marks the vertices to keep between them, those farther
/// than the tolerance from the chord between the kept vertices around them.
/// @param tolerance    In the measure: squared for distanceToSegmentSquared
/// @param keep         A flag for each vertex of the ring
/// @param keepFarthest Keep the vertex farthest from the chord from first to last even when within the tolerance
void simplifyChain(const QList<QPointF> &ring, qsizetype first, qsizetype last, double tolerance, QList<bool> &keep, bool keepFarthest = false,
                   ChainMeasure measure = distanceToSegment);

/// Reduces the vertex count of a polygon ring while keeping the whole original polygon inside the result.
/// The ring is simplified with the tolerance, then each edge is moved outward past the vertices it replaced,
/// or by the tolerance when that is not enough, so the result extends up to twice the tolerance beyond the
/// original boundary.
/// @param ring Unclosed polygon ring
/// @return The simplified ring, or the original ring if it can't be reduced safely
QList<QGeoCoordinate> simplifyOutward(const QList<QGeoCoordinate> &ring, double toleranceMeters);

} // namespace GeoZoneGeometry
