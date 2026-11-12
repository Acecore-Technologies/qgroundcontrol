/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFences.h"
#include "GeoZone.h"
#include "GeoZoneGeometry.h"
#include "GeoZonePolicy.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace GeoZoneFences {

int circleVertexCount(double radius, double simplifyTolerance)
{
    if (!(radius > 0)) {
        return minCircleVertices;
    }
    const double allowed = (std::max)(2 * simplifyTolerance, 0.02 * radius);
    const double halfAngle = std::acos(radius / (radius + allowed));
    const int count = static_cast<int>(std::ceil(std::numbers::pi / halfAngle));
    return std::clamp(count, minCircleVertices, maxCircleVertices);
}

QList<QGeoCoordinate> circlePolygon(const QGeoCircle &circle, int vertexCount)
{
    vertexCount = (std::max)(vertexCount, 3);
    // Vertices on the circumscribed circle, widened by 1% for the difference between the geodesic circle and the
    // local projections vehicles check fences in, which grows with the radius
    const double halfAngle = std::numbers::pi / vertexCount;
    const double vertexDistance = ((circle.radius() * 1.01) + boundaryTolerance) / std::cos(halfAngle);
    QList<QGeoCoordinate> polygon;
    polygon.reserve(vertexCount);
    for (int i = 0; i < vertexCount; i++) {
        const double azimuth = 360.0 * i / vertexCount;
        QGeoCoordinate vertex = circle.center().atDistanceAndAzimuth(vertexDistance, azimuth);
        vertex.setAltitude(qQNaN());
        polygon.append(vertex);
    }
    return polygon;
}

ZoneFences zoneFences(const GeoZone *zone, double simplifyTolerance, bool circlesAsPolygons)
{
    ZoneFences fences{ zone->exclusionPolygons(simplifyTolerance), zone->exclusionCircles() };
    if (circlesAsPolygons) {
        for (const QGeoCircle &circle : std::as_const(fences.circles)) {
            fences.polygons.append(circlePolygon(circle, circleVertexCount(circle.radius(), simplifyTolerance)));
        }
        fences.circles.clear();
    }
    return fences;
}

