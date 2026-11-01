/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePlanAudit.h"
#include "GeoZoneCatalog.h"

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QSet>

#include <algorithm>

namespace {

constexpr const char *versionKey = "version";
constexpr const char *savedAtKey = "saved";
constexpr const char *sourcesKey = "sources";
constexpr const char *zonesKey = "zones";

QString countryIdentifier(const QString &country, const QString &identifier)
{
    return identifier.isEmpty() ? QString() : (country + QChar(u'/') + identifier);
}

QString dateText(const QDateTime &dateTime)
{
    return dateTime.isValid() ? dateTime.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'")) : GeoZonePlanAudit::tr("unknown date");
}

} // namespace

GeoZonePlanAudit::Zone GeoZonePlanAudit::Zone::fromZone(const GeoZone *zone)
{
    Zone record;
    record.key = zone->key();
    record.sourceKey = zone->sourceKey();
    record.identifier = zone->identifier();
    record.country = zone->country();
    record.name = zone->name();
    record.restriction = zone->restriction();
    record.contentHash = zone->data().contentHash;
    return record;
}

GeoZonePlanAudit::Record GeoZonePlanAudit::Record::make(const QList<const GeoZone*> &zones, const QList<GeoZoneSourceVersion> &sources, const QDateTime &savedAt)
{
    Record record;
    record.savedAt = savedAt.toUTC();
    record.sources = sources;
    QSet<QString> keys;
    for (const GeoZone *zone : zones) {
        if (zone && !keys.contains(zone->key())) {
            keys.insert(zone->key());
            record.zones.append(Zone::fromZone(zone));
        }
    }
    return record;
}

QJsonObject GeoZonePlanAudit::Record::toJson() const
{
    QJsonArray sourceArray;
    for (const GeoZoneSourceVersion &source : sources) {
        sourceArray.append(source.toJson());
    }
    QJsonArray zoneArray;
    for (const Zone &zone : zones) {
        QJsonObject json;
        json.insert(QStringLiteral("key"), zone.key);
        json.insert(QStringLiteral("source"), zone.sourceKey);
        json.insert(QStringLiteral("identifier"), zone.identifier);
        json.insert(QStringLiteral("country"), zone.country);
        json.insert(QStringLiteral("name"), zone.name);
        json.insert(QStringLiteral("restriction"), static_cast<int>(zone.restriction));
        json.insert(QStringLiteral("contentHash"), QString::fromLatin1(zone.contentHash.toHex()));
        zoneArray.append(json);
    }
    QJsonObject json;
    json.insert(versionKey, version);
    json.insert(savedAtKey, savedAt.toUTC().toString(Qt::ISODate));
    json.insert(sourcesKey, sourceArray);
    json.insert(zonesKey, zoneArray);
    return json;
}

std::optional<GeoZonePlanAudit::Record> GeoZonePlanAudit::Record::fromJson(const QJsonObject &json)
{
    if (json.value(versionKey).toInt() < 1) {
        return std::nullopt;
    }
    Record record;
    record.savedAt = QDateTime::fromString(json.value(savedAtKey).toString(), Qt::ISODate).toUTC();
    for (const QJsonValue &value : json.value(sourcesKey).toArray()) {
        const GeoZoneSourceVersion source = GeoZoneSourceVersion::fromJson(value.toObject());
        if (!source.key.isEmpty()) {
            record.sources.append(source);
        }
    }
    for (const QJsonValue &value : json.value(zonesKey).toArray()) {
        const QJsonObject object = value.toObject();
        Zone zone;
        zone.key = object.value(QStringLiteral("key")).toString();
        zone.sourceKey = object.value(QStringLiteral("source")).toString();
        zone.identifier = object.value(QStringLiteral("identifier")).toString();
        zone.country = object.value(QStringLiteral("country")).toString();
        zone.name = object.value(QStringLiteral("name")).toString();
        const int restriction = object.value(QStringLiteral("restriction")).toInt(GeoZone::Unknown);
        zone.restriction = ((restriction >= GeoZone::Prohibited) && (restriction <= GeoZone::Unknown)) ? static_cast<GeoZone::Restriction>(restriction) : GeoZone::Unknown;
        zone.contentHash = QByteArray::fromHex(object.value(QStringLiteral("contentHash")).toString().toLatin1());
        if (!zone.key.isEmpty()) {
            record.zones.append(zone);
        }
    }
    return record;
}

