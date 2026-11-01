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

#include <functional>

/// Ground elevations from the terrain tiles of QGroundControl, for limits above ground along paths whose terrain
/// isn't known otherwise. Lookups don't wait: they use the tiles already loaded and start loading missing ones.
namespace GeoZoneTerrain {

using Lookup = std::function<double(const QGeoCoordinate &coordinate)>;

constexpr double defaultSpacing = 100;  ///< Meters between the positions added along paths
constexpr int maxPositions = 100;       ///< Positions of a path after adding the terrain, the spacing grows for long paths

/// @return Ground elevation in meters AMSL at the coordinate, NaN when its terrain tile isn't loaded
double elevationAt(const QGeoCoordinate &coordinate);

/// Replaces the terrain tiles, for tests. An empty lookup restores them.
void setLookup(const Lookup &lookup);

/// @return The path with the terrain elevation at the positions without one, or the elevation of the previous position
///         when its tile isn't loaded, and with positions added between them where the terrain is known, so that
///         limits above ground follow the terrain. Altitudes of the added positions are interpolated.
QList<GeoZone::Position> withTerrain(const QList<GeoZone::Position> &path, double spacing = defaultSpacing);

} // namespace GeoZoneTerrain
