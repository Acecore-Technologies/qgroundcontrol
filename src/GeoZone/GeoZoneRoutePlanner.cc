/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRoutePlanner.h"
#include "FactMetaData.h"
#include "FlightPathSegment.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneTerrain.h"
#include "MissionController.h"
#include "QmlObjectListModel.h"
#include "VisualMissionItem.h"

#include <QtCore/QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

double _cross(const QPointF &o, const QPointF &a, const QPointF &b)
{
    return ((a.x() - o.x()) * (b.y() - o.y())) - ((a.y() - o.y()) * (b.x() - o.x()));
}

/// @return Counterclockwise convex hull without collinear vertices
QList<QPointF> _convexHull(QList<QPointF> points)
{
    std::sort(points.begin(), points.end(), [](const QPointF &a, const QPointF &b) { return (a.x() != b.x()) ? (a.x() < b.x()) : (a.y() < b.y()); });
    if (points.count() < 3) {
        return points;
    }
    QList<QPointF> hull(points.count() * 2);
    qsizetype k = 0;
    for (const QPointF &point : std::as_const(points)) {
        while ((k >= 2) && (_cross(hull[k - 2], hull[k - 1], point) <= 0)) {
            k--;
        }
        hull[k++] = point;
    }
    for (qsizetype i = points.count() - 2, lower = k + 1; i >= 0; i--) {
        while ((k >= lower) && (_cross(hull[k - 2], hull[k - 1], points[i]) <= 0)) {
            k--;
        }
        hull[k++] = points[i];
    }
    hull.resize(k - 1);
    return hull;
}

/// @return Hull enclosing the convex polygon grown by the distance, its corners rounded with a few vertices
QList<QPointF> _grow(const QList<QPointF> &hull, double distance)
{
    constexpr int directions = 8;
    // Vertices of the polygons around the corners are outside the circle of the distance
    const double radius = distance / std::cos(M_PI / directions);
    QList<QPointF> points;
    for (const QPointF &vertex : hull) {
        for (int i = 0; i < directions; i++) {
            const double angle = (2 * M_PI * i) / directions;
            points.append(vertex + QPointF(radius * std::cos(angle), radius * std::sin(angle)));
        }
    }
    return _convexHull(points);
}

bool _properlyIntersect(const QPointF &a, const QPointF &b, const QPointF &c, const QPointF &d)
{
    constexpr double epsilon = 1e-9;
    const double o1 = _cross(a, b, c);
    const double o2 = _cross(a, b, d);
    const double o3 = _cross(c, d, a);
    const double o4 = _cross(c, d, b);
    return (((o1 > epsilon) && (o2 < -epsilon)) || ((o1 < -epsilon) && (o2 > epsilon))) &&
           (((o3 > epsilon) && (o4 < -epsilon)) || ((o3 < -epsilon) && (o4 > epsilon)));
}

/// @return true if the segment passes through the inside of the convex polygon, not only along its edges
bool _crossesInterior(const QPointF &p, const QPointF &q, const QList<QPointF> &polygon)
{
    for (qsizetype i = 0; i < polygon.count(); i++) {
        if (_properlyIntersect(p, q, polygon[i], polygon[(i + 1) % polygon.count()])) {
            return true;
        }
    }
    const QPointF middle = (p + q) / 2;
    return GeoZoneGeometry::ringContains(polygon, middle) && (GeoZoneGeometry::distanceToRing(middle, polygon) > 0.01);
}

double _length(const QPointF &a, const QPointF &b)
{
    return std::hypot(b.x() - a.x(), b.y() - a.y());
}

} // namespace

GeoZoneRoutePlanner::GeoZoneRoutePlanner(QObject *parent)
    : GeoZoneRoutePlanner(GeoZoneManager::instance(), parent)
{
}

GeoZoneRoutePlanner::GeoZoneRoutePlanner(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
{
    _updateTimer.setSingleShot(true);
    _updateTimer.setInterval(500);
    (void) connect(&_updateTimer, &QTimer::timeout, this, &GeoZoneRoutePlanner::update);

    // Suggestions refer to zones and mission items, so they are dropped when either changes and computed again
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, [this]() {
        if (!_suggestions.isEmpty()) {
            _suggestions.clear();
            emit suggestionsChanged();
        }
    });
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, [this]() {
        if (_active) {
            _updateTimer.start();
        }
    });
}