GeoZonePlanAudit::Changes GeoZonePlanAudit::compare(const Record &record, const QList<const GeoZone*> &relevantZones, const GeoZoneCatalog &catalog,
                                                    const QList<GeoZoneSourceVersion> &sources)
{
    // The zones of the record by key, then by country and identifier, which stay when a zone is renamed or moved
    QHash<QString, const GeoZone*> byKey;
    QHash<QString, QList<const GeoZone*>> byIdentifier;
    QSet<QString> savedIdentifiers;
    for (const Zone &zone : record.zones) {
        const QString identifier = countryIdentifier(zone.country, zone.identifier);
        if (!identifier.isEmpty()) {
            savedIdentifiers.insert(identifier);
        }
    }
    QSet<QString> savedKeys;
    for (const Zone &zone : record.zones) {
        savedKeys.insert(zone.key);
    }
    const auto index = [&](const GeoZone *zone) {
        if (savedKeys.contains(zone->key()) && !byKey.contains(zone->key())) {
            byKey.insert(zone->key(), zone);
        }
        const QString identifier = countryIdentifier(zone->country(), zone->identifier());
        if (!identifier.isEmpty() && savedIdentifiers.contains(identifier)) {
            byIdentifier[identifier].append(zone);
        }
    };
    for (const GeoZone *zone : relevantZones) {
        index(zone);
    }
    bool allFound = true;
    for (const Zone &zone : record.zones) {
        allFound = allFound && byKey.contains(zone.key);
    }
    if (!allFound) {
        QSet<const GeoZone*> relevant(relevantZones.cbegin(), relevantZones.cend());
        for (const GeoZone *zone : catalog.zones()) {
            if (!relevant.contains(zone)) {
                index(zone);
            }
        }
    }

    Changes changes;
    QSet<const GeoZone*> matched;
    for (const Zone &saved : record.zones) {
        const GeoZone *current = byKey.value(saved.key);
        if (!current) {
            // Renamed, reclassified or moved to another source, preferring the same source
            const QList<const GeoZone*> candidates = byIdentifier.value(countryIdentifier(saved.country, saved.identifier));
            for (const GeoZone *candidate : candidates) {
                if (!matched.contains(candidate) && (!current || ((candidate->sourceKey() == saved.sourceKey) && (current->sourceKey() != saved.sourceKey)))) {
                    current = candidate;
                }
            }
        }
        if (!current) {
            changes.removed.append(ZoneChange{ saved, Zone() });
            continue;
        }
        matched.insert(current);
        const Zone now = Zone::fromZone(current);
        const bool hashed = !saved.contentHash.isEmpty() && !now.contentHash.isEmpty();
        if ((hashed && (saved.contentHash != now.contentHash)) || (saved.name != now.name) || (saved.restriction != now.restriction)) {
            changes.modified.append(ZoneChange{ saved, now });
        }
    }
    QHash<QString, GeoZoneSourceVersion> currentSources;
    for (const GeoZoneSourceVersion &source : sources) {
        currentSources.insert(source.key, source);
    }
    QHash<QString, GeoZoneSourceVersion> savedSources;
    for (const GeoZoneSourceVersion &source : record.sources) {
        savedSources.insert(source.key, source);
    }

    // Zones the mission meets which weren't in the data when it was saved, rather than which it meets after edits:
    // those of sources updated since, or loaded since without covering the mission when saved
    QSet<const GeoZone*> added;
    for (const GeoZone *zone : relevantZones) {
        if (matched.contains(zone) || added.contains(zone)) {
            continue;
        }
        const GeoZoneSourceVersion current = currentSources.value(zone->sourceKey());
        const auto saved = savedSources.constFind(zone->sourceKey());
        const bool newData = (saved != savedSources.cend()) ? !saved->sameData(current) :
                                                              (!current.loadedAt.isValid() || !record.savedAt.isValid() || (current.loadedAt > record.savedAt));
        if (newData) {
            added.insert(zone);
            changes.added.append(ZoneChange{ Zone(), Zone::fromZone(zone) });
        }
    }

    for (const GeoZoneSourceVersion &saved : record.sources) {
        const auto it = currentSources.constFind(saved.key);
        if (it == currentSources.cend()) {
            changes.sources.append(SourceChange{ saved, GeoZoneSourceVersion() });
        } else if (!saved.sameData(*it)) {
            changes.sources.append(SourceChange{ saved, *it });
        }
    }
    return changes;
}

