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
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtQml/QQmlEngine>

namespace {

// Keys of GeoFenceController's JSON
constexpr const char *polygonsKey = "polygons";
constexpr const char *circlesKey = "circles";

// Keys of a fence's entry
constexpr const char *nameKey = "name";
constexpr const char *geoZoneKey = "geoZone";

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
            entries[i] = entry;
            tagged = true;
        }
        fenceJson[key] = entries;
    };
    saveTo(polygonsKey, geoFenceController->polygons());
    saveTo(circlesKey, geoFenceController->circles());
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
        }
    };
    loadFrom(polygonsKey, geoFenceController->polygons());
    loadFrom(circlesKey, geoFenceController->circles());
}
