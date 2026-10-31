/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneShape.h"

#include <QtCore/QtMath>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace {

constexpr double kMaxMercatorLatitude = 85.05112878;
constexpr double kEarthCircumferenceMeters = 40075016.686;
constexpr double kTilePixels = 256.0;

double _segmentDistanceSquared(const QPointF &point, const QPointF &a, const QPointF &b)
{
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    const double lengthSquared = (dx * dx) + (dy * dy);
    double t = 0;
    if (lengthSquared > 0) {
        t = std::clamp((((point.x() - a.x()) * dx) + ((point.y() - a.y()) * dy)) / lengthSquared, 0.0, 1.0);
    }
    const double px = a.x() + (t * dx) - point.x();
    const double py = a.y() + (t * dy) - point.y();
    return (px * px) + (py * py);
}

} // namespace

GeoZoneShape::GeoZoneShape(GeoZone *zone, const QList<QList<QGeoCoordinate>> &rings)
    : _zone(zone)
    , _rings(rings)
{
    bool first = true;
    for (const QGeoCoordinate &coordinate : _rings.value(0)) {
        const QPointF point = toMercator(coordinate);
        if (first) {
            _mercatorBounds = { point.x(), point.y(), point.x(), point.y() };
            first = false;
        } else {
            _mercatorBounds.minX = qMin(_mercatorBounds.minX, point.x());
            _mercatorBounds.minY = qMin(_mercatorBounds.minY, point.y());
            _mercatorBounds.maxX = qMax(_mercatorBounds.maxX, point.x());
            _mercatorBounds.maxY = qMax(_mercatorBounds.maxY, point.y());
        }
    }
}

GeoZoneShape::GeoZoneShape(GeoZone *zone, const QGeoCoordinate &center, double radius)
    : _zone(zone)
    , _isCircle(true)
    , _center(center)
    , _radius(radius)
{
    const QPointF point = toMercator(center);
    const double cosLatitude = qMax(0.01, qCos(qDegreesToRadians(center.latitude())));
    // Mercator scale grows with 1 / cos(latitude), equally in both directions
    const double mercatorRadius = radius / (kEarthCircumferenceMeters * cosLatitude);
    _mercatorBounds = { point.x() - mercatorRadius, point.y() - mercatorRadius, point.x() + mercatorRadius, point.y() + mercatorRadius };
}

QPointF GeoZoneShape::toMercator(const QGeoCoordinate &coordinate)
{
    const double latitude = qDegreesToRadians(std::clamp(coordinate.latitude(), -kMaxMercatorLatitude, kMaxMercatorLatitude));
    const double x = (coordinate.longitude() + 180.0) / 360.0;
    const double y = 0.5 - (std::log(std::tan((M_PI / 4.0) + (latitude / 2.0))) / (2.0 * M_PI));
    return QPointF(x, y);
}

double GeoZoneShape::mercatorPixels(double zoomLevel)
{
    return kTilePixels * std::exp2(zoomLevel);
}

QList<QGeoCoordinate> GeoZoneShape::simplifyRing(const QList<QGeoCoordinate> &ring, double mercatorTolerance)
{
    const qsizetype count = ring.count();
    if ((count <= 3) || (mercatorTolerance <= 0)) {
        return ring;
    }

    QList<QPointF> points;
    points.reserve(count);
    for (const QGeoCoordinate &coordinate : ring) {
        points.append(toMercator(coordinate));
    }

    // Split the closed ring at the first vertex and the vertex farthest from it
    qsizetype farthest = 0;
    double farthestDistance = -1;
    for (qsizetype i = 1; i < count; i++) {
        const double dx = points[i].x() - points[0].x();
        const double dy = points[i].y() - points[0].y();
        const double distance = (dx * dx) + (dy * dy);
        if (distance > farthestDistance) {
            farthestDistance = distance;
            farthest = i;
        }
    }

    QList<bool> keep(count, false);
    keep[0] = true;
    keep[farthest] = true;

    // Index count stands for vertex 0 closing the ring
    std::vector<std::pair<qsizetype, qsizetype>> segments = { { 0, farthest }, { farthest, count } };
    const double toleranceSquared = mercatorTolerance * mercatorTolerance;
    while (!segments.empty()) {
        const auto [first, last] = segments.back();
        segments.pop_back();

        const QPointF &a = points[first];
        const QPointF &b = points[last % count];
        qsizetype index = -1;
        double maxDistance = toleranceSquared;
        for (qsizetype i = first + 1; i < last; i++) {
            const double distance = _segmentDistanceSquared(points[i], a, b);
            if (distance > maxDistance) {
                maxDistance = distance;
                index = i;
            }
        }
        if (index >= 0) {
            keep[index] = true;
            segments.emplace_back(first, index);
            segments.emplace_back(index, last);
        }
    }

    QList<QGeoCoordinate> simplified;
    for (qsizetype i = 0; i < count; i++) {
        if (keep[i]) {
            simplified.append(ring[i]);
        }
    }
    return simplified;
}

QList<QGeoCoordinate> GeoZoneShape::spanningTriangle(const QList<QGeoCoordinate> &ring)
{
    if (ring.count() <= 3) {
        return ring;
    }

    QList<QPointF> points;
    points.reserve(ring.count());
    for (const QGeoCoordinate &coordinate : ring) {
        points.append(toMercator(coordinate));
    }

    qsizetype second = 1;
    double maxDistance = -1;
    for (qsizetype i = 1; i < points.count(); i++) {
        const QPointF offset = points[i] - points[0];
        const double distance = QPointF::dotProduct(offset, offset);
        if (distance > maxDistance) {
            maxDistance = distance;
            second = i;
        }
    }

    qsizetype third = (second == 1) ? 2 : 1;
    maxDistance = -1;
    for (qsizetype i = 1; i < points.count(); i++) {
        if (i == second) {
            continue;
        }
        const double distance = _segmentDistanceSquared(points[i], points[0], points[second]);
        if (distance > maxDistance) {
            maxDistance = distance;
            third = i;
        }
    }

    // Keep the published vertex order so the winding does not change
    QList<qsizetype> indexes = { 0, second, third };
    std::sort(indexes.begin(), indexes.end());
    return { ring[indexes[0]], ring[indexes[1]], ring[indexes[2]] };
}

QGeoPolygon GeoZoneShape::polygon(int zoomLevel) const
{
    if (_isCircle || _rings.isEmpty()) {
        return QGeoPolygon();
    }

    const int level = std::clamp(zoomLevel, 0, fullDetailZoomLevel);
    const auto cached = _polygonCache.constFind(level);
    if (cached != _polygonCache.constEnd()) {
        return cached.value();
    }

    QGeoPolygon polygon;
    if (level == fullDetailZoomLevel) {
        polygon.setPerimeter(_rings.first());
        for (qsizetype i = 1; i < _rings.count(); i++) {
            polygon.addHole(_rings[i]);
        }
    } else {
        const double tolerance = 0.5 / mercatorPixels(level);
        QList<QGeoCoordinate> perimeter = simplifyRing(_rings.first(), tolerance);
        if (perimeter.count() < 3) {
            // Shapes thinner than the tolerance still show as a small mark
            perimeter = spanningTriangle(_rings.first());
        }
        polygon.setPerimeter(perimeter);
        for (qsizetype i = 1; i < _rings.count(); i++) {
            const QList<QGeoCoordinate> hole = simplifyRing(_rings[i], tolerance);
            if (hole.count() >= 3) {
                polygon.addHole(hole);
            }
        }
    }

    _polygonCache.insert(level, polygon);
    return polygon;
}
