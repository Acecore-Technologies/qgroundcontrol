/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFenceTags.h"
#include "GeoFenceController.h"
#include "GeoZone.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"

#include <QtCore/QHash>
#include <QtCore/QSet>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtQml/QQmlEngine>

#include <algorithm>

namespace {

// Keys of GeoFenceController's JSON
constexpr const char *polygonsKey = "polygons";
constexpr const char *circlesKey = "circles";

// Keys of a fence's entry
constexpr const char *nameKey = "name";
constexpr const char *geoZoneKey = "geoZone";
constexpr const char *zoneKeyKey = "zoneKey";
constexpr const char *contentHashKey = "contentHash";
constexpr const char *validToKey = "validTo";

// Key of the GeoFence's import settings, and the controller property keeping them
constexpr const char *importKey = "geoZoneImport";
constexpr const char *importProperty = "_geoZoneFenceImport";

/// Calls the function with each fence of the controller
template <typename Function>
void forEachFence(GeoFenceController *geoFenceController, Function function)
{
    for (QmlObjectListModel *fences : { geoFenceController->polygons(), geoFenceController->circles() }) {
        for (int i = 0; i < fences->count(); i++) {
            function(fences->get(i));
        }
    }
}

} // namespace

GeoZoneFenceTag::GeoZoneFenceTag(QObject *fence)
    : QObject(fence)
{
    // Kept by its fence, also when QML reads it
    QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
}

GeoZoneFenceTag *GeoZoneFenceTag::of(QObject *fence)
{
    if (!fence) {
        return nullptr;
    }
    GeoZoneFenceTag *tag = fence->findChild<GeoZoneFenceTag*>(QString(), Qt::FindDirectChildrenOnly);
    return tag ? tag : new GeoZoneFenceTag(fence);
}

