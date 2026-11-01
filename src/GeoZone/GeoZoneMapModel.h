/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>
#include <QtPositioning/QGeoShape>

#include "GeoZone.h"

class GeoZoneManager;
class GeoZoneShape;
class GeoZoneSource;
class QmlObjectListModel;

/// A GeoZoneShape displayed on one map, at that map's level of detail
class GeoZoneMapItem : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("GeoZone.h")

    Q_PROPERTY(GeoZone*         zone        READ zone       CONSTANT FINAL)
    Q_PROPERTY(QGeoShape        geoShape    READ geoShape   NOTIFY geoShapeChanged FINAL) ///< Polygon including holes
    Q_PROPERTY(QGeoCoordinate   center      READ center     CONSTANT FINAL)               ///< Circle only
    Q_PROPERTY(double           radius      READ radius     CONSTANT FINAL)               ///< Circle only, meters

public:
    GeoZoneMapItem(const GeoZoneShape *shape, QObject *parent = nullptr);

    /// Identifies the item, only valid until the zones of its source change
    const GeoZoneShape *shape() const { return _shape; }
    GeoZone *zone() const { return _zone; }
    QGeoShape geoShape() const { return _geoShape; }
    QGeoCoordinate center() const { return _center; }
    double radius() const { return _radius; }

    void setZoomLevel(int zoomLevel);

signals:
    void geoShapeChanged();

private:
    const GeoZoneShape *_shape = nullptr;
    QPointer<GeoZone> _zone;
    QGeoCoordinate _center;
    double _radius = 0;
    bool _isCircle = false;
    int _zoomLevel = -1;
    QGeoShape _geoShape;
};

/// Selects the GeoZone shapes and labels shown on one map. Only shapes in or near the view are shown,
/// limited to maxShapeCount preferring the largest, with polygons simplified for the zoom level.
/// Map items cost time on every frame even when off screen, so this keeps large files usable.
class GeoZoneMapModel : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("QmlObjectListModel.h")

    Q_PROPERTY(QmlObjectListModel*  polygons        READ polygons           CONSTANT FINAL)                       ///< GeoZoneMapItem polygons
    Q_PROPERTY(QmlObjectListModel*  circles         READ circles            CONSTANT FINAL)                       ///< GeoZoneMapItem circles
    Q_PROPERTY(QmlObjectListModel*  labelZones      READ labelZones         CONSTANT FINAL)                       ///< GeoZone objects to label
    Q_PROPERTY(int                  hiddenZoneCount READ hiddenZoneCount    NOTIFY hiddenZoneCountChanged FINAL)  ///< Zones in view not shown because of the limit

public:
    static constexpr int maxShapeCount = 2000;
    static constexpr double viewMargin = 0.25;  ///< Shapes are prepared this fraction of the view size beyond each edge

    explicit GeoZoneMapModel(QObject *parent = nullptr);
    explicit GeoZoneMapModel(GeoZoneManager *manager, QObject *parent = nullptr);
    ~GeoZoneMapModel();

    /// Updates the shapes and labels for the map view. An invalid region removes everything.
    Q_INVOKABLE void setView(const QGeoRectangle &visibleRegion, double zoomLevel, bool showLabels);

    QmlObjectListModel *polygons() const { return _polygons; }
    QmlObjectListModel *circles() const { return _circles; }
    QmlObjectListModel *labelZones() const { return _labelZones; }
    int hiddenZoneCount() const { return _hiddenZoneCount; }

signals:
    void hiddenZoneCountChanged(int hiddenZoneCount);

private slots:
    void _clear();
    void _removeSource(GeoZoneSource *source);
    void _refresh();

private:
    void _syncItems(QmlObjectListModel *model, const QList<const GeoZoneShape*> &shapes, int zoomLevel);
    void _setHiddenZoneCount(int hiddenZoneCount);

    GeoZoneManager *_manager = nullptr;
    QmlObjectListModel *_polygons = nullptr;
    QmlObjectListModel *_circles = nullptr;
    QmlObjectListModel *_labelZones = nullptr;
    QHash<const GeoZoneShape*, GeoZoneMapItem*> _items;
    int _hiddenZoneCount = 0;

    QGeoRectangle _visibleRegion;
    double _zoomLevel = 0;
    bool _showLabels = false;
};
