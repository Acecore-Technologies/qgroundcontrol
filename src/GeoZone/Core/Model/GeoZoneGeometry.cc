/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneGeometry.h"
#include "GeoZoneVector.h"

#include <QtCore/QFile>
#include <QtCore/QRectF>
#include <QtCore/QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>

namespace GeoZoneGeometry {

namespace {

constexpr double kEarthRadiusMeters = 6371008.8;
constexpr double kDegreesToRadians = M_PI / 180.0;
constexpr double kMetersPerDegree = kEarthRadiusMeters * kDegreesToRadians;

bool _onSegment(const QPointF &a, const QPointF &b, const QPointF &point)
{
    return (point.x() >= std::min(a.x(), b.x())) && (point.x() <= std::max(a.x(), b.x())) &&
           (point.y() >= std::min(a.y(), b.y())) && (point.y() <= std::max(a.y(), b.y()));
}

/// Intersection of the lines through p with direction d and through q with direction e
QPointF _lineIntersection(const QPointF &p, const QPointF &d, const QPointF &q, const QPointF &e)
{
    const double denominator = GeoZoneVector::cross(d, e);
    if (std::abs(denominator) < 1e-12) {
        return p;
    }
    return p + (d * (GeoZoneVector::cross(q - p, e) / denominator));
}

/// Douglas-Peucker on the part of the closed ring from first to last, which may exceed the vertex count to wrap
/// around. Marks the vertices to keep, excluding first and last.
/// @param keepFarthest Keep the vertex farthest from the chord even when within the tolerance
void _simplifyChain(const QList<QPointF> &ring, qsizetype first, qsizetype last, double tolerance, QList<bool> &keep, bool keepFarthest = false)
{
    const qsizetype count = ring.count();
    QList<std::pair<qsizetype, qsizetype>> stack = { { first, last } };
    while (!stack.isEmpty()) {
        const auto [chainFirst, chainLast] = stack.takeLast();
        double maxDistance = 0;
        qsizetype maxIndex = -1;
        for (qsizetype i = chainFirst + 1; i < chainLast; i++) {
            const double distance = distanceToSegment(ring[i % count], ring[chainFirst % count], ring[chainLast % count]);
            if (distance > maxDistance) {
                maxDistance = distance;
                maxIndex = i;
            }
        }
        if ((maxIndex >= 0) && ((maxDistance > tolerance) || keepFarthest)) {
            keepFarthest = false;
            keep[maxIndex % count] = true;
            stack.append({ chainFirst, maxIndex });
            stack.append({ maxIndex, chainLast });
        }
    }
}

QPointF _outwardNormal(const QPointF &direction, bool counterClockwise)
{
    return counterClockwise ? QPointF(direction.y(), -direction.x()) : QPointF(-direction.y(), direction.x());
}

/// How far to move each edge of the simplified ring outward so that the vertices it dropped are inside
struct Expansion {
    QList<double> offsets;      ///< offsets[i] is for the edge from kept vertex i to i + 1
    QList<double> capDistances; ///< Distance along the bisector at which to cut sharp corners
};

/// @param kept Indices of the ring vertices kept by the simplification
Expansion _expansion(const QList<QPointF> &ring, const QList<qsizetype> &kept, bool counterClockwise, double margin)
{
    const qsizetype count = ring.count();
    const qsizetype keptCount = kept.count();
    const auto wrap = [keptCount](qsizetype index) { return (index + keptCount) % keptCount; };
    // Calls function for the original vertices from kept[edge] to kept[edge + 1] inclusive
    const auto forChain = [&ring, &kept, count, keptCount, &wrap](qsizetype edge, const auto &function) {
        const qsizetype first = kept[wrap(edge)];
        const qsizetype last = (wrap(edge) + 1 < keptCount) ? kept[wrap(edge) + 1] : (kept.first() + count);
        for (qsizetype i = first; i <= last; i++) {
            function(ring[i % count]);
        }
    };

    QList<QPointF> directions;
    QList<QPointF> normals;
    for (qsizetype i = 0; i < keptCount; i++) {
        directions.append(GeoZoneVector::unit(ring[kept[wrap(i + 1)]] - ring[kept[i]]));
        normals.append(_outwardNormal(directions.last(), counterClockwise));
    }
    QList<bool> convex;
    for (qsizetype i = 0; i < keptCount; i++) {
        const double turn = GeoZoneVector::cross(directions[wrap(i - 1)], directions[i]);
        convex.append(counterClockwise ? (turn > 0) : (turn < 0));
    }

    Expansion expansion;
    for (qsizetype i = 0; i < keptCount; i++) {
        const QPointF &origin = ring[kept[i]];
        const QPointF &normal = normals[i];
        double deviation = 0;
        const auto measure = [&origin, &normal, &deviation](const QPointF &point) {
            deviation = std::max(deviation, QPointF::dotProduct(point - origin, normal));
        };
        forChain(i, measure);
        // Near a convex corner the expanded ring is bounded by both edges, so the vertices dropped by the
        // adjacent edge must be inside this edge too
        if (convex[i]) {
            forChain(i - 1, measure);
        }
        if (convex[wrap(i + 1)]) {
            forChain(i + 1, measure);
        }
        expansion.offsets.append(deviation + margin);
    }

    for (qsizetype i = 0; i < keptCount; i++) {
        const QPointF &vertex = ring[kept[i]];
        QPointF bisector = GeoZoneVector::unit(normals[wrap(i - 1)] + normals[i]);
        if (bisector.isNull()) {
            bisector = directions[wrap(i - 1)];
        }
        double distance = 0;
        const auto measure = [&vertex, &bisector, &distance](const QPointF &point) {
            distance = std::max(distance, QPointF::dotProduct(point - vertex, bisector));
        };
        forChain(i - 1, measure);
        forChain(i, measure);
        expansion.capDistances.append(std::max({ expansion.offsets[wrap(i - 1)], expansion.offsets[i], distance + margin }));
    }

    return expansion;
}

/// Moves each edge outward by its offset. Corners sharper than 60 degrees are cut at the cap distance to limit
/// spikes, and nearly parallel edges with different offsets are joined with a step.
/// @param owners Receives the index of the ring vertex each result vertex was created for
QList<QPointF> _offsetRing(const QList<QPointF> &ring, const Expansion &expansion, bool counterClockwise, QList<qsizetype> &owners)
{
    QList<QPointF> result;
    owners.clear();
    // Points closer than this are merged since nearly zero length edges make intersection tests unreliable
    constexpr double minSpacing = 0.001;
    const auto append = [&result, &owners](const QPointF &point, qsizetype owner) {
        if (result.isEmpty() || (GeoZoneVector::length(point - result.last()) > minSpacing)) {
            result.append(point);
            owners.append(owner);
        }
    };

    const qsizetype count = ring.count();
    for (qsizetype i = 0; i < count; i++) {
        const QPointF &previous = ring[(i + count - 1) % count];
        const QPointF &vertex = ring[i];
        const QPointF &next = ring[(i + 1) % count];
        const QPointF d1 = GeoZoneVector::unit(vertex - previous);
        const QPointF d2 = GeoZoneVector::unit(next - vertex);
        const QPointF n1 = _outwardNormal(d1, counterClockwise);
        const QPointF n2 = _outwardNormal(d2, counterClockwise);
        const QPointF p1 = vertex + (n1 * expansion.offsets[(i + count - 1) % count]);
        const QPointF p2 = vertex + (n2 * expansion.offsets[i]);
        const double turn = GeoZoneVector::cross(d1, d2);

        if (std::abs(turn) < 1e-9) {
            // Collinear or folding back, the offset lines don't meet
            append(p1, i);
            append(p2, i);
            continue;
        }

        const bool convex = counterClockwise ? (turn > 0) : (turn < 0);
        const QPointF mitre = _lineIntersection(p1, d1, p2, d2);
        // The mitre may use up to half of each edge, leaving the rest for the corner at its other end
        const bool mitreFits = (QPointF::dotProduct(mitre - vertex, d1) >= -(GeoZoneVector::length(vertex - previous) / 2)) &&
                               (QPointF::dotProduct(mitre - vertex, d2) <= (GeoZoneVector::length(next - vertex) / 2));
        if (convex && (QPointF::dotProduct(n1, n2) < -0.5)) {
            QPointF bisector = GeoZoneVector::unit(n1 + n2);
            if (bisector.isNull()) {
                bisector = d1;
            }
            const QPointF capPoint = vertex + (bisector * expansion.capDistances[i]);
            const QPointF capDirection(-bisector.y(), bisector.x());
            append(_lineIntersection(p1, d1, capPoint, capDirection), i);
            append(_lineIntersection(p2, d2, capPoint, capDirection), i);
        } else if (!mitreFits) {
            // Nearly parallel edges with different offsets meet far away, step between them at the vertex instead
            append(p1, i);
            append(p2, i);
        } else {
            append(mitre, i);
        }
    }
    if ((result.count() > 1) && (GeoZoneVector::length(result.first() - result.last()) <= minSpacing)) {
        result.removeLast();
        owners.removeLast();
    }
    return result;
}

QRectF _segmentBounds(const QPointF &a, const QPointF &b)
{
    return QRectF(a, b).normalized();
}

QRectF _ringBounds(const QList<QPointF> &ring)
{
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (const QPointF &point : ring) {
        minX = std::min(minX, point.x());
        minY = std::min(minY, point.y());
        maxX = std::max(maxX, point.x());
        maxY = std::max(maxY, point.y());
    }
    return QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
}

/// Uniform grid of segment bounds to find candidate intersections without testing all pairs
class SegmentGrid
{
public:
    SegmentGrid(const QRectF &bounds, qsizetype segmentCount)
        : _bounds(bounds)
        , _size(std::clamp(static_cast<int>(std::sqrt(static_cast<double>(segmentCount))), 1, 512))
        , _cellWidth(std::max(bounds.width() / _size, 1e-6))
        , _cellHeight(std::max(bounds.height() / _size, 1e-6))
        , _cells(static_cast<qsizetype>(_size) * _size)
        , _marks(segmentCount, 0)
    {
    }

