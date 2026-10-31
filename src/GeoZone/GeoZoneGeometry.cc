/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneGeometry.h"

#include <QtCore/QFile>
#include <QtCore/QRectF>
#include <QtCore/QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace GeoZoneGeometry {

namespace {

constexpr double kEarthRadiusMeters = 6371008.8;
constexpr double kDegreesToRadians = M_PI / 180.0;

double _cross(const QPointF &a, const QPointF &b)
{
    return (a.x() * b.y()) - (a.y() * b.x());
}

double _length(const QPointF &vector)
{
    return std::hypot(vector.x(), vector.y());
}

QPointF _unit(const QPointF &vector)
{
    const double length = _length(vector);
    return (length > 0) ? (vector / length) : QPointF();
}

/// @return Sign of the turn from a->b to a->c, 0 when collinear
int _orientation(const QPointF &a, const QPointF &b, const QPointF &c)
{
    const double value = _cross(b - a, c - a);
    if (std::abs(value) < 1e-12) {
        return 0;
    }
    return (value > 0) ? 1 : -1;
}

bool _onSegment(const QPointF &a, const QPointF &b, const QPointF &point)
{
    return (point.x() >= std::min(a.x(), b.x())) && (point.x() <= std::max(a.x(), b.x())) &&
           (point.y() >= std::min(a.y(), b.y())) && (point.y() <= std::max(a.y(), b.y()));
}

double _signedArea(const QList<QPointF> &ring)
{
    double area = 0;
    for (qsizetype i = 0; i < ring.count(); i++) {
        area += _cross(ring[i], ring[(i + 1) % ring.count()]);
    }
    return area / 2;
}

/// Intersection of the lines through p with direction d and through q with direction e
QPointF _lineIntersection(const QPointF &p, const QPointF &d, const QPointF &q, const QPointF &e)
{
    const double denominator = _cross(d, e);
    if (std::abs(denominator) < 1e-12) {
        return p;
    }
    return p + (d * (_cross(q - p, e) / denominator));
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
        directions.append(_unit(ring[kept[wrap(i + 1)]] - ring[kept[i]]));
        normals.append(_outwardNormal(directions.last(), counterClockwise));
    }
    QList<bool> convex;
    for (qsizetype i = 0; i < keptCount; i++) {
        const double turn = _cross(directions[wrap(i - 1)], directions[i]);
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
        QPointF bisector = _unit(normals[wrap(i - 1)] + normals[i]);
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
        if (result.isEmpty() || (_length(point - result.last()) > minSpacing)) {
            result.append(point);
            owners.append(owner);
        }
    };

    const qsizetype count = ring.count();
    for (qsizetype i = 0; i < count; i++) {
        const QPointF &previous = ring[(i + count - 1) % count];
        const QPointF &vertex = ring[i];
        const QPointF &next = ring[(i + 1) % count];
        const QPointF d1 = _unit(vertex - previous);
        const QPointF d2 = _unit(next - vertex);
        const QPointF n1 = _outwardNormal(d1, counterClockwise);
        const QPointF n2 = _outwardNormal(d2, counterClockwise);
        const QPointF p1 = vertex + (n1 * expansion.offsets[(i + count - 1) % count]);
        const QPointF p2 = vertex + (n2 * expansion.offsets[i]);
        const double turn = _cross(d1, d2);

        if (std::abs(turn) < 1e-9) {
            // Collinear or folding back, the offset lines don't meet
            append(p1, i);
            append(p2, i);
            continue;
        }

        const bool convex = counterClockwise ? (turn > 0) : (turn < 0);
        const QPointF mitre = _lineIntersection(p1, d1, p2, d2);
        // The mitre may use up to half of each edge, leaving the rest for the corner at its other end
        const bool mitreFits = (QPointF::dotProduct(mitre - vertex, d1) >= -(_length(vertex - previous) / 2)) &&
                               (QPointF::dotProduct(mitre - vertex, d2) <= (_length(next - vertex) / 2));
        if (convex && (QPointF::dotProduct(n1, n2) < -0.5)) {
            QPointF bisector = _unit(n1 + n2);
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
    if ((result.count() > 1) && (_length(result.first() - result.last()) <= minSpacing)) {
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
        if ((_orientation(a1, a2, a3) == 0) && (QPointF::dotProduct(a2 - a1, a3 - a2) < 0)) {
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
    double longitudeDelta = coordinate.longitude() - _originLongitude;
    if (longitudeDelta > 180) {
        longitudeDelta -= 360;
    } else if (longitudeDelta < -180) {
        longitudeDelta += 360;
    }
    return QPointF(longitudeDelta * _metersPerDegreeLongitude, (coordinate.latitude() - _originLatitude) * _metersPerDegreeLatitude);
}

QList<QPointF> LocalProjection::toLocal(const QList<QGeoCoordinate> &coordinates) const
{
    QList<QPointF> points;
    points.reserve(coordinates.count());
    for (const QGeoCoordinate &coordinate : coordinates) {
        points.append(toLocal(coordinate));
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

bool ringContains(const QList<QPointF> &ring, const QPointF &point)
{
    bool inside = false;
    for (qsizetype i = 0, j = ring.count() - 1; i < ring.count(); j = i++) {
        const QPointF &a = ring[i];
        const QPointF &b = ring[j];
        if (((a.y() > point.y()) != (b.y() > point.y())) &&
            (point.x() < (((b.x() - a.x()) * (point.y() - a.y())) / (b.y() - a.y())) + a.x())) {
            inside = !inside;
        }
    }
    return inside;
}

bool segmentsIntersect(const QPointF &a1, const QPointF &a2, const QPointF &b1, const QPointF &b2)
{
    const int o1 = _orientation(a1, a2, b1);
    const int o2 = _orientation(a1, a2, b2);
    const int o3 = _orientation(b1, b2, a1);
    const int o4 = _orientation(b1, b2, a2);

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
    return _length(a + (ab * t) - point);
}

double distanceToRing(const QPointF &point, const QList<QPointF> &ring)
{
    double minDistance = std::numeric_limits<double>::max();
    for (qsizetype i = 0; i < ring.count(); i++) {
        minDistance = std::min(minDistance, distanceToSegment(point, ring[i], ring[(i + 1) % ring.count()]));
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
        if ((_orientation(a1, a2, a3) == 0) && (QPointF::dotProduct(a2 - a1, a3 - a2) < 0)) {
            return false;
        }
        for (qsizetype j = i + 2; j < count; j++) {
            if ((i == 0) && (j == count - 1)) {
                continue;
            }
            if (segmentsIntersect(a1, a2, ring[j], ring[(j + 1) % count])) {
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
    for (const QGeoCoordinate &coordinate : ring) {
        const QPointF point = projection.toLocal(coordinate);
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
        if (_length(original[i] - original[0]) > _length(original[farthest] - original[0])) {
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
    const bool counterClockwise = _signedArea(original) > 0;

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
