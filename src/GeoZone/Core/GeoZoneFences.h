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
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>

class GeoZone;

/// Exclusion fences of zones, as GeoZoneFenceImporter adds them to a GeoFence, and whether fences cover zones
namespace GeoZoneFences {

constexpr double boundaryTolerance = 1;     ///< Meters zones may extend past fences covering them, for rounding

/// Exclusion fences of a zone
struct ZoneFences {
    QList<QList<QGeoCoordinate>> polygons;
    QList<QGeoCircle> circles;

    bool isEmpty() const { return polygons.isEmpty() && circles.isEmpty(); }
};

/// @return The exclusion fences importing the zone adds: the outer boundaries of its polygon volumes, simplified
///         outward when the tolerance is positive, see GeoZone::exclusionPolygons, and its circles
ZoneFences zoneFences(const GeoZone *zone, double simplifyTolerance = 0);

/// @return true if each of the fences importing the zone without simplification adds lies within one of the fences,
///         up to boundaryTolerance, so that the fences exclude the whole zone. Fences importing it with any
///         simplification add enclose those. False for zones importing adds no fence for.
bool fencesCover(const ZoneFences &fences, const GeoZone *zone);

} // namespace GeoZoneFences
