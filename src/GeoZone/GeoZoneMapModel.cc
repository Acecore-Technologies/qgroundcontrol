/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMapModel.h"
#include "GeoZone.h"
#include "GeoZoneDisplay.h"
#include "GeoZoneManager.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "QmlObjectListModel.h"

#include <QtCore/QSet>
#include <QtCore/QtMath>

#include <algorithm>

GeoZoneMapItem::GeoZoneMapItem(const GeoZoneShape *shape, QObject *parent)
    : QObject(parent)
    , _shape(shape)
    , _zone(shape->zone())
    , _active(shape->zone()->active())
    , _center(shape->center())
    , _radius(shape->radius())
    , _isCircle(shape->isCircle())
{
}

void GeoZoneMapItem::setHighlighted(bool highlighted)
{
    if (highlighted != _highlighted) {
        _highlighted = highlighted;
        emit highlightedChanged();
    }
}

int GeoZoneMapItem::restriction() const
{
    return static_cast<int>(_zone->restriction());
}

void GeoZoneMapItem::updateActive()
{
    if (_zone->active() != _active) {
        _active = _zone->active();
        emit activeChanged();
    }
}

void GeoZoneMapItem::setZoomLevel(int zoomLevel)
{
    // Only called while the shape exists, the item is removed before its zone's shapes are deleted
    if (_isCircle || (zoomLevel == _zoomLevel)) {
        return;
    }

    _zoomLevel = zoomLevel;
    emit zoomLevelChanged();
}

QList<QList<QPointF>> GeoZoneMapItem::mercatorRings() const
{
    return _shape->mercatorRings(_zoomLevel);
}

GeoZoneMapModel::GeoZoneMapModel(QObject *parent)
    : GeoZoneMapModel(GeoZoneManager::instance(), parent)
{
}

GeoZoneMapModel::GeoZoneMapModel(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _polygons(new QmlObjectListModel(this))
    , _circles(new QmlObjectListModel(this))
{
    // Shapes are deleted when zones change, so items referencing them must go first
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMapModel::_removeSource);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMapModel::_refresh);
    (void) connect(_manager, &GeoZoneManager::activeZonesChanged, this, &GeoZoneMapModel::_updateActive);
    (void) connect(_manager->display(), &GeoZoneDisplay::hiddenRestrictionsChanged, this, &GeoZoneMapModel::_refresh);
}

GeoZoneMapModel::~GeoZoneMapModel()
{

}

