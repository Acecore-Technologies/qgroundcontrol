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
#include "GeoZoneInfo.h"

class GeoZoneManager;
class GeoZoneShape;
class GeoZoneSource;
class QmlObjectListModel;

/// A GeoZoneShape displayed on one map, at that map's level of detail
class GeoZoneMapItem : public QObject
{
    Q_OBJECT

    Q_PROPERTY(GeoZoneInfo      zone        READ zoneInfo   CONSTANT FINAL)
    Q_PROPERTY(int              restriction READ restriction CONSTANT FINAL)               ///< GeoZone::Restriction of the zone
    Q_PROPERTY(bool             active      READ active     NOTIFY activeChanged FINAL)    ///< The zone is active now
    Q_PROPERTY(QGeoShape        geoShape    READ geoShape   NOTIFY geoShapeChanged FINAL) ///< Polygon including holes
    Q_PROPERTY(QGeoCoordinate   center      READ center     CONSTANT FINAL)               ///< Circle only
    Q_PROPERTY(double           radius      READ radius     CONSTANT FINAL)               ///< Circle only, meters
    Q_PROPERTY(bool             highlighted READ highlighted NOTIFY highlightedChanged FINAL) ///< The zone is one of the model's highlightedZones

public:
    GeoZoneMapItem(const GeoZoneShape *shape, QObject *parent = nullptr);

    /// Identifies the item, only valid until the zones of its source change
    const GeoZoneShape *shape() const { return _shape; }
    GeoZone *zone() const { return _zone; }
    GeoZoneInfo zoneInfo() const { return GeoZoneInfo(_zone); }
    int restriction() const;
    bool active() const { return _active; }
    /// Re-reads whether the zone is active
    void updateActive();
    QGeoShape geoShape() const { return _geoShape; }
    QGeoCoordinate center() const { return _center; }
    double radius() const { return _radius; }
    bool highlighted() const { return _highlighted; }
    void setHighlighted(bool highlighted);

    void setZoomLevel(int zoomLevel);

signals:
    void geoShapeChanged();
    void highlightedChanged();
    void activeChanged();

private:
    const GeoZoneShape *_shape = nullptr;
    GeoZone *_zone = nullptr;
    bool _active = false;
    QGeoCoordinate _center;
    double _radius = 0;
    bool _isCircle = false;
    bool _highlighted = false;
    int _zoomLevel = -1;
    QGeoShape _geoShape;
};

/// Selects the GeoZone shapes and labels shown on one map, drawn by GeoZoneMapLayer. Only shapes in or near the view
/// are shown, limited to maxShapeCount preferring the largest, with polygons simplified for the zoom level, which
/// keeps the vertices drawn on each frame and the triangulation of new shapes bounded for large files.
class GeoZoneMapModel : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("QmlObjectListModel.h")

    Q_PROPERTY(QmlObjectListModel*  polygons        READ polygons           CONSTANT FINAL)                       ///< GeoZoneMapItem polygons
    Q_PROPERTY(QmlObjectListModel*  circles         READ circles            CONSTANT FINAL)                       ///< GeoZoneMapItem circles
    Q_PROPERTY(QVariantList         labelZones      READ labelZones         NOTIFY labelZonesChanged FINAL)       ///< GeoZoneInfo of the zones to label
    Q_PROPERTY(int                  hiddenZoneCount READ hiddenZoneCount    NOTIFY hiddenZoneCountChanged FINAL)  ///< Zones in view not shown because of the limit
    Q_PROPERTY(QVariantList         highlightedZones READ highlightedZones  WRITE setHighlightedZones NOTIFY highlightedZonesChanged FINAL)  ///< GeoZoneInfo of the zones drawn highlighted

public:
    static constexpr int maxShapeCount = 10000;
    static constexpr double viewMargin = 0.25;  ///< Shapes are prepared this fraction of the view size beyond each edge

    explicit GeoZoneMapModel(QObject *parent = nullptr);
    explicit GeoZoneMapModel(GeoZoneManager *manager, QObject *parent = nullptr);
    ~GeoZoneMapModel();

    /// Updates the shapes and labels for the map view. An invalid region removes everything.
    Q_INVOKABLE void setView(const QGeoRectangle &visibleRegion, double zoomLevel, bool showLabels);

    QmlObjectListModel *polygons() const { return _polygons; }
    QmlObjectListModel *circles() const { return _circles; }
    QVariantList labelZones() const { return _labelZones; }
    int hiddenZoneCount() const { return _hiddenZoneCount; }
    QVariantList highlightedZones() const { return _highlightedZones; }
    void setHighlightedZones(const QVariantList &zones);

signals:
    void hiddenZoneCountChanged(int hiddenZoneCount);
    void highlightedZonesChanged();
    void labelZonesChanged();

private slots:
    void _clear();
    void _removeSource(GeoZoneSource *source);
    void _refresh();
    void _updateActive();

private:
    void _syncItems(QmlObjectListModel *model, const QList<const GeoZoneShape*> &shapes, int zoomLevel);
    void _setHiddenZoneCount(int hiddenZoneCount);

    GeoZoneManager *_manager = nullptr;
    QmlObjectListModel *_polygons = nullptr;
    QmlObjectListModel *_circles = nullptr;
    QVariantList _labelZones;
    QHash<const GeoZoneShape*, GeoZoneMapItem*> _items;
    int _hiddenZoneCount = 0;
    QVariantList _highlightedZones;
    QSet<const GeoZone*> _highlightedZoneSet;

    QGeoRectangle _visibleRegion;
    double _zoomLevel = 0;
    bool _showLabels = false;
};
