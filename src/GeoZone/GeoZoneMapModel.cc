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
#include "GeoZoneManager.h"
#include "GeoZoneShape.h"
#include "QmlObjectListModel.h"

#include <QtCore/QSet>
#include <QtCore/QtMath>

#include <algorithm>

GeoZoneMapItem::GeoZoneMapItem(const GeoZoneShape *shape, QObject *parent)
    : QObject(parent)
    , _shape(shape)
{

}

GeoZone *GeoZoneMapItem::zone() const
{
    return _shape->zone();
}

QGeoCoordinate GeoZoneMapItem::center() const
{
    return _shape->center();
}

double GeoZoneMapItem::radius() const
{
    return _shape->radius();
}

void GeoZoneMapItem::setZoomLevel(int zoomLevel)
{
    if (_shape->isCircle() || (zoomLevel == _zoomLevel)) {
        return;
    }

    _zoomLevel = zoomLevel;
    _geoShape = _shape->polygon(zoomLevel);
    emit geoShapeChanged();
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
    , _labelZones(new QmlObjectListModel(this))
{
    // Shapes are deleted when zones change, so items referencing them must go first
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMapModel::_clear);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMapModel::_refresh);
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
    QList<GeoZoneMercatorBounds> nearViews;
    for (const GeoZoneMercatorBounds &view : views) {
        nearViews.append({ view.minX - marginX, view.minY - marginY, view.maxX + marginX, view.maxY + marginY });
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
    for (const GeoZoneShape *shape : _manager->shapes()) {
        const GeoZoneMercatorBounds &bounds = shape->mercatorBounds();
        if (!intersectsAny(nearViews, bounds)) {
            continue;
        }
        const bool inView = intersectsAny(views, bounds);
        if (inView) {
            zonesInView.insert(shape->zone());
        }
        candidates.append({ shape, qMax(bounds.width(), bounds.height()) * pixels, inView });
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

    QList<QObject*> labelZones;
    if (showLabels) {
        for (QObject *zone : _manager->labelZones(visibleRegion)) {
            if (shownZones.contains(qobject_cast<GeoZone*>(zone))) {
                labelZones.append(zone);
            }
        }
    }
    if (labelZones != *_labelZones->objectList()) {
        _labelZones->clear();
        if (!labelZones.isEmpty()) {
            _labelZones->append(labelZones);
        }
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
    _labelZones->clear();
    _polygons->clear();
    _circles->clear();
    for (GeoZoneMapItem *item : std::as_const(_items)) {
        item->deleteLater();
    }
    _items.clear();
    _setHiddenZoneCount(0);
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
