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
#include <QtPositioning/QGeoCoordinate>

/// Simplification of geozone polygons which keeps them inside, see GeoZoneGeometry.h
namespace GeoZoneGeometry {

/// Reduces the vertex count of a polygon ring while keeping the whole original polygon inside the result.
/// The ring is simplified with the tolerance, then each edge is moved outward past the vertices it replaced,
/// or by the tolerance when that is not enough, so the result extends up to twice the tolerance beyond the
/// original boundary.
/// @param ring Unclosed polygon ring
/// @return The simplified ring, or the original ring if it can't be reduced safely
QList<QGeoCoordinate> simplifyOutward(const QList<QGeoCoordinate> &ring, double toleranceMeters);

} // namespace GeoZoneGeometry