void GeoZoneMapModel::setView(const QGeoRectangle &visibleRegion, double zoomLevel, bool showLabels)
{
    _visibleRegion = visibleRegion;
    _zoomLevel = zoomLevel;
    _showLabels = showLabels;

    if (!visibleRegion.isValid() || _manager->shapes().isEmpty()) {
        _clear();
        return;
    }

    // A view crossing the antimeridian is split in two
    const QPointF topLeft = GeoZoneShape::toMercator(visibleRegion.topLeft());
    const QPointF bottomRight = GeoZoneShape::toMercator(visibleRegion.bottomRight());
    QList<GeoZoneMercatorBounds> views;
    if (topLeft.x() <= bottomRight.x()) {
        views.append({ topLeft.x(), topLeft.y(), bottomRight.x(), bottomRight.y() });
    } else {
        views.append({ topLeft.x(), topLeft.y(), 1.0, bottomRight.y() });
        views.append({ 0.0, topLeft.y(), bottomRight.x(), bottomRight.y() });
    }

    double viewWidth = 0;
    for (const GeoZoneMercatorBounds &view : views) {
        viewWidth += view.width();
    }
    const double marginX = viewWidth * viewMargin;
    const double marginY = views.first().height() * viewMargin;
    const int hiddenRestrictions = _manager->display()->hiddenRestrictions();
    QList<GeoZoneMercatorBounds> nearViews;
    QList<const GeoZoneShape*> nearShapes;
    QSet<const GeoZoneShape*> nearShapeSet;
    for (const GeoZoneMercatorBounds &view : views) {
        const GeoZoneMercatorBounds nearView{ view.minX - marginX, view.minY - marginY, view.maxX + marginX, view.maxY + marginY };
        nearViews.append(nearView);

        // Mercator y grows southward, the edges of the projection include the poles
        const QGeoCoordinate northWest = GeoZoneShape::fromMercator(QPointF(std::clamp(nearView.minX, 0.0, 1.0), nearView.minY));
        const QGeoCoordinate southEast = GeoZoneShape::fromMercator(QPointF(std::clamp(nearView.maxX, 0.0, 1.0), nearView.maxY));
        const double north = (nearView.minY <= 0) ? 90.0 : northWest.latitude();
        const double south = (nearView.maxY >= 1) ? -90.0 : southEast.latitude();
        for (const GeoZoneShape *shape : _manager->shapesInArea(south, northWest.longitude(), north, southEast.longitude())) {
            if (!nearShapeSet.contains(shape) && !(hiddenRestrictions & (1 << shape->zone()->restriction()))) {
                nearShapeSet.insert(shape);
                nearShapes.append(shape);
            }
        }
    }

    const auto intersectsAny = [](const QList<GeoZoneMercatorBounds> &bounds, const GeoZoneMercatorBounds &shapeBounds) {
        return std::any_of(bounds.cbegin(), bounds.cend(), [&shapeBounds](const GeoZoneMercatorBounds &b) { return b.intersects(shapeBounds); });
    };

    struct Candidate {
        const GeoZoneShape *shape;
        double sizePixels;
        bool inView;
    };
    QList<Candidate> candidates;
    QSet<GeoZone*> zonesInView;
    const double pixels = GeoZoneShape::mercatorPixels(zoomLevel);
    for (const GeoZoneShape *shape : std::as_const(nearShapes)) {
        const GeoZoneMercatorBounds &bounds = shape->mercatorBounds();
        if (!intersectsAny(nearViews, bounds)) {
            continue;
        }
        const bool inView = intersectsAny(views, bounds);
        if (inView) {
            zonesInView.insert(shape->zone());
        }
        candidates.append({ shape, (std::max)(bounds.width(), bounds.height()) * pixels, inView });
    }

    if (candidates.count() > maxShapeCount) {
        // Keep the shapes in view first, then the largest
        std::nth_element(candidates.begin(), candidates.begin() + maxShapeCount, candidates.end(), [](const Candidate &a, const Candidate &b) {
            return (a.inView != b.inView) ? a.inView : (a.sizePixels > b.sizePixels);
        });
        candidates.resize(maxShapeCount);
    }

    QList<const GeoZoneShape*> polygonShapes;
    QList<const GeoZoneShape*> circleShapes;
    QSet<GeoZone*> shownZones;
    for (const Candidate &candidate : candidates) {
        (candidate.shape->isCircle() ? circleShapes : polygonShapes).append(candidate.shape);
        shownZones.insert(candidate.shape->zone());
    }

    const int level = qFloor(zoomLevel);
    _syncItems(_polygons, polygonShapes, level);
    _syncItems(_circles, circleShapes, level);
    for (int i = 0; i < _polygons->count(); i++) {
        _polygons->value<GeoZoneMapItem*>(i)->setZoomLevel(level);
    }

    QList<GeoZone*> labelZones;
    if (showLabels) {
        for (GeoZone *zone : _manager->catalog().labelZones(visibleRegion, GeoZoneManager::maxLabelCount)) {
            if (shownZones.contains(zone)) {
                labelZones.append(zone);
            }
        }
    }
    const QVariantList labelInfos = GeoZoneInfo::toVariantList(labelZones);
    if (labelInfos != _labelZones) {
        _labelZones = labelInfos;
        emit labelZonesChanged();
    }

    int hiddenZoneCount = 0;
    for (GeoZone *zone : std::as_const(zonesInView)) {
        if (!shownZones.contains(zone)) {
            hiddenZoneCount++;
        }
    }
    _setHiddenZoneCount(hiddenZoneCount);
}

