/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneCatalog.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"

#include <algorithm>
#include <map>
#include <numeric>

void GeoZoneCatalog::rebuild(const QList<const GeoZoneSource*> &sources)
{
    _zones.clear();
    _shapes.clear();
    _boundingBox = QGeoRectangle();
    for (const GeoZoneSource *source : sources) {
        _zones.append(source->zones());
        _shapes.append(source->shapes());
        if (source->boundingBox().isValid()) {
            _boundingBox = _boundingBox.isValid() ? _boundingBox.united(source->boundingBox()) : source->boundingBox();
        }
    }

    QHash<const GeoZone*, qsizetype> zoneIndexes;
    zoneIndexes.reserve(_zones.count());
    for (qsizetype i = 0; i < _zones.count(); i++) {
        zoneIndexes.insert(_zones[i], i);
    }
    _zoneShapes = QList<QList<const GeoZoneShape*>>(_zones.count());
    for (const GeoZoneShape *shape : std::as_const(_shapes)) {
        _zoneShapes[zoneIndexes.value(shape->zone())].append(shape);
    }
    _index.build(_zones);
}

QList<const GeoZoneShape*> GeoZoneCatalog::shapesInArea(double south, double west, double north, double east) const
{
    QList<const GeoZoneShape*> shapes;
    for (const int index : _index.query(south, west, north, east)) {
        shapes.append(_zoneShapes[index]);
    }
    return shapes;
}

QList<GeoZone*> GeoZoneCatalog::labelZones(const QGeoRectangle &region, int maxCount) const
{
    QList<GeoZone*> candidates;
    for (const int index : _index.query(region)) {
        GeoZone *const zone = _zones[index];
        if (region.contains(zone->center())) {
            candidates.append(zone);
        }
    }

    if (candidates.count() > maxCount) {
        std::stable_sort(candidates.begin(), candidates.end(), [](const GeoZone *a, const GeoZone *b) {
            return GeoZone::restrictionSeverity(a->restriction()) < GeoZone::restrictionSeverity(b->restriction());
        });
        candidates.resize(maxCount);
    }

    return candidates;
}

QList<GeoZone*> GeoZoneCatalog::zonesAt(const QGeoCoordinate &coordinate) const
{
    QList<GeoZone*> zones;
    if (!coordinate.isValid()) {
        return {};
    }
    for (const int index : _index.query(coordinate.latitude(), coordinate.longitude(), coordinate.latitude(), coordinate.longitude())) {
        GeoZone *const zone = _zones[index];
        for (qsizetype i = 0; i < zone->data().volumes.count(); i++) {
            if (zone->volumeContains(i, coordinate)) {
                zones.append(zone);
                break;
            }
        }
    }
    std::stable_sort(zones.begin(), zones.end(), [](const GeoZone *a, const GeoZone *b) {
        if (a->active() != b->active()) {
            return a->active();
        }
        return GeoZone::restrictionSeverity(a->restriction()) < GeoZone::restrictionSeverity(b->restriction());
    });
    return zones;
}

QList<GeoZone*> GeoZoneCatalog::search(const QString &text, int maxResults) const
{
    const QString query = text.trimmed();
    if (query.isEmpty() || (maxResults <= 0)) {
        return {};
    }

    struct Match {
        GeoZone *zone;
        bool prefix;
    };
    QList<Match> matches;
    for (GeoZone *zone : _zones) {
        const QString name = zone->displayName();
        if (name.startsWith(query, Qt::CaseInsensitive) || zone->identifier().startsWith(query, Qt::CaseInsensitive)) {
            matches.append({ zone, true });
        } else if (name.contains(query, Qt::CaseInsensitive) || zone->identifier().contains(query, Qt::CaseInsensitive)) {
            matches.append({ zone, false });
        }
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        if (a.prefix != b.prefix) {
            return a.prefix;
        }
        const int severityA = GeoZone::restrictionSeverity(a.zone->restriction());
        const int severityB = GeoZone::restrictionSeverity(b.zone->restriction());
        if (severityA != severityB) {
            return severityA < severityB;
        }
        return a.zone->displayName().compare(b.zone->displayName(), Qt::CaseInsensitive) < 0;
    });

    QList<GeoZone*> zones;
    for (qsizetype i = 0; i < (std::min)(matches.count(), qsizetype{ maxResults }); i++) {
        zones.append(matches[i].zone);
    }
    return zones;
}