    void insert(qsizetype id, const QRectF &box)
    {
        _forCells(box, [this, id](qsizetype cell) { _cells[cell].append(id); });
    }

    /// Calls function once for each segment whose cells overlap the box
    template<typename Function>
    void query(const QRectF &box, Function function)
    {
        _stamp++;
        _forCells(box, [this, &function](qsizetype cell) {
            for (const qsizetype id : std::as_const(_cells[cell])) {
                if (_marks[id] != _stamp) {
                    _marks[id] = _stamp;
                    function(id);
                }
            }
        });
    }

private:
    int _cellIndex(double value, double origin, double cellSize) const
    {
        return std::clamp(static_cast<int>((value - origin) / cellSize), 0, _size - 1);
    }

    template<typename Function>
    void _forCells(const QRectF &box, Function function) const
    {
        const int x0 = _cellIndex(box.left(), _bounds.left(), _cellWidth);
        const int x1 = _cellIndex(box.right(), _bounds.left(), _cellWidth);
        const int y0 = _cellIndex(box.top(), _bounds.top(), _cellHeight);
        const int y1 = _cellIndex(box.bottom(), _bounds.top(), _cellHeight);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                function((static_cast<qsizetype>(y) * _size) + x);
            }
        }
    }

    QRectF _bounds;
    int _size = 1;
    double _cellWidth = 1;
    double _cellHeight = 1;
    QList<QList<qsizetype>> _cells;
    QList<int> _marks;
    int _stamp = 0;
};

