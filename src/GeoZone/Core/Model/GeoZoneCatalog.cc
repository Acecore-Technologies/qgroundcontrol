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
#include <numeric>

void GeoZoneCatalog::rebuild(const QList<const GeoZoneSource*> &sources)
{
    const QSharedPointer<Data> data = QSharedPointer<Data>::create();
    for (const GeoZoneSource *source : sources) {
        data->zones.append(source->zones());
        data->sharedZones.append(source->sharedZones());
        data->shapes.append(source->shapes());
        data->boundingBox = data->boundingBox.united(source->box());
    }

    QHash<const GeoZone*, qsizetype> zoneIndexes;
    zoneIndexes.reserve(data->zones.count());
    for (qsizetype i = 0; i < data->zones.count(); i++) {
        zoneIndexes.insert(data->zones[i], i);
    }
    data->zoneShapes = QList<QList<const GeoZoneShape*>>(data->zones.count());
    for (const GeoZoneShape *shape : std::as_const(data->shapes)) {
        data->zoneShapes[zoneIndexes.value(shape->zone())].append(shape);
    }
    data->index.build(data->zones);
    _data = data;
}

QList<const GeoZoneShape*> GeoZoneCatalog::shapesInArea(double south, double west, double north, double east) const
{
    QList<const GeoZoneShape*> shapes;
    for (const int index : _data->index.query(south, west, north, east)) {
        shapes.append(_data->zoneShapes[index]);
    }
    return shapes;
}

QList<const GeoZone*> GeoZoneCatalog::labelZones(const QGeoRectangle &region, int maxCount) const
{
    QList<const GeoZone*> candidates;
    for (const int index : _data->index.query(region)) {
        const GeoZone *const zone = _data->zones[index];
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

QList<const GeoZone*> GeoZoneCatalog::zonesAt(const QGeoCoordinate &coordinate) const
{
    QList<const GeoZone*> zones;
    if (!coordinate.isValid()) {
        return {};
    }
    for (const int index : _data->index.query(coordinate.latitude(), coordinate.longitude(), coordinate.latitude(), coordinate.longitude())) {
        const GeoZone *const zone = _data->zones[index];
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

QList<const GeoZone*> GeoZoneCatalog::search(const QString &text, int maxResults) const
{
    const QString query = text.trimmed();
    if (query.isEmpty() || (maxResults <= 0)) {
        return {};
    }

    struct Match {
        const GeoZone *zone;
        bool prefix;
    };
    QList<Match> matches;
    for (const GeoZone *zone : _data->zones) {
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

    QList<const GeoZone*> zones;
    for (qsizetype i = 0; i < (std::min)(matches.count(), qsizetype{ maxResults }); i++) {
        zones.append(matches[i].zone);
    }
    return zones;
}

QList<const GeoZone*> GeoZoneCatalog::zonesInArea(const GeoZoneBox &area, const GeoZonePolicy &policy) const
{
    QList<int> candidates;
    if (area.isValid()) {
        candidates = _data->index.query(area);
    } else {
        candidates.resize(_data->zones.count());
        std::iota(candidates.begin(), candidates.end(), 0);
    }

    QList<const GeoZone*> zones;
    for (const int index : std::as_const(candidates)) {
        const GeoZone *const zone = _data->zones[index];
        if (policy.accepts(zone)) {
            zones.append(zone);
        }
    }
    return zones;
}

QList<const GeoZone*> GeoZoneCatalog::zonesIntersectingAreas(const QList<GeoZone::Area> &areas, const GeoZonePolicy &policy) const
{
    QList<const GeoZone*> zones;
    for (const GeoZone::Area &area : areas) {
        if (area.polygon.count() < 3) {
            continue;
        }
        // Edges go the shorter way around, which may cross the antimeridian, like those of the zones
        for (const int index : _data->index.query(GeoZoneBox::fromPath(area.polygon))) {
            const GeoZone *const zone = _data->zones[index];
            if (policy.accepts(zone) && !zones.contains(zone) && zone->intersectsArea(area)) {
                zones.append(zone);
            }
        }
    }
    return zones;
}