namespace {

/// @return Points in the areas the ring winds around an even number of times, each half the boundary tolerance beside
///         a stretch of an edge between its crossings with the other edges. Zones include these areas, see
///         GeoZoneGeometry::ringContains, while the even-odd rule of the autopilots leaves them out of a fence.
///         Nothing for rings which don't cross themselves, and for those crossing like a figure eight, whose parts
///         are each enclosed once.
QList<QPointF> _openOverlaps(const QList<QPointF> &ring)
{
    QList<QPointF> open;
    if (GeoZoneGeometry::isSimple(ring)) {
        return open;
    }
    const auto cross = [](const QPointF &a, const QPointF &b) { return (a.x() * b.y()) - (a.y() * b.x()); };
    constexpr double offset = boundaryTolerance / 2;
    const qsizetype count = ring.count();
    for (qsizetype i = 0; i < count; i++) {
        const QPointF &a = ring[i];
        const QPointF direction = ring[(i + 1) % count] - a;
        const double length = std::hypot(direction.x(), direction.y());
        if (!(length > 0)) {
            continue;
        }
        // Where the other edges cross this one, as fractions of it
        QList<double> cuts = { 0, 1 };
        for (qsizetype j = 0; j < count; j++) {
            const QPointF &c = ring[j];
            const QPointF other = ring[(j + 1) % count] - c;
            const double denominator = cross(direction, other);
            if ((j == i) || (denominator == 0)) {
                continue;
            }
            const double fraction = cross(c - a, other) / denominator;
            const double otherFraction = cross(c - a, direction) / denominator;
            if ((fraction > 0) && (fraction < 1) && (otherFraction >= 0) && (otherFraction <= 1)) {
                cuts.append(fraction);
            }
        }
        std::sort(cuts.begin(), cuts.end());
        const QPointF normal(-direction.y() / length, direction.x() / length);
        for (qsizetype cut = 1; cut < cuts.count(); cut++) {
            if (((cuts[cut] - cuts[cut - 1]) * length) <= GeoZoneGeometry::kBoundaryTolerance) {
                continue;
            }
            const QPointF middle = a + (direction * ((cuts[cut - 1] + cuts[cut]) / 2));
            for (const double side : { -offset, offset }) {
                const QPointF point = middle + (normal * side);
                if (GeoZoneGeometry::ringContains(ring, point, Qt::WindingFill) && !GeoZoneGeometry::ringContains(ring, point, Qt::OddEvenFill)) {
                    open.append(point);
                }
            }
        }
    }
    return open;
}

/// @return true if the vertices and edges of the polygon are within the fence filled with the even-odd rule, or
///         within the boundary tolerance of its ring
bool _boundaryWithin(const QList<QGeoCoordinate> &polygon, const GeoZoneGeometry::LocalProjection &projection, const QList<QList<QPointF>> &fenceRings)
{
    const QList<QPointF> &ring = fenceRings.first();
    const auto within = [&ring](const QPointF &point) {
        return GeoZoneGeometry::ringContains(ring, point, Qt::OddEvenFill) || (GeoZoneGeometry::distanceToRing(point, ring) <= boundaryTolerance);
    };
    for (qsizetype i = 0; i < polygon.count(); i++) {
        // Vertices inside, and edges not leaving the fence between them, like around a notch of the fence
        const QPointF a = projection.toLocal(polygon[i]);
        const QPointF b = projection.toLocal(polygon[(i + 1) % polygon.count()], a);
        if (!within(a)) {
            return false;
        }
        double covered = 0;
        QList<GeoZoneGeometry::Interval> inside = GeoZoneGeometry::segmentPolygonIntervals(a, b, fenceRings, Qt::OddEvenFill);
        inside.append({ 1, 1 });
        for (const GeoZoneGeometry::Interval &interval : std::as_const(inside)) {
            if (interval.first > covered) {
                for (const double fraction : { 0.25, 0.5, 0.75 }) {
                    if (!within(a + ((b - a) * (covered + ((interval.first - covered) * fraction))))) {
                        return false;
                    }
                }
            }
            covered = (std::max)(covered, interval.second);
        }
    }
    return true;
}

/// @return true if the polygon lies within the fence polygon up to the tolerance, edges included. Fences are filled
///         with the even-odd rule of the autopilots, so a fence crossing itself doesn't cover what it encloses twice:
///         neither a polygon reaching into such an area, nor one around it, nor the fence's own polygon.
bool _polygonWithin(const QList<QGeoCoordinate> &polygon, const QList<QGeoCoordinate> &fence)
{
    const GeoZoneGeometry::LocalProjection projection(fence.first());
    const QList<QList<QPointF>> fenceRings = { projection.toLocal(fence) };
    const QList<QPointF> &ring = fenceRings.first();
    // The boundary of the fence's own polygon is on the fence
    if ((polygon != fence) && !_boundaryWithin(polygon, projection, fenceRings)) {
        return false;
    }
    const QList<QPointF> open = _openOverlaps(ring);
    if (open.isEmpty()) {
        return true;
    }
    const QList<QPointF> polygonRing = projection.toLocal(polygon);
    return std::none_of(open.cbegin(), open.cend(), [&polygonRing](const QPointF &point) { return GeoZoneGeometry::ringContains(polygonRing, point); });
}

bool _polygonWithinCircle(const QList<QGeoCoordinate> &polygon, const QGeoCircle &fence)
{
    return std::all_of(polygon.cbegin(), polygon.cend(), [&fence](const QGeoCoordinate &point) {
        return fence.center().distanceTo(point) <= (fence.radius() + boundaryTolerance);
    });
}

bool _circleWithin(const QGeoCircle &circle, const QList<QGeoCoordinate> &fence)
{
    const GeoZoneGeometry::LocalProjection projection(circle.center());
    const QList<QPointF> ring = projection.toLocal(fence);
    return GeoZoneGeometry::ringContains(ring, QPointF(0, 0), Qt::OddEvenFill) &&
           (GeoZoneGeometry::distanceToRing(QPointF(0, 0), ring) >= (circle.radius() - boundaryTolerance));
}

} // namespace

