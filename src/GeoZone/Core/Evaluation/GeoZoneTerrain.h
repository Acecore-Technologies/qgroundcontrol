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

#include "GeoZone.h"

/// Ground elevations from GeoZoneEnvironment::terrainElevation, the terrain tiles of QGroundControl in the application,
/// for limits above ground along paths whose terrain isn't known otherwise. Lookups don't wait: they use the tiles
/// already loaded and start loading missing ones.
namespace GeoZoneTerrain {

constexpr double defaultSpacing = 100;  ///< Meters between the positions added along paths
constexpr int maxPositions = 1000;      ///< Positions of a path after adding the terrain, the spacing grows for longer paths

/// @return Ground elevation in meters AMSL at the coordinate, NaN when its terrain tile isn't loaded
double elevationAt(const QGeoCoordinate &coordinate);

/// @return The path with the terrain elevation at the positions without one, and with positions added between them,
///         so that limits above ground follow the terrain. Altitudes of the added positions are interpolated. Where the
///         tile isn't loaded the terrain is NaN, so limits above ground don't bound the zones there.
QList<GeoZone::Position> withTerrain(const QList<GeoZone::Position> &path, double spacing = defaultSpacing);

} // namespace GeoZoneTerrain