/// Finds where the expanded ring intersects itself or the original ring
/// @param badExpandedEdges Receives the expanded edges involved, edge i runs from vertex i to i + 1
/// @param badOriginalEdges Receives the original edges involved
void _findCrossings(const QList<QPointF> &expanded, const QList<QPointF> &original, QList<qsizetype> &badExpandedEdges, QList<qsizetype> &badOriginalEdges)
{
    const qsizetype count = expanded.count();
    const QRectF bounds = _ringBounds(expanded).united(_ringBounds(original));
    SegmentGrid grid(bounds, count);
    QList<QRectF> edgeBounds;
    edgeBounds.reserve(count);
    for (qsizetype i = 0; i < count; i++) {
        edgeBounds.append(_segmentBounds(expanded[i], expanded[(i + 1) % count]));
        grid.insert(i, edgeBounds.last());
    }

    for (qsizetype i = 0; i < count; i++) {
        const QPointF &a1 = expanded[i];
        const QPointF &a2 = expanded[(i + 1) % count];
        const QPointF &a3 = expanded[(i + 2) % count];
        // Adjacent edges may only share their vertex, not fold back over each other
        if ((GeoZoneVector::orientation(a1, a2, a3) == 0) && (QPointF::dotProduct(a2 - a1, a3 - a2) < 0)) {
            badExpandedEdges.append(i);
            badExpandedEdges.append((i + 1) % count);
        }
        grid.query(edgeBounds[i], [&](qsizetype j) {
            if ((j <= i) || (j == ((i + 1) % count)) || (i == ((j + 1) % count))) {
                return;
            }
            if (segmentsIntersect(a1, a2, expanded[j], expanded[(j + 1) % count])) {
                badExpandedEdges.append(i);
                badExpandedEdges.append(j);
            }
        });
    }

    for (qsizetype i = 0; i < original.count(); i++) {
        const QPointF &a = original[i];
        const QPointF &b = original[(i + 1) % original.count()];
        grid.query(_segmentBounds(a, b), [&](qsizetype j) {
            if (segmentsIntersect(a, b, expanded[j], expanded[(j + 1) % count])) {
                badExpandedEdges.append(j);
                badOriginalEdges.append(i);
            }
        });
    }
}

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