bool fencesCover(const ZoneFences &fences, const GeoZone *zone)
{
    const ZoneFences zoneShapes = zoneFences(zone);
    if (zoneShapes.isEmpty()) {
        return false;
    }
    for (const QList<QGeoCoordinate> &polygon : zoneShapes.polygons) {
        const bool covered = std::any_of(fences.polygons.cbegin(), fences.polygons.cend(), [&polygon](const QList<QGeoCoordinate> &fence) { return _polygonWithin(polygon, fence); }) ||
                             std::any_of(fences.circles.cbegin(), fences.circles.cend(), [&polygon](const QGeoCircle &fence) { return _polygonWithinCircle(polygon, fence); });
        if (!covered) {
            return false;
        }
    }
    for (const QGeoCircle &circle : zoneShapes.circles) {
        const bool covered =
            std::any_of(fences.circles.cbegin(), fences.circles.cend(), [&circle](const QGeoCircle &fence) {
                return (fence.center().distanceTo(circle.center()) + circle.radius()) <= (fence.radius() + boundaryTolerance);
            }) ||
            std::any_of(fences.polygons.cbegin(), fences.polygons.cend(), [&circle](const QList<QGeoCoordinate> &fence) { return _circleWithin(circle, fence); });
        if (!covered) {
            return false;
        }
    }
    return true;
}

double distanceTo(const ZoneFences &fences, const QList<QList<QGeoCoordinate>> &lines)
{
    if (fences.isEmpty()) {
        return qInf();
    }
    const GeoZoneGeometry::LocalProjection projection(fences.polygons.isEmpty() ? fences.circles.first().center() : fences.polygons.first().first());
    QList<QList<QPointF>> localLines;
    for (const QList<QGeoCoordinate> &line : lines) {
        if (!line.isEmpty()) {
            localLines.append(projection.toLocal(line));
        }
    }

    double distance = qInf();
    for (const QList<QGeoCoordinate> &polygon : fences.polygons) {
        const QList<QPointF> ring = projection.toLocal(polygon);
        for (const QList<QPointF> &line : std::as_const(localLines)) {
            if (GeoZoneGeometry::ringContains(ring, line.first(), Qt::OddEvenFill)) {
                return 0;
            }
            distance = (std::min)(distance, GeoZoneGeometry::distanceToRing(line.first(), ring));
            for (qsizetype i = 1; i < line.count(); i++) {
                const QPointF &a = line[i - 1];
                const QPointF &b = line[i];
                if (GeoZoneGeometry::segmentIntersectsRing(a, b, ring)) {
                    return 0;
                }
                distance = (std::min)(distance, GeoZoneGeometry::distanceToRing(b, ring));
                for (const QPointF &vertex : ring) {
                    distance = (std::min)(distance, GeoZoneGeometry::distanceToSegment(vertex, a, b));
                }
            }
        }
    }
    for (const QGeoCircle &circle : fences.circles) {
        const QPointF center = projection.toLocal(circle.center());
        for (const QList<QPointF> &line : std::as_const(localLines)) {
            double lineDistance = std::hypot(line.first().x() - center.x(), line.first().y() - center.y());
            for (qsizetype i = 1; i < line.count(); i++) {
                lineDistance = (std::min)(lineDistance, GeoZoneGeometry::distanceToSegment(center, line[i - 1], line[i]));
            }
            distance = (std::min)(distance, (std::max)(lineDistance - circle.radius(), 0.0));
        }
    }
    return distance;
}

void Usage::addPolygon(qsizetype vertexCount)
{
    items += static_cast<int>(vertexCount);
    bytes += Capacity::arduPilotPolygonBytes + (Capacity::arduPilotVertexBytes * static_cast<int>(vertexCount));
    largestPolygon = (std::max)(largestPolygon, static_cast<int>(vertexCount));
}

void Usage::addCircle()
{
    items++;
    bytes += Capacity::arduPilotCircleBytes;
}

void Usage::addReturnPoint()
{
    items++;
    bytes += Capacity::arduPilotReturnPointBytes;
}

void Usage::add(const ZoneFences &fences)
{
    for (const QList<QGeoCoordinate> &polygon : fences.polygons) {
        addPolygon(polygon.count());
    }
    for (qsizetype i = 0; i < fences.circles.count(); i++) {
        addCircle();
    }
}

Usage &Usage::operator+=(const Usage &other)
{
    items += other.items;
    bytes += other.bytes;
    largestPolygon = (std::max)(largestPolygon, other.largestPolygon);
    return *this;
}

bool Capacity::fits(const Usage &usage) const
{
    if (!exclusions && (usage.items > 0)) {
        return false;
    }
    return ((maxItems <= 0) || (usage.items <= maxItems)) &&
           ((storageBytes <= 0) || (usage.items == 0) || ((usage.bytes + arduPilotFixedBytes) <= storageBytes)) &&
           ((maxPolygonVertices <= 0) || (usage.largestPolygon <= maxPolygonVertices));
}