QList<GeoZone*> GeoZoneCatalog::zonesInArea(const QGeoRectangle &area, const GeoZonePolicy &policy) const
{
    QList<int> candidates;
    if (area.isValid()) {
        candidates = _index.query(area);
    } else {
        candidates.resize(_zones.count());
        std::iota(candidates.begin(), candidates.end(), 0);
    }

    QList<GeoZone*> zones;
    for (const int index : std::as_const(candidates)) {
        GeoZone *const zone = _zones[index];
        if (policy.accepts(zone)) {
            zones.append(zone);
        }
    }
    return zones;
}

QList<GeoZoneCatalog::PathHit> GeoZoneCatalog::pathHits(const QList<QList<GeoZone::Position>> &paths, const GeoZonePolicy &policy) const
{
    // Candidates near any segment, checked once per path
    std::map<int, QList<int>> candidatePaths;
    for (int pathIndex = 0; pathIndex < paths.count(); pathIndex++) {
        const QList<GeoZone::Position> &path = paths[pathIndex];
        for (qsizetype i = 0; i < path.count(); i++) {
            const QGeoCoordinate &from = path[(std::max)(i - 1, qsizetype{ 0 })].coordinate;
            const QGeoCoordinate &to = path[i].coordinate;
            if (!from.isValid() || !to.isValid()) {
                continue;
            }
            const QList<int> candidates = _index.query((std::min)(from.latitude(), to.latitude()), (std::min)(from.longitude(), to.longitude()),
                                                       (std::max)(from.latitude(), to.latitude()), (std::max)(from.longitude(), to.longitude()));
            for (const int index : candidates) {
                QList<int> &candidatePathList = candidatePaths[index];
                if (candidatePathList.isEmpty() || (candidatePathList.last() != pathIndex)) {
                    candidatePathList.append(pathIndex);
                }
            }
        }
    }

    // Meters from the start of each path to each of its positions
    QList<QList<double>> pathDistances(paths.count());
    const auto distanceAlong = [&paths, &pathDistances](int pathIndex, const GeoZone::PathEntry &entry) {
        QList<double> &distances = pathDistances[pathIndex];
        const QList<GeoZone::Position> &path = paths[pathIndex];
        if (distances.isEmpty()) {
            distances.reserve(path.count());
            double distance = 0;
            for (qsizetype i = 0; i < path.count(); i++) {
                if ((i > 0) && path[i - 1].coordinate.isValid() && path[i].coordinate.isValid()) {
                    distance += path[i - 1].coordinate.distanceTo(path[i].coordinate);
                }
                distances.append(distance);
            }
        }
        const double start = distances[entry.segment];
        const double end = distances[(std::min)(entry.segment + 1, path.count() - 1)];
        return start + ((end - start) * entry.fraction);
    };

    QList<PathHit> hits;
    for (auto it = candidatePaths.cbegin(); it != candidatePaths.cend(); ++it) {
        GeoZone *const zone = _zones[it->first];
        if (!policy.accepts(zone)) {
            continue;
        }
        for (const int pathIndex : it->second) {
            if (const auto entry = zone->pathEntry(paths[pathIndex])) {
                hits.append(PathHit{ zone, pathIndex, *entry, distanceAlong(pathIndex, *entry) });
                break;
            }
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const PathHit &a, const PathHit &b) {
        return (a.path != b.path) ? (a.path < b.path) : (a.distance < b.distance);
    });
    return hits;
}

QList<GeoZone*> GeoZoneCatalog::zonesIntersectingAreas(const QList<GeoZone::Area> &areas, const GeoZonePolicy &policy) const
{
    QList<GeoZone*> zones;
    for (const GeoZone::Area &area : areas) {
        if (area.polygon.count() < 3) {
            continue;
        }
        for (const int index : _index.query(QGeoRectangle(area.polygon))) {
            GeoZone *const zone = _zones[index];
            if (policy.accepts(zone) && !zones.contains(zone) && zone->intersectsArea(area)) {
                zones.append(zone);
            }
        }
    }
    return zones;
}