/// @return Winding number of the unclosed ring around the point, positive counterclockwise, nothing when the point is on
///         the boundary within kBoundaryTolerance
std::optional<int> _winding(const QList<QPointF> &ring, const QPointF &point)
{
    int winding = 0;
    for (qsizetype i = 0, j = ring.count() - 1; i < ring.count(); j = i++) {
        const QPointF &a = ring[j];
        const QPointF &b = ring[i];
        const QPointF edge = b - a;
        const double side = GeoZoneVector::cross(edge, point - a);
        if ((point.x() >= ((std::min)(a.x(), b.x()) - kBoundaryTolerance)) && (point.x() <= ((std::max)(a.x(), b.x()) + kBoundaryTolerance)) &&
            (point.y() >= ((std::min)(a.y(), b.y()) - kBoundaryTolerance)) && (point.y() <= ((std::max)(a.y(), b.y()) + kBoundaryTolerance)) &&
            (std::abs(side) <= (kBoundaryTolerance * GeoZoneVector::length(edge)))) {
            return std::nullopt;
        }
        // Edges crossing the horizontal line through the point upwards with the point on their left, or downwards with
        // the point on their right
        if (a.y() <= point.y()) {
            if ((b.y() > point.y()) && (side > 0)) {
                winding++;
            }
        } else if ((b.y() <= point.y()) && (side < 0)) {
            winding--;
        }
    }
    return winding;
}

