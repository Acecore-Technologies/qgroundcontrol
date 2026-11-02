/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneCatalog.h"
#include "GeoZoneDataStream.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"

#include <QtCore/QHash>
#include <QtCore/QSet>

#include <algorithm>
#include <numeric>

namespace {

/// Restriction, volumes and schedule of a zone, as serialized
QByteArray contentBytes(const GeoZone &zone)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << static_cast<quint8>(zone.restriction());
    GeoZoneDataStream::writeList(stream, zone.data().volumes);
    GeoZoneDataStream::writeList(stream, zone.data().applicability);
    return bytes;
}

} // namespace

bool GeoZoneCatalog::sameContent(const GeoZone &a, const GeoZone &b)
{
    if ((a.identifier() != b.identifier()) || (a.country() != b.country()) || (a.restriction() != b.restriction()) ||
        (a.data().volumes.count() != b.data().volumes.count())) {
        return false;
    }
    // The same published feature reads the same
    const QByteArray &hashA = a.data().contentHash;
    if (!hashA.isEmpty() && (hashA == b.data().contentHash)) {
        return true;
    }
    return contentBytes(a) == contentBytes(b);
}

void GeoZoneCatalog::rebuild(const QList<const GeoZoneSource*> &sources)
{
    const QSharedPointer<Data> data = QSharedPointer<Data>::create();

    // Zones kept with an identifier, by identifier and country, to find those later sources publish again
    QHash<QString, QList<GeoZone*>> identified;
    QHash<GeoZone*, QList<GeoZone::OtherSource>> otherSources;
    for (const GeoZoneSource *source : sources) {
        QSet<const GeoZone*> left;
        for (GeoZone *zone : source->zones()) {
            zone->setOtherSources({});
            if ((sources.count() > 1) && !zone->identifier().isEmpty()) {
                QList<GeoZone*> &kept = identified[zone->identifier() + QChar(0x1f) + zone->country()];
                const auto same = std::find_if(kept.cbegin(), kept.cend(), [zone](const GeoZone *keptZone) {
                    return (keptZone->sourceKey() != zone->sourceKey()) && sameContent(*keptZone, *zone);
                });
                if (same != kept.cend()) {
                    QList<GeoZone::OtherSource> &others = otherSources[*same];
                    if (!std::any_of(others.cbegin(), others.cend(), [zone](const GeoZone::OtherSource &other) { return other.key == zone->sourceKey(); })) {
                        others.append(GeoZone::OtherSource{ zone->sourceKey(), zone->sourceText() });
                    }
                    data->merged.emplace_back(zone, *same);
                    left.insert(zone);
                    continue;
                }
                kept.append(zone);
            }
            data->zones.append(zone);
        }
        // Zones left out are kept alive too, so that values referring to them stay valid
        data->sharedZones.append(source->sharedZones());
        if (left.isEmpty()) {
            data->shapes.append(source->shapes());
        } else {
            for (GeoZoneShape *shape : source->shapes()) {
                if (!left.contains(shape->zone())) {
                    data->shapes.append(shape);
                }
            }
        }
        data->boundingBox = data->boundingBox.united(source->box());
    }
    for (auto it = otherSources.cbegin(); it != otherSources.cend(); ++it) {
        it.key()->setOtherSources(it.value());
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

QHash<QString, int> GeoZoneCatalog::sharedZoneCounts(const QString &sourceKey) const
{
    QHash<QString, int> counts;
    for (const auto &[leftZone, keptZone] : _data->merged) {
        if (leftZone->sourceKey() == sourceKey) {
            counts[keptZone->sourceKey()]++;
        } else if (keptZone->sourceKey() == sourceKey) {
            counts[leftZone->sourceKey()]++;
        }
    }
    return counts;
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

QString GeoZoneCatalog::searchKey(const QString &text)
{
    const bool ascii = std::all_of(text.cbegin(), text.cend(), [](QChar c) { return c.unicode() < 0x80; });
    if (ascii) {
        return text.toCaseFolded();
    }
    // Letters which don't decompose into a base letter and marks
    static const QHash<QChar, QString> letters = {
        { QChar(0x00DF), QStringLiteral("ss") }, { QChar(0x00E6), QStringLiteral("ae") }, { QChar(0x00C6), QStringLiteral("ae") },
        { QChar(0x00F8), QStringLiteral("o") }, { QChar(0x00D8), QStringLiteral("o") }, { QChar(0x0153), QStringLiteral("oe") },
        { QChar(0x0152), QStringLiteral("oe") }, { QChar(0x0142), QStringLiteral("l") }, { QChar(0x0141), QStringLiteral("l") },
        { QChar(0x0111), QStringLiteral("d") }, { QChar(0x0110), QStringLiteral("d") }, { QChar(0x00F0), QStringLiteral("d") },
        { QChar(0x00D0), QStringLiteral("d") }, { QChar(0x00FE), QStringLiteral("th") }, { QChar(0x00DE), QStringLiteral("th") },
        { QChar(0x0131), QStringLiteral("i") },
    };
    const QString decomposed = text.normalized(QString::NormalizationForm_D);
    QString key;
    key.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing) {
            continue;
        }
        const auto letter = letters.constFind(c);
        if (letter != letters.cend()) {
            key += letter.value();
        } else {
            key += c;
        }
    }
    return key.toCaseFolded();
}

QList<const GeoZone*> GeoZoneCatalog::search(const QString &text, int maxResults) const
{
    const QString query = searchKey(text.trimmed());
    if (query.isEmpty() || (maxResults <= 0)) {
        return {};
    }

    struct Match {
        const GeoZone *zone;
        bool prefix;
    };
    QList<Match> matches;
    // Zones overlapping sources publish alike are already one zone, see rebuild
    for (const GeoZone *zone : _data->zones) {
        const QString name = searchKey(zone->displayName());
        const QString identifier = searchKey(zone->identifier());
        const bool prefix = name.startsWith(query) || identifier.startsWith(query);
        if (!prefix && !name.contains(query) && !identifier.contains(query)) {
            continue;
        }
        matches.append({ zone, prefix });
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
