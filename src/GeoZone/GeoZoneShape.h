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

#include <utility>

#include "GeoZone.h"

/// Bounds in normalized Web Mercator coordinates: x and y in [0, 1], y increasing southwards. Shapes crossing the
/// antimeridian extend past 0 or 1 in x.
struct GeoZoneMercatorBounds {
    double minX = 0;
    double minY = 0;
    double maxX = 0;
    double maxY = 0;

    /// x wraps around, one unit being 360 degrees of longitude
    bool intersects(const GeoZoneMercatorBounds &other) const {
        if ((minY > other.maxY) || (maxY < other.minY)) {
            return false;
        }
        for (const double shift : { -1.0, 0.0, 1.0 }) {
            if ((minX <= (other.maxX + shift)) && (maxX >= (other.minX + shift))) {
                return true;
            }
        }
        return false;
    }
    double width() const { return maxX - minX; }
    double height() const { return maxY - minY; }
};

/// One polygon (with holes) or circle of a GeoZone prepared for map display. Polygons provide Web Mercator copies
/// simplified for each zoom level, computed on first use.
class GeoZoneShape
{
public:
    /// @param rings Of a polygon volume, shared with the zone
    GeoZoneShape(GeoZone *zone, const QList<GeoZone::Ring> &rings);
    GeoZoneShape(GeoZone *zone, const QGeoCoordinate &center, double radius);

    GeoZone *zone() const { return _zone; }
    bool isCircle() const { return _isCircle; }
    QGeoCoordinate center() const { return _center; }
    double radius() const { return _radius; }
    const GeoZoneMercatorBounds &mercatorBounds() const { return _mercatorBounds; }

    /// @return Polygon rings in mercator coordinates, the outer boundary first, simplified for the zoom level so that
    ///         the outline moves by at most half a pixel. Perimeters smaller than that become a triangle spanning the
    ///         shape, and holes are dropped. Not simplified from fullDetailZoomLevel up. Empty for circles.
    QList<QList<QPointF>> mercatorRings(int zoomLevel) const;

    static constexpr int fullDetailZoomLevel = 17;

    static QPointF toMercator(const QGeoCoordinate &coordinate);
    /// @param lonLat x is longitude and y latitude in degrees
    static QPointF toMercator(const QPointF &lonLat);
    static QList<QPointF> toMercator(const GeoZone::Ring &ring);
    static QGeoCoordinate fromMercator(const QPointF &point);

    /// Size in pixels of one normalized mercator unit at the zoom level, for 256 pixel map tiles
    static double mercatorPixels(double zoomLevel);

    /// Douglas-Peucker simplification of a closed mercator ring (first vertex not repeated).
    /// @return Subset of the ring vertices, or fewer than 3 vertices if the ring collapses.
    static QList<QPointF> simplifyRing(const QList<QPointF> &ring, double mercatorTolerance);

    /// @return The three ring vertices spanning the largest triangle found by a farthest point search
    static QList<QPointF> spanningTriangle(const QList<QPointF> &ring);

private:
    GeoZone *_zone = nullptr;
    bool _isCircle = false;
    QList<GeoZone::Ring> _rings;
    QGeoCoordinate _center;
    double _radius = 0;
    GeoZoneMercatorBounds _mercatorBounds;
    /// By simplified zoom level, a list since few levels are used and a hash allocates much more
    mutable QList<std::pair<int, QList<QList<QPointF>>>> _mercatorCache;
};