bool _filled(int winding, Qt::FillRule fillRule)
{
    return (fillRule == Qt::WindingFill) ? (winding != 0) : ((winding % 2) != 0);
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

LocalProjection::LocalProjection(const QGeoCoordinate &origin)
    : _originLatitude(origin.latitude())
    , _originLongitude(origin.longitude())
    , _metersPerDegreeLatitude(kEarthRadiusMeters * kDegreesToRadians)
    , _metersPerDegreeLongitude(kEarthRadiusMeters * kDegreesToRadians * std::cos(origin.latitude() * kDegreesToRadians))
{
}

QPointF LocalProjection::toLocal(const QGeoCoordinate &coordinate) const
{
    return lonLatToLocal(QPointF(coordinate.longitude(), coordinate.latitude()));
}

QPointF LocalProjection::lonLatToLocal(const QPointF &lonLat) const
{
    double longitudeDelta = lonLat.x() - _originLongitude;
    if (longitudeDelta > 180) {
        longitudeDelta -= 360;
    } else if (longitudeDelta < -180) {
        longitudeDelta += 360;
    }
    return QPointF(longitudeDelta * _metersPerDegreeLongitude, (lonLat.y() - _originLatitude) * _metersPerDegreeLatitude);
}

QPointF LocalProjection::lonLatToLocal(const QPointF &lonLat, const QPointF &previous) const
{
    const double previousDelta = (_metersPerDegreeLongitude != 0) ? (previous.x() / _metersPerDegreeLongitude) : 0;
    double longitudeDelta = lonLat.x() - _originLongitude;
    // Whole turns, so that unwrapped longitudes are projected exactly like single points
    longitudeDelta += 360.0 * std::round((previousDelta - longitudeDelta) / 360.0);
    return QPointF(longitudeDelta * _metersPerDegreeLongitude, (lonLat.y() - _originLatitude) * _metersPerDegreeLatitude);
}

QList<QPointF> LocalProjection::lonLatToLocal(const QList<QPointF> &lonLats) const
{
    QList<QPointF> points;
    points.reserve(lonLats.count());
    for (const QPointF &lonLat : lonLats) {
        points.append(points.isEmpty() ? lonLatToLocal(lonLat) : lonLatToLocal(lonLat, points.constLast()));
    }
    return points;
}

double LocalProjection::eastScaleAt(double latitude) const
{
    const double originCos = std::cos(_originLatitude * kDegreesToRadians);
    return (originCos > 1e-9) ? (std::cos(latitude * kDegreesToRadians) / originCos) : 1.0;
}

QPointF LocalProjection::toLocal(const QGeoCoordinate &coordinate, const QPointF &previous) const
{
    return lonLatToLocal(QPointF(coordinate.longitude(), coordinate.latitude()), previous);
}

QList<QPointF> LocalProjection::toLocal(const QList<QGeoCoordinate> &coordinates) const
{
    QList<QPointF> points;
    points.reserve(coordinates.count());
    for (const QGeoCoordinate &coordinate : coordinates) {
        points.append(points.isEmpty() ? toLocal(coordinate) : toLocal(coordinate, points.constLast()));
    }
    return points;
}

QGeoCoordinate LocalProjection::toGeo(const QPointF &point) const
{
    double longitude = _originLongitude + (point.x() / _metersPerDegreeLongitude);
    if (longitude > 180) {
        longitude -= 360;
    } else if (longitude < -180) {
        longitude += 360;
    }
    return QGeoCoordinate(_originLatitude + (point.y() / _metersPerDegreeLatitude), longitude);
}

bool ringContains(const QList<QPointF> &ring, const QPointF &point, Qt::FillRule fillRule)
{
    const std::optional<int> winding = _winding(ring, point);
    return !winding || _filled(*winding, fillRule);
}

bool polygonContains(const QList<QList<QPointF>> &rings, const QPointF &point, Qt::FillRule fillRule)
{
    if (rings.isEmpty()) {
        return false;
    }
    const std::optional<int> outer = _winding(rings.first(), point);
    if (!outer) {
        return true;
    }
    if (!_filled(*outer, fillRule)) {
        return false;
    }
    for (qsizetype i = 1; i < rings.count(); i++) {
        const std::optional<int> hole = _winding(rings[i], point);
        // The boundary of a hole is that of the polygon
        if (!hole) {
            return true;
        }
        if (_filled(*hole, Qt::OddEvenFill)) {
            return false;
        }
    }
    return true;
}

double signedArea(const QList<QPointF> &ring)
{
    double area = 0;
    for (qsizetype i = 0; i < ring.count(); i++) {
        area += GeoZoneVector::cross(ring[i], ring[(i + 1) % ring.count()]);
    }
    return area / 2;
}

double signedArea(const QList<QGeoCoordinate> &ring)
{
    double area = 0;
    for (qsizetype i = 0; i < ring.count(); i++) {
        const QGeoCoordinate &a = ring[i];
        const QGeoCoordinate &b = ring[(i + 1) % ring.count()];
        area += (a.longitude() * b.latitude()) - (b.longitude() * a.latitude());
    }
    return area / 2;
}

bool segmentsIntersect(const QPointF &a1, const QPointF &a2, const QPointF &b1, const QPointF &b2)
{
    const int o1 = GeoZoneVector::orientation(a1, a2, b1);
    const int o2 = GeoZoneVector::orientation(a1, a2, b2);
    const int o3 = GeoZoneVector::orientation(b1, b2, a1);
    const int o4 = GeoZoneVector::orientation(b1, b2, a2);

    if ((o1 != o2) && (o3 != o4)) {
        return true;
    }
    return ((o1 == 0) && _onSegment(a1, a2, b1)) || ((o2 == 0) && _onSegment(a1, a2, b2)) ||
           ((o3 == 0) && _onSegment(b1, b2, a1)) || ((o4 == 0) && _onSegment(b1, b2, a2));
}

double distanceToSegment(const QPointF &point, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double lengthSquared = QPointF::dotProduct(ab, ab);
    const double t = (lengthSquared > 0) ? std::clamp(QPointF::dotProduct(point - a, ab) / lengthSquared, 0.0, 1.0) : 0.0;
    return GeoZoneVector::length(a + (ab * t) - point);
}

double distanceToRing(const QPointF &point, const QList<QPointF> &ring, double xScale)
{
    const auto offset = [&point, xScale](const QPointF &vertex) {
        return QPointF((vertex.x() - point.x()) * xScale, vertex.y() - point.y());
    };
    double minDistance = std::numeric_limits<double>::max();
    for (qsizetype i = 0; i < ring.count(); i++) {
        minDistance = std::min(minDistance, distanceToSegment(QPointF(), offset(ring[i]), offset(ring[(i + 1) % ring.count()])));
    }
    return minDistance;
}

bool segmentIntersectsRing(const QPointF &a, const QPointF &b, const QList<QPointF> &ring)
{
    for (qsizetype i = 0; i < ring.count(); i++) {
        if (segmentsIntersect(a, b, ring[i], ring[(i + 1) % ring.count()])) {
            return true;
        }
    }
    return false;
}

QList<Interval> segmentPolygonIntervals(const QPointF &a, const QPointF &b, const QList<QList<QPointF>> &rings, Qt::FillRule fillRule)
{
    if (rings.isEmpty()) {
        return {};
    }
    const QPointF direction = b - a;
    const double lengthSquared = QPointF::dotProduct(direction, direction);
    if (lengthSquared < 1e-12) {
        return polygonContains(rings, a, fillRule) ? QList<Interval>{ { 0, 1 } } : QList<Interval>{};
    }
    const double length = std::sqrt(lengthSquared);

    // Fractions where the segment meets the boundary, which is inside, and the segment ends
    QList<std::pair<double, bool>> cuts = { { 0, false }, { 1, false } };
    constexpr double tolerance = 1e-9;
    for (const QList<QPointF> &ring : rings) {
        for (qsizetype i = 0; i < ring.count(); i++) {
            const QPointF &edgeStart = ring[i];
            const QPointF edge = ring[(i + 1) % ring.count()] - edgeStart;
            const QPointF offset = edgeStart - a;
            const double denominator = GeoZoneVector::cross(direction, edge);
            if (std::abs(denominator) <= (tolerance * length * GeoZoneVector::length(edge))) {
                // Parallel, a collinear edge overlaps the segment on the boundary
                if (std::abs(GeoZoneVector::cross(offset, direction)) > (1e-6 * length)) {
                    continue;
                }
                const double t1 = QPointF::dotProduct(offset, direction) / lengthSquared;
                const double t2 = QPointF::dotProduct(offset + edge, direction) / lengthSquared;
                const double first = (std::max)((std::min)(t1, t2), 0.0);
                const double last = (std::min)((std::max)(t1, t2), 1.0);
                if (first <= last) {
                    cuts.append({ first, true });
                    cuts.append({ last, true });
                }
                continue;
            }
            const double t = GeoZoneVector::cross(offset, edge) / denominator;
            const double s = GeoZoneVector::cross(offset, direction) / denominator;
            if ((t >= -tolerance) && (t <= (1 + tolerance)) && (s >= -tolerance) && (s <= (1 + tolerance))) {
                cuts.append({ std::clamp(t, 0.0, 1.0), true });
            }
        }
    }
    std::sort(cuts.begin(), cuts.end());

    // Between consecutive cuts the segment is either inside or outside
    QList<Interval> intervals;
    const auto pointAt = [&a, &direction](double t) { return a + (direction * t); };
    for (qsizetype i = 0; i < cuts.count(); i++) {
        const double t = cuts[i].first;
        const bool boundary = cuts[i].second || ((i + 1 < cuts.count()) && (cuts[i + 1].first == t) && cuts[i + 1].second);
        if (boundary || polygonContains(rings, pointAt(t), fillRule)) {
            intervals.append({ t, t });
        }
        if ((i + 1 < cuts.count()) && (cuts[i + 1].first > t) && polygonContains(rings, pointAt((t + cuts[i + 1].first) / 2), fillRule)) {
            intervals.append({ t, cuts[i + 1].first });
        }
    }
    return intervals;
}

std::optional<Interval> segmentCircleInterval(const QPointF &a, const QPointF &b, double radius)
{
    const QPointF direction = b - a;
    const double quadratic = QPointF::dotProduct(direction, direction);
    const double linear = 2 * QPointF::dotProduct(a, direction);
    const double constant = QPointF::dotProduct(a, a) - (radius * radius);
    if (quadratic < 1e-12) {
        return (constant <= 0) ? std::make_optional(Interval{ 0, 1 }) : std::nullopt;
    }
    const double discriminant = (linear * linear) - (4 * quadratic * constant);
    if (discriminant < 0) {
        return std::nullopt;
    }
    const double root = std::sqrt(discriminant);
    const double first = (-linear - root) / (2 * quadratic);
    const double last = (-linear + root) / (2 * quadratic);
    if ((last < 0) || (first > 1)) {
        return std::nullopt;
    }
    return Interval{ (std::max)(first, 0.0), (std::min)(last, 1.0) };
}

bool isSimple(const QList<QPointF> &ring)
{
    const qsizetype count = ring.count();
    if (count < 3) {
        return false;
    }

    for (qsizetype i = 0; i < count; i++) {
        const QPointF &a1 = ring[i];
        const QPointF &a2 = ring[(i + 1) % count];
        const QPointF &a3 = ring[(i + 2) % count];
        // Adjacent edges may only share their vertex, not fold back over each other
        if ((GeoZoneVector::orientation(a1, a2, a3) == 0) && (QPointF::dotProduct(a2 - a1, a3 - a2) < 0)) {
            return false;
        }
    }

    const auto adjacent = [count](qsizetype i, qsizetype j) { return (j == ((i + 1) % count)) || (i == ((j + 1) % count)); };
    constexpr qsizetype smallRing = 32;
    if (count <= smallRing) {
        for (qsizetype i = 0; i < count; i++) {
            const QPointF &a1 = ring[i];
            const QPointF &a2 = ring[(i + 1) % count];
            for (qsizetype j = i + 2; j < count; j++) {
                const QPointF &b1 = ring[j];
                const QPointF &b2 = ring[(j + 1) % count];
                const bool apart = ((std::max)(a1.x(), a2.x()) < (std::min)(b1.x(), b2.x())) || ((std::max)(b1.x(), b2.x()) < (std::min)(a1.x(), a2.x())) ||
                                   ((std::max)(a1.y(), a2.y()) < (std::min)(b1.y(), b2.y())) || ((std::max)(b1.y(), b2.y()) < (std::min)(a1.y(), a2.y()));
                if (!apart && !adjacent(i, j) && segmentsIntersect(a1, a2, b1, b2)) {
                    return false;
                }
            }
        }
        return true;
    }

    // Sweep over the edges in the order of their west end, testing each against the following edges which overlap it
    // in x, so that large rings don't take a test of all pairs
    QList<QRectF> bounds;
    bounds.reserve(count);
    for (qsizetype i = 0; i < count; i++) {
        bounds.append(_segmentBounds(ring[i], ring[(i + 1) % count]));
    }
    QList<qsizetype> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&bounds](qsizetype a, qsizetype b) { return bounds[a].left() < bounds[b].left(); });
    for (qsizetype k = 0; k < count; k++) {
        const qsizetype i = order[k];
        const QRectF &box = bounds[i];
        for (qsizetype l = k + 1; (l < count) && (bounds[order[l]].left() <= box.right()); l++) {
            const qsizetype j = order[l];
            if (adjacent(i, j) || (bounds[j].top() > box.bottom()) || (bounds[j].bottom() < box.top())) {
                continue;
            }
            if (segmentsIntersect(ring[i], ring[(i + 1) % count], ring[j], ring[(j + 1) % count])) {
                return false;
            }
        }
    }
    return true;
}