void GeoZoneRoutePlanner::setActive(bool active)
{
    if (active == _active) {
        return;
    }
    _active = active;
    emit activeChanged();
    if (_active) {
        update();
    } else {
        _updateTimer.stop();
    }
}

MissionController *GeoZoneRoutePlanner::missionController() const
{
    return _missionController;
}

void GeoZoneRoutePlanner::setMissionController(MissionController *missionController)
{
    if (missionController == _missionController) {
        return;
    }
    if (_missionController) {
        (void) disconnect(_missionController, nullptr, this, nullptr);
    }
    _missionController = missionController;
    if (_missionController) {
        const auto missionChanged = [this]() {
            if (_active) {
                _updateTimer.start();
            }
        };
        (void) connect(_missionController, &MissionController::visualItemsChanged, this, missionChanged);
        (void) connect(_missionController, &MissionController::recalcTerrainProfile, this, missionChanged);
    }
    _suggestions.clear();
    emit missionControllerChanged();
    emit suggestionsChanged();
    if (_active) {
        _updateTimer.start();
    }
}

QVariantList GeoZoneRoutePlanner::suggestions() const
{
    QVariantList suggestions;
    for (const Suggestion &suggestion : _suggestions) {
        suggestions.append(QVariantMap{
            { QStringLiteral("text"), suggestion.text },
            { QStringLiteral("canApply"), suggestion.insertIndex >= 0 },
        });
    }
    return suggestions;
}

std::optional<QList<QGeoCoordinate>> GeoZoneRoutePlanner::detourAround(const QGeoCoordinate &from, const QGeoCoordinate &to, const QList<const GeoZone*> &zones,
                                                                       double margin)
{
    const GeoZoneGeometry::LocalProjection projection(from);
    QList<QPointF> zonePoints;
    for (const GeoZone *zone : zones) {
        for (const QList<QGeoCoordinate> &polygon : zone->exclusionPolygons()) {
            zonePoints.append(projection.toLocal(polygon));
        }
        for (const QGeoCircle &circle : zone->exclusionCircles()) {
            constexpr int circlePoints = 24;
            const QPointF center = projection.toLocal(circle.center());
            const double radius = circle.radius() / std::cos(M_PI / circlePoints);
            for (int i = 0; i < circlePoints; i++) {
                const double angle = (2 * M_PI * i) / circlePoints;
                zonePoints.append(center + QPointF(radius * std::cos(angle), radius * std::sin(angle)));
            }
        }
    }
    const QList<QPointF> zoneHull = _convexHull(zonePoints);
    if (zoneHull.count() < 3) {
        return std::nullopt;
    }

    // The path follows the hull grown by the margin, then is straightened where it keeps half the margin
    const QList<QPointF> obstacle = _grow(zoneHull, margin);
    const QList<QPointF> clearance = _grow(zoneHull, margin / 2);
    const QPointF start = projection.toLocal(from);
    const QPointF end = projection.toLocal(to);
    if (GeoZoneGeometry::ringContains(obstacle, start) || GeoZoneGeometry::ringContains(obstacle, end)) {
        return std::nullopt;
    }
    if (!_crossesInterior(start, end, obstacle)) {
        return QList<QGeoCoordinate>();
    }

    // Shortest path from start (0) to end (1) through the hull vertices
    QList<QPointF> nodes = { start, end };
    nodes.append(obstacle);
    QList<double> distances(nodes.count(), std::numeric_limits<double>::infinity());
    QList<qsizetype> previous(nodes.count(), -1);
    QList<bool> done(nodes.count(), false);
    distances[0] = 0;
    for (qsizetype round = 0; round < nodes.count(); round++) {
        qsizetype current = -1;
        for (qsizetype i = 0; i < nodes.count(); i++) {
            if (!done[i] && ((current < 0) || (distances[i] < distances[current]))) {
                current = i;
            }
        }
        if ((current < 0) || std::isinf(distances[current]) || (current == 1)) {
            break;
        }
        done[current] = true;
        for (qsizetype next = 0; next < nodes.count(); next++) {
            if (done[next] || _crossesInterior(nodes[current], nodes[next], obstacle)) {
                continue;
            }
            const double distance = distances[current] + _length(nodes[current], nodes[next]);
            if (distance < distances[next]) {
                distances[next] = distance;
                previous[next] = current;
            }
        }
    }
    if (previous[1] < 0) {
        return std::nullopt;
    }

    QList<QPointF> path;
    for (qsizetype node = 1; node >= 0; node = previous[node]) {
        path.prepend(nodes[node]);
    }

    // Fewer waypoints: skip vertices while the straight line keeps half the margin
    QList<QPointF> straightened = { path.constFirst() };
    for (qsizetype i = 0; i < (path.count() - 1);) {
        qsizetype farthest = i + 1;
        for (qsizetype j = path.count() - 1; j > (i + 1); j--) {
            if (!_crossesInterior(path[i], path[j], clearance)) {
                farthest = j;
                break;
            }
        }
        straightened.append(path[farthest]);
        i = farthest;
    }

    QList<QGeoCoordinate> waypoints;
    for (qsizetype i = 1; i < (straightened.count() - 1); i++) {
        waypoints.append(projection.toGeo(straightened[i]));
    }
    return waypoints;
}