void GeoZoneMapModel::_syncItems(QmlObjectListModel *model, const QList<const GeoZoneShape*> &shapes, int zoomLevel)
{
    const QSet<const GeoZoneShape*> wanted(shapes.cbegin(), shapes.cend());

    QList<QObject*> keptItems;
    QList<GeoZoneMapItem*> removedItems;
    for (int i = 0; i < model->count(); i++) {
        GeoZoneMapItem *const item = model->value<GeoZoneMapItem*>(i);
        if (wanted.contains(item->shape())) {
            keptItems.append(item);
        } else {
            removedItems.append(item);
        }
    }

    QList<QObject*> newItems;
    for (const GeoZoneShape *shape : shapes) {
        if (!_items.contains(shape)) {
            GeoZoneMapItem *const item = new GeoZoneMapItem(shape, this);
            item->setZoomLevel(zoomLevel);
            item->setHighlighted(_highlightedZoneSet.contains(item->zone()));
            _items.insert(shape, item);
            newItems.append(item);
        }
    }

    if (removedItems.count() > keptItems.count()) {
        // Removing most rows one at a time is slower than recreating the remaining delegates
        model->clear();
        keptItems.append(newItems);
        if (!keptItems.isEmpty()) {
            model->append(keptItems);
        }
    } else {
        if (!removedItems.isEmpty()) {
            for (int i = model->count() - 1; i >= 0; i--) {
                if (!wanted.contains(model->value<GeoZoneMapItem*>(i)->shape())) {
                    (void) model->removeAt(i);
                }
            }
        }
        if (!newItems.isEmpty()) {
            model->append(newItems);
        }
    }

    for (GeoZoneMapItem *item : std::as_const(removedItems)) {
        (void) _items.remove(item->shape());
        item->deleteLater();
    }
}

void GeoZoneMapModel::_clear()
{
    if (!_labelZones.isEmpty()) {
        _labelZones.clear();
        emit labelZonesChanged();
    }
    _polygons->clear();
    _circles->clear();
    for (GeoZoneMapItem *item : std::as_const(_items)) {
        item->deleteLater();
    }
    _items.clear();
    _setHiddenZoneCount(0);
}

void GeoZoneMapModel::setHighlightedZones(const QVariantList &zones)
{
    if (zones == _highlightedZones) {
        return;
    }
    _highlightedZones = zones;
    const QList<const GeoZone*> highlighted = GeoZoneInfo::fromVariantList(zones);
    _highlightedZoneSet = QSet<const GeoZone*>(highlighted.cbegin(), highlighted.cend());
    for (GeoZoneMapItem *item : std::as_const(_items)) {
        item->setHighlighted(_highlightedZoneSet.contains(item->zone()));
    }
    emit highlightedZonesChanged();
}

void GeoZoneMapModel::_removeSource(GeoZoneSource *source)
{
    const auto fromSource = [source](const QObject *object) {
        return qobject_cast<const GeoZoneMapItem*>(object)->zone()->sourceKey() == source->key();
    };
    for (QmlObjectListModel *model : { _polygons, _circles }) {
        QList<QObject*> kept = *model->objectList();
        if (kept.removeIf(fromSource) > 0) {
            (void) model->swapObjectList(kept);
        }
    }
    const qsizetype labelCount = _labelZones.count();
    _labelZones.removeIf([source](const QVariant &value) { return GeoZoneInfo::fromVariant(value)->sourceKey() == source->key(); });
    if (_labelZones.count() != labelCount) {
        emit labelZonesChanged();
    }
    for (auto it = _items.begin(); it != _items.end();) {
        if (fromSource(it.value())) {
            it.value()->deleteLater();
            it = _items.erase(it);
        } else {
            ++it;
        }
    }
}

void GeoZoneMapModel::_updateActive()
{
    for (GeoZoneMapItem *item : std::as_const(_items)) {
        item->updateActive();
    }
}

void GeoZoneMapModel::_refresh()
{
    setView(_visibleRegion, _zoomLevel, _showLabels);
}

void GeoZoneMapModel::_setHiddenZoneCount(int hiddenZoneCount)
{
    if (hiddenZoneCount != _hiddenZoneCount) {
        _hiddenZoneCount = hiddenZoneCount;
        emit hiddenZoneCountChanged(_hiddenZoneCount);
    }
}