namespace {

// Fence areas of ArduPilot's StorageManager layouts, for flight controllers with 16 KB of storage or more, with 8 KB or
// 15 KB and with 4 KB
constexpr std::array<int, 3> arduPilotCopterLayouts = { 560, Capacity::arduPilotCopterBytes, 48 };
constexpr std::array<int, 3> arduPilotOtherLayouts = { 672, Capacity::arduPilotOtherBytes, 160 };

int _arduPilotMaxItems(int storageBytes)
{
    return (std::max)(storageBytes - Capacity::arduPilotHeaderBytes, 0) / Capacity::arduPilotVertexBytes;
}

} // namespace

Capacity Capacity::px4(int majorVersion, int minorVersion)
{
    Capacity capacity;
    if ((majorVersion < 0) || (minorVersion < 0)) {
        // The smaller of both, since the version isn't known
        capacity.maxItems = px4LegacyMaxItems;
        capacity.estimated = true;
    } else {
        const bool full = (majorVersion > px4FullItemsMajor) || ((majorVersion == px4FullItemsMajor) && (minorVersion >= px4FullItemsMinor));
        capacity.maxItems = full ? px4MaxItems : px4LegacyMaxItems;
    }
    return capacity;
}

Capacity Capacity::arduPilot(bool copter, int sdCardKB)
{
    Capacity capacity;
    if (sdCardKB > 0) {
        // Clamped before it is multiplied: the parameter is a number the vehicle sent
        constexpr int maxKB = (arduPilotMaxBytes / 1024) + 1;
        capacity.storageBytes = (std::min)((std::min)(sdCardKB, maxKB) * 1024, arduPilotMaxBytes);
    } else {
        capacity.storageBytes = copter ? arduPilotCopterBytes : arduPilotOtherBytes;
        capacity.estimated = true;
    }
    capacity.maxItems = _arduPilotMaxItems(capacity.storageBytes);
    capacity.maxPolygonVertices = arduPilotMaxPolygonVertices;
    return capacity;
}

Capacity Capacity::conservative()
{
    Capacity capacity;
    capacity.maxItems = px4LegacyMaxItems;
    capacity.storageBytes = arduPilotCopterBytes;
    capacity.maxPolygonVertices = arduPilotMaxPolygonVertices;
    capacity.circles = false;
    capacity.estimated = true;
    return capacity;
}

Capacity Capacity::reducedBelow(const Usage &rejected) const
{
    Capacity capacity = *this;
    capacity.estimated = true;
    capacity.reduced = true;
    if (storageBytes > 0) {
        // The largest storage of a layout the fence doesn't fit, less than the storage now
        const int needed = rejected.bytes + arduPilotFixedBytes;
        int bytes = 0;
        for (const auto &layouts : { arduPilotCopterLayouts, arduPilotOtherLayouts }) {
            for (const int layout : layouts) {
                if ((layout < needed) && (layout < storageBytes)) {
                    bytes = (std::max)(bytes, layout);
                }
            }
        }
        if (bytes == 0) {
            bytes = (std::min)(needed, storageBytes) - 1;
        }
        capacity.storageBytes = (std::max)(bytes, arduPilotFixedBytes);
        capacity.maxItems = (maxItems > 0) ? (std::min)(maxItems, _arduPilotMaxItems(capacity.storageBytes)) : _arduPilotMaxItems(capacity.storageBytes);
        return capacity;
    }
    // Items only, like PX4: its smaller limits, then one item less each time. Never without a limit.
    const int limit = (maxItems > 0) ? (std::min)(maxItems, rejected.items) : rejected.items;
    int items = limit - 1;
    for (const int known : { px4LegacyMaxItems, px4ConstrainedMaxItems }) {
        if ((known < rejected.items) && (known < limit)) {
            items = known;
            break;
        }
    }
    capacity.maxItems = (std::max)(items, 1);
    return capacity;
}

Capacity Capacity::unsupported()
{
    Capacity capacity;
    capacity.exclusions = false;
    capacity.circles = false;
    return capacity;
}

