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

/// @return true if the polygon lies within the fence polygon up to the tolerance, edges included
bool _polygonWithin(const QList<QGeoCoordinate> &polygon, const QList<QGeoCoordinate> &fence)
{
    if (polygon == fence) {
        return true;
    }
    const GeoZoneGeometry::LocalProjection projection(fence.first());
    const QList<QList<QPointF>> fenceRings = { projection.toLocal(fence) };
    const QList<QPointF> &ring = fenceRings.first();
    const auto near = [&ring](const QPointF &point) {
        return GeoZoneGeometry::ringContains(ring, point) || (GeoZoneGeometry::distanceToRing(point, ring) <= boundaryTolerance);
    };
    for (qsizetype i = 0; i < polygon.count(); i++) {
        // Vertices inside, and edges not leaving the fence between them, like around a notch of the fence
        const QPointF a = projection.toLocal(polygon[i]);
        const QPointF b = projection.toLocal(polygon[(i + 1) % polygon.count()], a);
        if (!near(a)) {
            return false;
        }
        double covered = 0;
        QList<GeoZoneGeometry::Interval> inside = GeoZoneGeometry::segmentPolygonIntervals(a, b, fenceRings);
        inside.append({ 1, 1 });
        for (const GeoZoneGeometry::Interval &interval : std::as_const(inside)) {
            if (interval.first > covered) {
                for (const double fraction : { 0.25, 0.5, 0.75 }) {
                    if (!near(a + ((b - a) * (covered + ((interval.first - covered) * fraction))))) {
                        return false;
                    }
                }
            }
            covered = (std::max)(covered, interval.second);
        }
    }
    return true;
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
    return GeoZoneGeometry::ringContains(ring, QPointF(0, 0)) &&
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
            if (GeoZoneGeometry::ringContains(ring, line.first())) {
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
    bytes += 2 + (8 * static_cast<int>(vertexCount));
    largestPolygon = (std::max)(largestPolygon, static_cast<int>(vertexCount));
}

void Usage::addCircle()
{
    items++;
    bytes += 13;
}

void Usage::addReturnPoint()
{
    items++;
    bytes += 9;
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

Capacity Capacity::px4()
{
    Capacity capacity;
    capacity.maxItems = px4MaxItems;
    return capacity;
}

Capacity Capacity::arduPilot(bool copter, int sdCardKB)
{
    Capacity capacity;
    if (sdCardKB > 0) {
        capacity.storageBytes = (std::min)(sdCardKB * 1024, arduPilotMaxBytes);
    } else {
        capacity.storageBytes = copter ? arduPilotCopterBytes : arduPilotOtherBytes;
        capacity.estimated = true;
    }
    capacity.maxItems = capacity.storageBytes / 8;
    capacity.maxPolygonVertices = arduPilotMaxPolygonVertices;
    return capacity;
}

Capacity Capacity::conservative()
{
    Capacity capacity;
    capacity.maxItems = px4MaxItems;
    capacity.storageBytes = arduPilotCopterBytes;
    capacity.maxPolygonVertices = arduPilotMaxPolygonVertices;
    capacity.circles = false;
    capacity.estimated = true;
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

QList<Candidate> sortedByPriority(const QList<Candidate> &candidates, const QList<double> &simplifyTolerances, bool circlesAsPolygons)
{
    const double tolerance = simplifyTolerances.isEmpty() ? 0 : simplifyTolerances.last();
    struct Keyed {
        Candidate candidate;
        int items = 0;
    };
    QList<Keyed> keyed;
    keyed.reserve(candidates.count());
    for (const Candidate &candidate : candidates) {
        Usage usage;
        usage.add(zoneFences(candidate.zone, tolerance, circlesAsPolygons));
        keyed.append({ candidate, usage.items });
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed &a, const Keyed &b) {
        const GeoZone *const zoneA = a.candidate.zone;
        const GeoZone *const zoneB = b.candidate.zone;
        const auto key = [](const Keyed &keyed) {
            const Candidate &candidate = keyed.candidate;
            return std::make_tuple(_tier(candidate.zone->restriction()), static_cast<int>(candidate.proximity),
                                   (candidate.proximity == Proximity::Far) ? candidate.distance : 0.0,
                                   GeoZone::restrictionSeverity(candidate.zone->restriction()), keyed.items);
        };
        const auto keyA = key(a);
        const auto keyB = key(b);
        if (keyA != keyB) {
            return keyA < keyB;
        }
        return zoneA->displayName().localeAwareCompare(zoneB->displayName()) < 0;
    });
    QList<Candidate> sorted;
    sorted.reserve(keyed.count());
    for (const Keyed &entry : std::as_const(keyed)) {
        sorted.append(entry.candidate);
    }
    return sorted;
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

Selection select(const QList<Candidate> &candidates, const Capacity &capacity, const Usage &reserved, const QList<double> &simplifyTolerances)
{
    const QList<double> tolerances = simplifyTolerances.isEmpty() ? QList<double>{ 0 } : simplifyTolerances;
    const double largest = tolerances.last();
    const bool circlesAsPolygons = !capacity.circles;

    QList<Choice> choices;
    QMultiHash<size_t, qsizetype> shapes;   // Unsimplified fences of the zones before, to find duplicates
    Usage usage = reserved;
    for (const Candidate &candidate : sortedByPriority(candidates, tolerances, circlesAsPolygons)) {
        Choice choice{ candidate };
        const ZoneFences original = zoneFences(candidate.zone);
        const size_t hash = _fencesHash(original);
        for (auto it = shapes.constFind(hash); (it != shapes.cend()) && (it.key() == hash); ++it) {
            const Choice &earlier = choices[it.value()];
            if (zoneFences(earlier.candidate.zone) == original) {
                choice.duplicateOf = earlier.candidate.zone;
                break;
            }
        }
        if (!original.isEmpty() && !choice.duplicateOf) {
            shapes.insert(hash, choices.count());
        }

        choice.fences = zoneFences(candidate.zone, largest, circlesAsPolygons);
        choice.simplifyTolerance = largest;
        choice.usage.add(choice.fences);
        if (original.isEmpty()) {
            choice.status = Status::NoShape;
        } else if (choice.duplicateOf) {
            choice.status = Status::Duplicate;
        } else if (candidate.excluded) {
            choice.status = Status::Excluded;
        } else if (!capacity.exclusions) {
            choice.status = Status::Unsupported;
        } else if (capacity.fits(usage + choice.usage)) {
            choice.status = Status::Selected;
            usage += choice.usage;
        } else {
            choice.status = capacity.fits(reserved + choice.usage) ? Status::NoRoom : Status::TooLarge;
        }
        if (choice.status != Status::Selected) {
            choice.fences = ZoneFences();
        }
        choices.append(choice);
    }

    // The zones first in priority get the least simplified fences which fit
    for (qsizetype i = 0; i < choices.count(); i++) {
        Choice &choice = choices[i];
        if ((choice.status != Status::Selected) || (tolerances.count() < 2)) {
            continue;
        }
        Usage others = reserved;
        for (qsizetype j = 0; j < choices.count(); j++) {
            if ((j != i) && (choices[j].status == Status::Selected)) {
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
    for (const Choice &choice : std::as_const(choices)) {
        if (choice.status == Status::Selected) {
            selection.usage += choice.usage;
            selection.choices.append(choice);
        }
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
        return QCoreApplication::translate("GeoZoneFences", "Left out");
    case Status::NoRoom:
        return QCoreApplication::translate("GeoZoneFences", "No room left in the vehicle fence");
    case Status::TooLarge:
        return QCoreApplication::translate("GeoZoneFences", "Too many fence points (%1) even simplified").arg(choice.usage.items);
    case Status::Unsupported:
        return QCoreApplication::translate("GeoZoneFences", "The vehicle doesn't support exclusion fences");
    case Status::NoShape:
        return QCoreApplication::translate("GeoZoneFences", "No area to fence");
    case Status::Duplicate:
        return QCoreApplication::translate("GeoZoneFences", "Same area as %1").arg(choice.duplicateOf ? choice.duplicateOf->displayName() : QString());
    }
    return QString();
}

} // namespace GeoZoneFences