QList<GeoZone*> GeoZoneRoutePlanner::_zonesOnPath(const QList<GeoZone::Position> &path) const
{
    const auto [start, end] = GeoZoneMissionCheck::plannedFlightTimes(_manager, _missionController);
    QList<GeoZone*> zones;
    for (const GeoZoneCatalog::PathHit &hit : _manager->catalog().pathHits(path, GeoZonePolicy::routing(start, end))) {
        zones.append(hit.zone);
    }
    return zones;
}

void GeoZoneRoutePlanner::update()
{
    _updateTimer.stop();
    QList<Suggestion> suggestions;
    const QmlObjectListModel *const visualItems = _missionController ? _missionController->visualItems() : nullptr;

    QList<int> coordinateItems;
    for (int i = 0; visualItems && (i < visualItems->count()); i++) {
        const VisualMissionItem *const item = visualItems->value<const VisualMissionItem*>(i);
        if (item && item->specifiesCoordinate() && !item->isStandaloneCoordinate()) {
            coordinateItems.append(i);
        }
    }

    for (qsizetype k = 1; k < coordinateItems.count(); k++) {
        VisualMissionItem *const fromItem = visualItems->value<VisualMissionItem*>(coordinateItems[k - 1]);
        const VisualMissionItem *const toItem = visualItems->value<const VisualMissionItem*>(coordinateItems[k]);
        const QGeoCoordinate from = fromItem->exitCoordinate();
        const QGeoCoordinate to = toItem->coordinate();
        if (!from.isValid() || !to.isValid() || (from.distanceTo(to) < 1)) {
            continue;
        }

        // Altitudes and terrain of the leg from its flight path segment
        GeoZoneMissionCheck::Segment segment;
        segment.from = from;
        segment.to = to;
        double legTerrain = qQNaN();
        const FlightPathSegment *const flightPathSegment = fromItem->simpleFlightPathSegment();
        if (flightPathSegment && (flightPathSegment->coordinate2().distanceTo(to) < 1)) {
            segment.fromAltitudeAMSL = flightPathSegment->coord1AMSLAlt();
            segment.toAltitudeAMSL = flightPathSegment->coord2AMSLAlt();
            double terrainSum = 0;
            for (const QVariant &height : flightPathSegment->amslTerrainHeights()) {
                segment.terrainAMSL.append(height.toDouble());
                terrainSum += height.toDouble();
            }
            segment.terrainSpacing = flightPathSegment->distanceBetween();
            if (!segment.terrainAMSL.isEmpty()) {
                legTerrain = terrainSum / segment.terrainAMSL.count();
            }
        }
        const QList<QList<GeoZone::Position>> legPaths = GeoZoneMissionCheck::pathsFromSegments({ segment });
        QList<GeoZone*> crossed;
        for (const QList<GeoZone::Position> &path : legPaths) {
            for (GeoZone *zone : _zonesOnPath(path)) {
                if (!crossed.contains(zone)) {
                    crossed.append(zone);
                }
            }
        }
        if (crossed.isEmpty()) {
            continue;
        }

        const QString leg = tr("Leg to item %1").arg(toItem->sequenceNumber());

        // Zones which don't reach the ground can be flown under
        for (const GeoZone *zone : std::as_const(crossed)) {
            const QList<GeoZone::Volume> &volumes = zone->data().volumes;
            const bool raised = std::all_of(volumes.cbegin(), volumes.cend(), [](const GeoZone::Volume &volume) {
                return !std::isnan(volume.lowerLimit) && (volume.lowerLimit > 0) && (volume.lowerReference != GeoZone::VerticalReference::Unknown);
            });
            if (raised) {
                suggestions.append(Suggestion{ tr("%1: fly below the floor of %2 (%3).").arg(leg, zone->displayName(), zone->verticalLimitsText()) });
            }
        }

        // Detours entering other zones avoid those too
        QList<const GeoZone*> avoided(crossed.cbegin(), crossed.cend());
        QList<QGeoCoordinate> waypoints;
        bool found = false;
        for (int round = 0; round < maxDetourRounds; round++) {
            const std::optional<QList<QGeoCoordinate>> detour = detourAround(from, to, avoided);
            if (!detour) {
                break;
            }

            QList<QGeoCoordinate> points = { from };
            points.append(*detour);
            points.append(to);
            double totalLength = 0;
            for (qsizetype i = 1; i < points.count(); i++) {
                totalLength += points[i - 1].distanceTo(points[i]);
            }
            QList<GeoZoneMissionCheck::Segment> detourSegments;
            double along = 0;
            const auto altitudeAt = [&segment, totalLength](double distance) {
                return segment.fromAltitudeAMSL + ((segment.toAltitudeAMSL - segment.fromAltitudeAMSL) * (distance / totalLength));
            };
            for (qsizetype i = 1; i < points.count(); i++) {
                GeoZoneMissionCheck::Segment detourSegment;
                detourSegment.from = points[i - 1];
                detourSegment.to = points[i];
                detourSegment.fromAltitudeAMSL = altitudeAt(along);
                along += points[i - 1].distanceTo(points[i]);
                detourSegment.toAltitudeAMSL = altitudeAt(along);
                detourSegments.append(detourSegment);
            }
            QList<GeoZone*> entered;
            for (const QList<GeoZone::Position> &detourPath : GeoZoneMissionCheck::pathsFromSegments(detourSegments)) {
                // The terrain tiles give the terrain along the detour where they are loaded, the leg's is close elsewhere
                QList<GeoZone::Position> path = GeoZoneTerrain::withTerrain(detourPath);
                for (GeoZone::Position &position : path) {
                    if (std::isnan(position.terrainAMSL)) {
                        position.terrainAMSL = legTerrain;
                    }
                }
                for (GeoZone *zone : _zonesOnPath(path)) {
                    if (!avoided.contains(zone) && !entered.contains(zone)) {
                        entered.append(zone);
                    }
                }
            }
            if (entered.isEmpty()) {
                waypoints = *detour;
                found = true;
                break;
            }
            avoided.append(QList<const GeoZone*>(entered.cbegin(), entered.cend()));
        }

        QStringList names;
        for (const GeoZone *zone : std::as_const(crossed)) {
            names.append(zone->displayName());
        }
        if (found && !waypoints.isEmpty()) {
            double detourLength = from.distanceTo(waypoints.constFirst()) + waypoints.constLast().distanceTo(to);
            for (qsizetype i = 1; i < waypoints.count(); i++) {
                detourLength += waypoints[i - 1].distanceTo(waypoints[i]);
            }
            const double added = detourLength - from.distanceTo(to);
            const QString addedText = QStringLiteral("%1 %2").arg(qRound(FactMetaData::metersToAppSettingsHorizontalDistanceUnits(added).toDouble()))
                                                         .arg(FactMetaData::appSettingsHorizontalDistanceUnitsString());
            suggestions.append(Suggestion{ tr("%1: detour around %2, %n waypoint(s) adding %3.", nullptr, static_cast<int>(waypoints.count()))
                                               .arg(leg, names.join(QStringLiteral(", ")), addedText),
                                           coordinateItems[k], waypoints });
        } else {
            suggestions.append(Suggestion{ tr("%1: no detour around %2 was found, a waypoint is within %3 m of a zone or the zones surround it.")
                                               .arg(leg, names.join(QStringLiteral(", "))).arg(detourMargin) });
        }
    }

    _suggestions = suggestions;
    emit suggestionsChanged();
}

bool GeoZoneRoutePlanner::applySuggestion(int index)
{
    if (!_missionController || (index < 0) || (index >= _suggestions.count()) || (_suggestions[index].insertIndex < 0)) {
        return false;
    }
    const Suggestion suggestion = _suggestions[index];
    // The new waypoints take the altitude of the previous item
    for (qsizetype i = 0; i < suggestion.waypoints.count(); i++) {
        (void) _missionController->insertSimpleMissionItem(suggestion.waypoints[i], suggestion.insertIndex + static_cast<int>(i));
    }
    update();
    return true;
}