QList<Candidate> candidates(const QList<const GeoZone*> &zones, const GeoZonePathEvaluator::Result &evaluation, const QList<QList<QGeoCoordinate>> &lines, double margin)
{
    QList<Candidate> result;
    result.reserve(zones.count());
    for (const GeoZone *zone : zones) {
        Candidate candidate{ zone };
        const GeoZonePathEvaluator::ZoneResult *const zoneResult = evaluation.find(zone);
        if (zoneResult && zoneResult->entered()) {
            candidate.proximity = Proximity::Entered;
            candidate.distance = 0;
        } else if (zoneResult && !zoneResult->stretches.isEmpty()) {
            // An exclusion fence would stop the vehicle where it may pass above or below the zone
            candidate.proximity = Proximity::Overflown;
            candidate.distance = 0;
            candidate.excluded = true;
        } else {
            candidate.distance = distanceTo(zoneFences(zone), lines);
            candidate.proximity = (candidate.distance <= margin) ? Proximity::Near : Proximity::Far;
        }
        result.append(candidate);
    }
    return result;
}

namespace {

int _tier(GeoZone::Restriction restriction)
{
    if (restriction == GeoZone::Prohibited) {
        return 0;
    }
    return GeoZonePolicy::isRestricted(restriction) ? 1 : 2;
}

size_t _fencesHash(const ZoneFences &fences)
{
    size_t hash = 0;
    for (const QList<QGeoCoordinate> &polygon : fences.polygons) {
        for (const QGeoCoordinate &coordinate : polygon) {
            hash = qHashMulti(hash, coordinate.latitude(), coordinate.longitude());
        }
    }
    for (const QGeoCircle &circle : fences.circles) {
        hash = qHashMulti(hash, circle.center().latitude(), circle.center().longitude(), circle.radius());
    }
    return hash;
}

} // namespace

Prepared prepare(const QList<Candidate> &candidates, const QList<double> &simplifyTolerances, bool circlesAsPolygons)
{
    Prepared prepared;
    if (!simplifyTolerances.isEmpty()) {
        prepared.simplifyTolerances = simplifyTolerances;
    }
    prepared.circlesAsPolygons = circlesAsPolygons;
    const double tolerance = prepared.simplifyTolerances.last();

    prepared.entries.reserve(candidates.count());
    for (const Candidate &candidate : candidates) {
        Prepared::Entry entry{ candidate };
        entry.usage.add(zoneFences(candidate.zone, tolerance, circlesAsPolygons));
        const ZoneFences original = zoneFences(candidate.zone);
        entry.hasShape = !original.isEmpty();
        entry.shapeHash = _fencesHash(original);
        prepared.entries.append(entry);
    }
    std::stable_sort(prepared.entries.begin(), prepared.entries.end(), [](const Prepared::Entry &a, const Prepared::Entry &b) {
        const GeoZone *const zoneA = a.candidate.zone;
        const GeoZone *const zoneB = b.candidate.zone;
        const auto key = [](const Prepared::Entry &entry) {
            const Candidate &candidate = entry.candidate;
            // Zones active only part of the flight aren't fenced, the zones with their area after them may be
            return std::make_tuple(candidate.partTime, _tier(candidate.zone->restriction()), static_cast<int>(candidate.proximity),
                                   (candidate.proximity == Proximity::Far) ? candidate.distance : 0.0,
                                   GeoZone::restrictionSeverity(candidate.zone->restriction()), entry.usage.items);
        };
        const auto keyA = key(a);
        const auto keyB = key(b);
        if (keyA != keyB) {
            return keyA < keyB;
        }
        return zoneA->displayName().localeAwareCompare(zoneB->displayName()) < 0;
    });
    return prepared;
}

QList<const GeoZone*> Selection::selectedZones() const
{
    QList<const GeoZone*> zones;
    for (const Choice &choice : choices) {
        if (choice.status == Status::Selected) {
            zones.append(choice.candidate.zone);
        }
    }
    return zones;
}

QList<Choice> Selection::notFenced() const
{
    QList<Choice> result;
    for (const Choice &choice : choices) {
        if (!choice.fenced()) {
            result.append(choice);
        }
    }
    return result;
}

Selection select(const QList<Candidate> &candidates, const Capacity &capacity, const Usage &reserved, const QList<double> &simplifyTolerances)
{
    return select(prepare(candidates, simplifyTolerances, !capacity.circles), capacity, reserved);
}