QString GeoZonePlanAudit::Changes::summary() const
{
    if (isEmpty()) {
        return QString();
    }
    QStringList parts;
    if (!modified.isEmpty()) {
        parts.append(tr("%n zone(s) modified", nullptr, static_cast<int>(modified.count())));
    }
    if (!added.isEmpty()) {
        parts.append(tr("%n new", nullptr, static_cast<int>(added.count())));
    }
    if (!removed.isEmpty()) {
        parts.append(tr("%n removed", nullptr, static_cast<int>(removed.count())));
    }
    const qsizetype removedSources = std::count_if(sources.cbegin(), sources.cend(), [](const SourceChange &change) { return change.removed(); });
    const qsizetype updatedSources = sources.count() - removedSources;
    if (updatedSources > 0) {
        parts.append(tr("%n source(s) updated", nullptr, static_cast<int>(updatedSources)));
    }
    if (removedSources > 0) {
        parts.append(tr("%n source(s) removed", nullptr, static_cast<int>(removedSources)));
    }
    if (modified.isEmpty() && added.isEmpty() && removed.isEmpty()) {
        return tr("Geozone data changed since this plan was saved: %1, the zones of the mission are unchanged.").arg(parts.join(QStringLiteral(", ")));
    }
    return tr("Geozone data changed since this plan was saved: %1. Review the plan.").arg(parts.join(QStringLiteral(", ")));
}

QStringList GeoZonePlanAudit::Changes::details() const
{
    const auto describe = [](const Zone &zone) { return tr("%1 (%2)").arg(zone.displayName(), GeoZone::restrictionToString(zone.restriction)); };
    QStringList lines;
    for (const ZoneChange &change : modified) {
        QStringList was;
        if (change.saved.displayName() != change.current.displayName()) {
            was.append(change.saved.displayName());
        }
        if (change.saved.restriction != change.current.restriction) {
            was.append(GeoZone::restrictionToString(change.saved.restriction));
        }
        lines.append(was.isEmpty() ? tr("Modified: %1").arg(describe(change.current)) :
                                     tr("Modified: %1, was %2").arg(describe(change.current), was.join(QStringLiteral(", "))));
    }
    for (const ZoneChange &change : added) {
        lines.append(tr("New: %1").arg(describe(change.current)));
    }
    for (const ZoneChange &change : removed) {
        lines.append(tr("Removed: %1").arg(describe(change.saved)));
    }
    for (const SourceChange &change : sources) {
        const QString name = change.saved.name.isEmpty() ? change.saved.key : change.saved.name;
        lines.append(change.removed() ? tr("Source removed or disabled: %1").arg(name) :
                                        tr("Source updated: %1, data of %2, was %3").arg(name, dateText(change.current.dataDate), dateText(change.saved.dataDate)));
    }
    return lines;
}
