/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QPointF>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoPolygon>

class GeoZone;

/// Bounds in normalized Web Mercator coordinates: x and y in [0, 1], y increasing southwards
struct GeoZoneMercatorBounds {
    double minX = 0;
    double minY = 0;
    double maxX = 0;
    double maxY = 0;

    bool intersects(const GeoZoneMercatorBounds &other) const {
        return (minX <= other.maxX) && (maxX >= other.minX) && (minY <= other.maxY) && (maxY >= other.minY);
    }
    double width() const { return maxX - minX; }
    double height() const { return maxY - minY; }
};

/// One polygon (with holes) or circle of a GeoZone prepared for map display. Polygons provide copies
/// simplified for each zoom level, computed on first use.
class GeoZoneShape
{
public:
    GeoZoneShape(GeoZone *zone, const QList<QList<QGeoCoordinate>> &rings);
    GeoZoneShape(GeoZone *zone, const QGeoCoordinate &center, double radius);

    GeoZone *zone() const { return _zone; }
    bool isCircle() const { return _isCircle; }
    QGeoCoordinate center() const { return _center; }
    double radius() const { return _radius; }
    const GeoZoneMercatorBounds &mercatorBounds() const { return _mercatorBounds; }

    /// @return Polygon including holes, simplified for the zoom level so that the outline moves by at most
    ///         half a pixel. Perimeters smaller than that become a triangle spanning the shape, and holes
    ///         are dropped. Not simplified from fullDetailZoomLevel up.
    QGeoPolygon polygon(int zoomLevel) const;

    static constexpr int fullDetailZoomLevel = 17;

    static QPointF toMercator(const QGeoCoordinate &coordinate);

    /// Size in pixels of one normalized mercator unit at the zoom level, for 256 pixel map tiles
    static double mercatorPixels(double zoomLevel);

    /// Douglas-Peucker simplification of a closed ring (first vertex not repeated) in mercator space.
    /// @return Subset of the ring vertices, or fewer than 3 vertices if the ring collapses.
    static QList<QGeoCoordinate> simplifyRing(const QList<QGeoCoordinate> &ring, double mercatorTolerance);

    /// @return The three ring vertices spanning the largest triangle found by a farthest point search
    static QList<QGeoCoordinate> spanningTriangle(const QList<QGeoCoordinate> &ring);

private:
    GeoZone *_zone = nullptr;
    bool _isCircle = false;
    QList<QList<QGeoCoordinate>> _rings;
    QGeoCoordinate _center;
    double _radius = 0;
    GeoZoneMercatorBounds _mercatorBounds;
    mutable QHash<int, QGeoPolygon> _polygonCache;
};