QList<QGeoCoordinate> simplifyOutward(const QList<QGeoCoordinate> &ring, double toleranceMeters)
{
    if ((ring.count() < 4) || (toleranceMeters <= 0)) {
        return ring;
    }

    const LocalProjection projection(ring.first());
    QList<QPointF> original;
    for (const QPointF &point : projection.toLocal(ring)) {
        if (original.isEmpty() || (point != original.last())) {
            original.append(point);
        }
    }
    if ((original.count() > 1) && (original.first() == original.last())) {
        original.removeLast();
    }
    if (original.count() < 4) {
        return ring;
    }

    const qsizetype count = original.count();
    // Douglas-Peucker from vertex 0 and the vertex farthest from it
    qsizetype farthest = 0;
    for (qsizetype i = 1; i < count; i++) {
        if (GeoZoneVector::length(original[i] - original[0]) > GeoZoneVector::length(original[farthest] - original[0])) {
            farthest = i;
        }
    }
    QList<bool> keep(count, false);
    keep[0] = keep[farthest] = true;
    // Keeping a vertex on each side makes polygons smaller than the tolerance at least a triangle
    _simplifyChain(original, 0, farthest, toleranceMeters, keep, true);
    _simplifyChain(original, farthest, count, toleranceMeters, keep, true);
    QList<double> chainTolerances(count, toleranceMeters);  ///< Tolerance used for the chain starting at a kept vertex

    // The orientation of the simplified ring may flip when it degenerates, so the original defines outward
    const bool counterClockwise = signedArea(original) > 0;

    // Narrow features can make the expanded ring intersect itself or the original ring. Simplify the chains
    // around each problem with half the tolerance and try again, which keeps the rest of the ring unchanged.
    constexpr int maxIterations = 30;
    constexpr double minTolerance = 1;
    for (int iteration = 0; iteration < maxIterations; iteration++) {
        QList<qsizetype> kept;
        for (qsizetype i = 0; i < count; i++) {
            if (keep[i]) {
                kept.append(i);
            }
        }
        if ((kept.count() < 3) || (kept.count() >= ring.count())) {
            break;
        }

        QList<QPointF> simplified;
        simplified.reserve(kept.count());
        for (const qsizetype index : kept) {
            simplified.append(original[index]);
        }

        // Move each edge out just past the vertices it dropped, which are at most the tolerance away. A small
        // margin keeps the vertices strictly inside despite rounding.
        constexpr double margin = 0.1;
        QList<qsizetype> owners;
        const QList<QPointF> expanded = _offsetRing(simplified, _expansion(original, kept, counterClockwise, margin), counterClockwise, owners);

        QList<qsizetype> badExpandedEdges;
        QList<qsizetype> badOriginalEdges;
        _findCrossings(expanded, original, badExpandedEdges, badOriginalEdges);

        if (badExpandedEdges.isEmpty() && badOriginalEdges.isEmpty()) {
            // Without crossings the original ring is either entirely inside or entirely outside
            if ((expanded.count() >= ring.count()) || !ringContains(expanded, original.first())) {
                break;
            }
            QList<QGeoCoordinate> result;
            result.reserve(expanded.count());
            for (const QPointF &point : expanded) {
                result.append(projection.toGeo(point));
            }
            return result;
        }

        // Map the problems to edges of the simplified ring, where edge i runs from kept[i] to kept[i + 1]
        const qsizetype keptCount = kept.count();
        QList<bool> badEdges(keptCount, false);
        const auto markBad = [&badEdges, keptCount](qsizetype edge) {
            for (qsizetype neighbor = edge - 1; neighbor <= edge + 1; neighbor++) {
                badEdges[(neighbor + keptCount) % keptCount] = true;
            }
        };
        for (const qsizetype edge : std::as_const(badExpandedEdges)) {
            const qsizetype from = owners[edge];
            if (from == owners[(edge + 1) % expanded.count()]) {
                // Corner edge created at a vertex, involving the edges on both sides of it
                markBad(from - 1);
            }
            markBad(from);
        }
        for (const qsizetype edge : std::as_const(badOriginalEdges)) {
            markBad(std::upper_bound(kept.cbegin(), kept.cend(), edge) - kept.cbegin() - 1);
        }

        bool refined = false;
        for (qsizetype edge = 0; edge < keptCount; edge++) {
            if (!badEdges[edge]) {
                continue;
            }
            const qsizetype first = kept[edge];
            const qsizetype last = (edge + 1 < keptCount) ? kept[edge + 1] : (kept.first() + count);
            if (last - first < 2) {
                continue;
            }
            const double tolerance = chainTolerances[first] / 2;
            if (tolerance < minTolerance) {
                for (qsizetype i = first + 1; i < last; i++) {
                    keep[i % count] = true;
                }
            } else {
                _simplifyChain(original, first, last, tolerance, keep);
            }
            for (qsizetype i = first; i < last; i++) {
                if (keep[i % count]) {
                    chainTolerances[i % count] = tolerance;
                }
            }
            refined = true;
        }
        if (!refined) {
            break;
        }
    }

    return ring;
}

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

