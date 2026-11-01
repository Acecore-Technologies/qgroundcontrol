/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneGreatCircle.h"
#include "GeoZonePredicates.h"
#include "GeoZoneProjection.h"

#include <cmath>

namespace GeoZoneGeometry {

namespace {

constexpr double kDegreesToRadians = M_PI / 180.0;
constexpr double kMetersPerDegree = kEarthRadiusMeters * kDegreesToRadians;

struct Vector3 {
    double x = 0;
    double y = 0;
    double z = 0;
};

/// @return Unit vector of the point, x longitude and y latitude in degrees
Vector3 _toVector(const QPointF &lonLat)
{
    const double latitude = lonLat.y() * kDegreesToRadians;
    const double longitude = lonLat.x() * kDegreesToRadians;
    return { std::cos(latitude) * std::cos(longitude), std::cos(latitude) * std::sin(longitude), std::sin(latitude) };
}

/// Fast upper bound of greatCircleDeviation for short edges. In latitude and longitude great circles curve away from
/// a straight edge by up to its length squared times the tangent of the latitude over 4 earth radii, half that for
/// east-west edges. This is twice that, at the higher latitude of the edge.
double _greatCircleDeviationBound(const QPointF &from, const QPointF &to)
{
    // Edges shorter than about 150 m skip the tangent, 573 is the tangent of 89.9 degrees
    constexpr double shortSquared = 2 * kEarthRadiusMeters / 573.0;
    const double north = (to.y() - from.y()) * kMetersPerDegree;
    const double east = std::remainder(to.x() - from.x(), 360.0) * kMetersPerDegree * std::cos(from.y() * kDegreesToRadians);
    const double lengthSquared = (north * north) + (east * east);
    if (lengthSquared < shortSquared) {
        return lengthSquared / shortSquared;
    }
    const double latitude = (std::min)((std::max)(std::abs(from.y()), std::abs(to.y())), 89.9);
    return lengthSquared * std::tan(latitude * kDegreesToRadians) / (2 * kEarthRadiusMeters);
}

/// Adds the points between from and to, exclusive, to the ring while it has fewer than maxVertices
/// @param to Continuous from from, the shorter way
void _densifyEdge(const QPointF &from, const QPointF &to, double tolerance, int depth, qsizetype maxVertices, QList<QPointF> &ring)
{
    if ((depth <= 0) || (ring.count() >= maxVertices) || (greatCircleDeviation(from, to) <= tolerance)) {
        return;
    }
    const QPointF middle = greatCircleInterpolate(from, to, 0.5);
    _densifyEdge(from, middle, tolerance, depth - 1, maxVertices, ring);
    if (ring.count() < maxVertices) {
        ring.append(QPointF(std::remainder(middle.x(), 360.0), middle.y()));
    }
    _densifyEdge(middle, to, tolerance, depth - 1, maxVertices, ring);
}

/// Adds the fractions between first and last, exclusive, of the great circle from one point to another, halving it
/// until the straight edges are within the tolerance
void _greatCircleFractions(const QPointF &from, const QPointF &to, double first, double last, double tolerance, int depth, QList<double> &fractions,
                           bool &exceeded)
{
    const QPointF firstPoint = (first <= 0) ? from : greatCircleInterpolate(from, to, first);
    const QPointF lastPoint = (last >= 1) ? to : greatCircleInterpolate(from, to, last);
    if ((_greatCircleDeviationBound(firstPoint, lastPoint) <= tolerance) || (greatCircleDeviation(firstPoint, lastPoint) <= tolerance)) {
        return;
    }
    if (depth <= 0) {
        exceeded = true;
        return;
    }
    const double middle = (first + last) / 2;
    _greatCircleFractions(from, to, first, middle, tolerance, depth - 1, fractions, exceeded);
    fractions.append(middle);
    _greatCircleFractions(from, to, middle, last, tolerance, depth - 1, fractions, exceeded);
}

} // namespace

QPointF greatCircleInterpolate(const QPointF &from, const QPointF &to, double fraction)
{
    const double longitudeDelta = std::remainder(to.x() - from.x(), 360.0);
    const QPointF straight(from.x() + (longitudeDelta * fraction), from.y() + ((to.y() - from.y()) * fraction));
    const Vector3 a = _toVector(from);
    const Vector3 b = _toVector(to);
    const Vector3 normal{ (a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x) };
    const double sine = std::sqrt((normal.x * normal.x) + (normal.y * normal.y) + (normal.z * normal.z));
    // The same or opposite points have no single great circle
    if (sine < 1e-12) {
        return straight;
    }
    const double angle = std::atan2(sine, (a.x * b.x) + (a.y * b.y) + (a.z * b.z));
    const double weightA = std::sin((1 - fraction) * angle) / sine;
    const double weightB = std::sin(fraction * angle) / sine;
    const Vector3 point{ (weightA * a.x) + (weightB * b.x), (weightA * a.y) + (weightB * b.y), (weightA * a.z) + (weightB * b.z) };
    const double longitude = std::atan2(point.y, point.x) / kDegreesToRadians;
    const double latitude = std::atan2(point.z, std::hypot(point.x, point.y)) / kDegreesToRadians;
    return QPointF(longitude + (360.0 * std::round((straight.x() - longitude) / 360.0)), latitude);
}

double greatCircleDeviation(const QPointF &from, const QPointF &to)
{
    double deviation = 0;
    for (const double fraction : { 0.25, 0.5, 0.75 }) {
        // Measured around the point, where the projection keeps distances. It is affine in latitude and longitude, so
        // the edge stays straight in it.
        const QPointF point = greatCircleInterpolate(from, to, fraction);
        const LocalProjection projection(QGeoCoordinate(point.y(), std::remainder(point.x(), 360.0)));
        deviation = (std::max)(deviation, distanceToSegment(QPointF(), projection.lonLatToLocal(from), projection.lonLatToLocal(to)));
    }
    return deviation;
}

QList<double> greatCircleFractions(const QPointF &from, const QPointF &to, double toleranceMeters, bool *exceeded)
{
    QList<double> fractions = { 0 };
    bool farther = false;
    _greatCircleFractions(from, to, 0, 1, toleranceMeters, kMaxGreatCircleSubdivisions, fractions, farther);
    fractions.append(1);
    if (exceeded) {
        *exceeded = farther;
    }
    return fractions;
}

QList<QPointF> densifyGreatCircles(const QList<QPointF> &ring, qsizetype maxVertices, const QList<bool> &greatCircleEdges, double toleranceMeters)
{
    const qsizetype count = ring.count();
    QList<QPointF> result;
    qsizetype copied = 0;
    for (qsizetype i = 0; (count > 1) && (i < count); i++) {
        const QPointF &from = ring[i];
        const QPointF &next = ring[(i + 1) % count];
        const QPointF to(from.x() + std::remainder(next.x() - from.x(), 360.0), next.y());
        if (!greatCircleEdges.value(i, greatCircleEdges.isEmpty()) || (_greatCircleDeviationBound(from, to) <= toleranceMeters)) {
            continue;
        }
        result.append(ring.mid(copied, (i + 1) - copied));
        copied = i + 1;
        // Room is kept for the vertices still to copy
        _densifyEdge(from, to, toleranceMeters, kMaxGreatCircleSubdivisions, maxVertices - (count - copied), result);
    }
    if (copied == 0) {
        return ring;
    }
    result.append(ring.mid(copied));
    return result;
}

} // namespace GeoZoneGeometry