Selection select(const Prepared &prepared, const Capacity &capacity, const Usage &reserved)
{
    const QList<double> &tolerances = prepared.simplifyTolerances;
    const double largest = tolerances.last();
    const bool circlesAsPolygons = prepared.circlesAsPolygons;

    QList<Choice> choices;
    choices.reserve(prepared.entries.count());
    QList<qsizetype> selected;
    QMultiHash<size_t, qsizetype> shapes;   // Fences of the zones before, to find duplicates
    Usage usage = reserved;
    for (const Prepared::Entry &entry : prepared.entries) {
        const Candidate &candidate = entry.candidate;
        Choice choice{ candidate };
        if (entry.hasShape) {
            for (auto it = shapes.constFind(entry.shapeHash); (it != shapes.cend()) && (it.key() == entry.shapeHash); ++it) {
                const Choice &earlier = choices[it.value()];
                if (zoneFences(earlier.candidate.zone) == zoneFences(candidate.zone)) {
                    choice.duplicateOf = earlier.candidate.zone;
                    choice.duplicateSelected = earlier.status == Status::Selected;
                    break;
                }
            }
            if (!choice.duplicateOf) {
                shapes.insert(entry.shapeHash, choices.count());
            }
        }

        choice.simplifyTolerance = largest;
        choice.usage = entry.usage;
        if (!entry.hasShape) {
            choice.status = Status::NoShape;
        } else if (choice.duplicateOf) {
            choice.status = Status::Duplicate;
        } else if (candidate.partTime) {
            choice.status = Status::PartTime;
        } else if (candidate.excluded) {
            choice.status = Status::Excluded;
        } else if (!capacity.exclusions) {
            choice.status = Status::Unsupported;
        } else if (capacity.fits(usage + choice.usage)) {
            choice.status = Status::Selected;
            choice.fences = zoneFences(candidate.zone, largest, circlesAsPolygons);
            usage += choice.usage;
            selected.append(choices.count());
        } else {
            choice.status = capacity.fits(reserved + choice.usage) ? Status::NoRoom : Status::TooLarge;
        }
        choices.append(choice);
    }

    // The zones first in priority get the least simplified fences which fit
    for (const qsizetype i : std::as_const(selected)) {
        if (tolerances.count() < 2) {
            break;
        }
        Choice &choice = choices[i];
        Usage others = reserved;
        for (const qsizetype j : std::as_const(selected)) {
            if (j != i) {
                others += choices[j].usage;
            }
        }
        for (const double tolerance : tolerances) {
            if (tolerance >= choice.simplifyTolerance) {
                break;
            }
            const ZoneFences fences = zoneFences(choice.candidate.zone, tolerance, circlesAsPolygons);
            Usage fencesUsage;
            fencesUsage.add(fences);
            if (capacity.fits(others + fencesUsage)) {
                choice.fences = fences;
                choice.simplifyTolerance = tolerance;
                choice.usage = fencesUsage;
                break;
            }
        }
    }

    Selection selection;
    selection.reserved = reserved;
    selection.usage = reserved;
    selection.choices.reserve(choices.count());
    for (const qsizetype i : std::as_const(selected)) {
        selection.usage += choices[i].usage;
        selection.choices.append(choices[i]);
    }
    for (const Choice &choice : std::as_const(choices)) {
        if (choice.status != Status::Selected) {
            selection.choices.append(choice);
        }
    }
    return selection;
}

QString statusText(const Choice &choice)
{
    switch (choice.status) {
    case Status::Selected:
        break;
    case Status::Excluded:
        if (choice.candidate.proximity == Proximity::Overflown) {
            return QCoreApplication::translate("GeoZoneFences", "The flight passes above or below it, its fence would stop the vehicle there");
        }
        return QCoreApplication::translate("GeoZoneFences", "Left out by you");
    case Status::NoRoom:
        return QCoreApplication::translate("GeoZoneFences", "No room left in the vehicle fence");
    case Status::TooLarge:
        return QCoreApplication::translate("GeoZoneFences", "Too many fence points (%1) even simplified").arg(choice.usage.items);
    case Status::Unsupported:
        return QCoreApplication::translate("GeoZoneFences", "The vehicle doesn't support exclusion fences");
    case Status::NoShape:
        return QCoreApplication::translate("GeoZoneFences", "No area to fence");
    case Status::PartTime:
        return QCoreApplication::translate("GeoZoneFences", "Active only part of the flight, QGroundControl warns instead");
    case Status::Duplicate:
        if (choice.duplicateSelected) {
            return QCoreApplication::translate("GeoZoneFences", "Same area as %1, covered by its fence").arg(choice.duplicateOf ? choice.duplicateOf->displayName() : QString());
        }
        return QCoreApplication::translate("GeoZoneFences", "Same area as %1, which is left out").arg(choice.duplicateOf ? choice.duplicateOf->displayName() : QString());
    }
    return QString();
}

} // namespace GeoZoneFences