double geoidHeight(const QGeoCoordinate &coordinate)
{
    constexpr int rows = 181;
    constexpr int columns = 360;
    // Decimeters, rows from 90N to 90S, columns from 0E eastwards
    static const QList<qint16> grid = []() {
        QList<qint16> values;
        QFile file(QStringLiteral(":/geoid/egm96_1deg.bin"));
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.readAll();
            if (bytes.size() == (rows * columns * 2)) {
                values.reserve(rows * columns);
                for (int i = 0; i < (rows * columns); i++) {
                    values.append(qFromLittleEndian<qint16>(bytes.constData() + (i * 2)));
                }
            }
        }
        return values;
    }();

    if (!coordinate.isValid() || grid.isEmpty()) {
        return qQNaN();
    }

    const double row = 90.0 - coordinate.latitude();
    const double column = std::fmod(coordinate.longitude() + 360.0, 360.0);
    const int row0 = std::clamp(static_cast<int>(std::floor(row)), 0, rows - 1);
    const int column0 = static_cast<int>(std::floor(column)) % columns;
    const int row1 = std::min(row0 + 1, rows - 1);
    const int column1 = (column0 + 1) % columns;
    const double rowFraction = std::clamp(row - row0, 0.0, 1.0);
    const double columnFraction = column - std::floor(column);
    const auto value = [](int r, int c) { return grid[(r * columns) + c] / 10.0; };

    return (value(row0, column0) * (1 - rowFraction) * (1 - columnFraction)) +
           (value(row0, column1) * (1 - rowFraction) * columnFraction) +
           (value(row1, column0) * rowFraction * (1 - columnFraction)) +
           (value(row1, column1) * rowFraction * columnFraction);
}

} // namespace GeoZoneGeometry