const GeoZoneFenceTag *GeoZoneFenceTag::find(const QObject *fence)
{
    return fence ? fence->findChild<const GeoZoneFenceTag*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

bool GeoZoneFenceTag::isGeoZone(const QObject *fence)
{
    const GeoZoneFenceTag *const tag = find(fence);
    return tag && tag->geoZone();
}

void GeoZoneFenceTag::setName(const QString &name)
{
    if (name != _name) {
        _name = name;
        emit nameChanged();
    }
}

void GeoZoneFenceTag::setGeoZone(bool geoZone)
{
    if (geoZone != _geoZone) {
        _geoZone = geoZone;
        emit geoZoneChanged();
    }
}

void GeoZoneFenceTag::setZone(const GeoZone *zone)
{
    setName(zone->displayName());
    setGeoZone(true);
    _zoneKey = zone->key();
    _contentHash = zone->data().contentHash;
    _validTo = validityEnd(zone);
}

QDateTime GeoZoneFenceTag::validityEnd(const GeoZone *zone)
{
    QDateTime end;
    for (const GeoZone::TimePeriod &period : zone->data().applicability) {
        if (period.permanent || !period.end.isValid()) {
            return QDateTime();
        }
        end = end.isValid() ? (std::max)(end, period.end) : period.end;
    }
    return end;
}

GeoZoneFenceTags::GeoZoneFenceTags(QObject *parent)
    : QObject(parent)
{
}

GeoZoneFenceTag *GeoZoneFenceTags::tag(QObject *fence) const
{
    return GeoZoneFenceTag::of(fence);
}

void GeoZoneFenceTags::deleteFences(GeoFenceController *geoFenceController, bool geoZoneOnly)
{
    if (!geoFenceController) {
        return;
    }
    const auto deleteFrom = [geoZoneOnly](QmlObjectListModel *fences) {
        QList<QObject*> kept;
        QList<QObject*> deleted;
        for (QObject *fence : std::as_const(*fences->objectList())) {
            ((geoZoneOnly && !GeoZoneFenceTag::isGeoZone(fence)) ? kept : deleted).append(fence);
        }
        if (deleted.isEmpty()) {
            return false;
        }
        for (QObject *fence : std::as_const(deleted)) {
            (void) fence->disconnect(fences);
            fence->deleteLater();
        }
        (void) fences->swapObjectList(kept);
        fences->setDirty(true);
        return true;
    };
    const bool deletedPolygons = deleteFrom(geoFenceController->polygons());
    const bool deletedCircles = deleteFrom(geoFenceController->circles());
    // The lists only signal becoming dirty, not a change while they are
    if (deletedPolygons || deletedCircles) {
        geoFenceController->setDirty(true);
    }
}

void GeoZoneFenceTags::save(GeoFenceController *geoFenceController, QJsonObject &planJson)
{
    if (!geoFenceController) {
        return;
    }
    QJsonObject fenceJson = planJson.value(PlanMasterController::kJsonGeoFenceObjectKey).toObject();
    bool tagged = false;
    const auto saveTo = [&fenceJson, &tagged](const char *key, QmlObjectListModel *fences) {
        QJsonArray entries = fenceJson.value(key).toArray();
        if (entries.count() != fences->count()) {
            return;
        }
        for (int i = 0; i < fences->count(); i++) {
            const GeoZoneFenceTag *const tag = GeoZoneFenceTag::find(fences->get(i));
            if (!tag || (tag->name().isEmpty() && !tag->geoZone())) {
                continue;
            }
            QJsonObject entry = entries[i].toObject();
            if (!tag->name().isEmpty()) {
                entry[nameKey] = tag->name();
            }
            if (tag->geoZone()) {
                entry[geoZoneKey] = true;
            }
            if (!tag->zoneKey().isEmpty()) {
                entry[zoneKeyKey] = tag->zoneKey();
                entry[contentHashKey] = QString::fromLatin1(tag->contentHash().toHex());
                if (tag->validTo().isValid()) {
                    entry[validToKey] = tag->validTo().toUTC().toString(Qt::ISODate);
                }
            }
            entries[i] = entry;
            tagged = true;
        }
        fenceJson[key] = entries;
    };
    saveTo(polygonsKey, geoFenceController->polygons());
    saveTo(circlesKey, geoFenceController->circles());
    const QJsonObject settings = importSettings(geoFenceController);
    if (tagged && !settings.isEmpty()) {
        fenceJson[importKey] = settings;
    }
    if (tagged) {
        planJson[PlanMasterController::kJsonGeoFenceObjectKey] = fenceJson;
    }
}

void GeoZoneFenceTags::load(GeoFenceController *geoFenceController, const QJsonObject &planJson)
{
    if (!geoFenceController) {
        return;
    }
    const QJsonObject fenceJson = planJson.value(PlanMasterController::kJsonGeoFenceObjectKey).toObject();
    const auto loadFrom = [&fenceJson](const char *key, QmlObjectListModel *fences) {
        // GeoFenceController loads all fences of the plan in their order, or none
        const QJsonArray entries = fenceJson.value(key).toArray();
        if (entries.count() != fences->count()) {
            return;
        }
        for (int i = 0; i < fences->count(); i++) {
            const QJsonObject entry = entries[i].toObject();
            const QString name = entry.value(nameKey).toString();
            const bool geoZone = entry.value(geoZoneKey).toBool();
            if (name.isEmpty() && !geoZone) {
                continue;
            }
            GeoZoneFenceTag *const tag = GeoZoneFenceTag::of(fences->get(i));
            tag->setName(name);
            tag->setGeoZone(geoZone);
            tag->_zoneKey = entry.value(zoneKeyKey).toString();
            tag->_contentHash = QByteArray::fromHex(entry.value(contentHashKey).toString().toLatin1());
            tag->_validTo = QDateTime::fromString(entry.value(validToKey).toString(), Qt::ISODate).toUTC();
        }
    };
    loadFrom(polygonsKey, geoFenceController->polygons());
    loadFrom(circlesKey, geoFenceController->circles());
    setImportSettings(geoFenceController, fenceJson.value(importKey).toObject());
}

QList<GeoZoneFenceTags::StaleZone> GeoZoneFenceTags::staleZones(GeoFenceController *geoFenceController, const QList<GeoZone*> &zones, const QDateTime &now)
{
    QList<StaleZone> stale;
    if (!geoFenceController) {
        return stale;
    }
    QHash<QString, const GeoZone*> zonesByKey;
    QSet<QString> checked;
    forEachFence(geoFenceController, [&](const QObject *fence) {
        const GeoZoneFenceTag *const tag = GeoZoneFenceTag::find(fence);
        if (!tag || !tag->geoZone() || tag->zoneKey().isEmpty() || checked.contains(tag->zoneKey())) {
            return;
        }
        checked.insert(tag->zoneKey());
        if (zonesByKey.isEmpty()) {
            for (const GeoZone *zone : zones) {
                zonesByKey.insert(zone->key(), zone);
            }
        }
        const GeoZone *const zone = zonesByKey.value(tag->zoneKey());
        if (!zone) {
            stale.append(StaleZone{ tag->name(), StaleZone::Reason::Removed });
        } else if (zone->data().contentHash != tag->contentHash()) {
            stale.append(StaleZone{ tag->name(), StaleZone::Reason::Changed });
        } else if (tag->validTo().isValid() && (tag->validTo() <= now)) {
            stale.append(StaleZone{ tag->name(), StaleZone::Reason::Expired });
        }
    });
    return stale;
}

QString GeoZoneFenceTags::staleText(const QList<StaleZone> &staleZones)
{
    if (staleZones.isEmpty()) {
        return QString();
    }
    QStringList names;
    for (const StaleZone &zone : staleZones) {
        if (names.count() == GeoZoneFenceTags::maxListedStaleZones) {
            names.append(tr("%n more", nullptr, static_cast<int>(staleZones.count() - names.count())));
            break;
        }
        switch (zone.reason) {
        case StaleZone::Reason::Changed:
            names.append(tr("%1 (changed)").arg(zone.name));
            break;
        case StaleZone::Reason::Removed:
            names.append(tr("%1 (no longer loaded)").arg(zone.name));
            break;
        case StaleZone::Reason::Expired:
            names.append(tr("%1 (expired)").arg(zone.name));
            break;
        }
    }
    return tr("Geozone fences are outdated: %1. Rebuild the geozone fences.").arg(names.join(QStringLiteral(", ")));
}

QJsonObject GeoZoneFenceTags::importSettings(const GeoFenceController *geoFenceController)
{
    return geoFenceController ? geoFenceController->property(importProperty).toJsonObject() : QJsonObject();
}

void GeoZoneFenceTags::setImportSettings(GeoFenceController *geoFenceController, const QJsonObject &settings)
{
    if (geoFenceController) {
        (void) geoFenceController->setProperty(importProperty, settings);
    }
}
