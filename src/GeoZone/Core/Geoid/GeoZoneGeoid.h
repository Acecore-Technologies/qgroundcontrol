/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtPositioning/QGeoCoordinate>

/// Geoid heights for limits above the WGS84 ellipsoid, see GeoZoneGeometry.h
namespace GeoZoneGeometry {

/// @return Height of the EGM96 geoid, approximately mean sea level, above the WGS84 ellipsoid in meters.
///         NaN for an invalid coordinate.
double geoidHeight(const QGeoCoordinate &coordinate);

} // namespace GeoZoneGeometry
