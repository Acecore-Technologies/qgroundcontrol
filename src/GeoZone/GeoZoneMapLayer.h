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
#include <QtCore/QPointer>
#include <QtGui/QColor>
#include <QtGui/QMatrix4x4>
#include <QtGui/QTransform>
#include <QtPositioning/QGeoShape>
#include <QtQuick/QQuickItem>

#include <functional>

class GeoZoneMapItem;
class GeoZoneMapModel;
class QDeclarativeGeoMap;

/// Draws the polygons and circles of a GeoZoneMapModel on a map in two scene graph nodes, the fills then the
/// outlines, instead of one map item per shape. Shapes are triangulated once per level of detail in Web Mercator
/// coordinates, and moved with the map by a transform, so panning, zooming and rotating only rebuild the vertices
/// when the zoom changes by half a level. Add it as a child of the map filling it; it doesn't take mouse events.
/// The map is expected to be flat, tilted maps aren't supported.
class GeoZoneMapLayer : public QQuickItem
{
    Q_OBJECT
    Q_MOC_INCLUDE("GeoZoneMapModel.h")

    Q_PROPERTY(QQuickItem       *map                READ map                WRITE setMap                NOTIFY mapChanged FINAL)
    Q_PROPERTY(GeoZoneMapModel  *model              READ model              WRITE setModel              NOTIFY modelChanged FINAL)
    Q_PROPERTY(QVariantList     restrictionColors   READ restrictionColors  WRITE setRestrictionColors  NOTIFY stylesChanged FINAL)  ///< Colors by GeoZone::Restriction
    Q_PROPERTY(QColor           highlightColor      READ highlightColor     WRITE setHighlightColor     NOTIFY stylesChanged FINAL)
    Q_PROPERTY(qreal            activeFillAlpha     READ activeFillAlpha    WRITE setActiveFillAlpha    NOTIFY stylesChanged FINAL)
    Q_PROPERTY(qreal            inactiveFillAlpha   READ inactiveFillAlpha  WRITE setInactiveFillAlpha  NOTIFY stylesChanged FINAL)
    Q_PROPERTY(int              vertexCount         READ vertexCount        NOTIFY vertexCountChanged FINAL)    ///< Vertices drawn, for tests

public:
    explicit GeoZoneMapLayer(QQuickItem *parent = nullptr);
    ~GeoZoneMapLayer() override;

    QQuickItem *map() const { return _map; }
    void setMap(QQuickItem *map);
    GeoZoneMapModel *model() const { return _model; }
    void setModel(GeoZoneMapModel *model);
    QVariantList restrictionColors() const;
    void setRestrictionColors(const QVariantList &colors);
    QColor highlightColor() const { return _highlightColor; }
    void setHighlightColor(const QColor &color);
    qreal activeFillAlpha() const { return _activeFillAlpha; }
    void setActiveFillAlpha(qreal alpha);
    qreal inactiveFillAlpha() const { return _inactiveFillAlpha; }
    void setInactiveFillAlpha(qreal alpha);
    int vertexCount() const { return static_cast<int>((_fillVertices.count() + _strokeVertices.count())); }

    /// Shape of a map item in Web Mercator coordinates, see GeoZoneShape::toMercator
    struct Triangulation {
        QList<QPointF> triangles;       ///< Fill, three vertices per triangle
        QList<QList<QPointF>> rings;    ///< Outlines, unclosed
        QPointF center;                 ///< Of the bounds, for wrapping around the antimeridian
    };
    /// @return Triangulated polygon with holes, filled even-odd
    static Triangulation triangulatePolygon(const QList<QList<QPointF>> &rings);
    /// @return Triangulated circle of the radius in meters, as a polygon following the geodesic circle
    static Triangulation triangulateCircle(const QGeoCoordinate &center, double radius);

    /// Finds the affine transform from Web Mercator to item coordinates of a flat map
    /// @param toItem Item position of a coordinate on the map, not finite for coordinates it can't project
    /// @return false when the map can't be projected
    static bool mercatorTransform(const QGeoCoordinate &center, double zoomLevel, const std::function<QPointF(const QGeoCoordinate&)> &toItem, QTransform &transform);

signals:
    void mapChanged();
    void modelChanged();
    void stylesChanged();
    void vertexCountChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void updatePolish() override;

private:
    /// Vertex of the scene graph geometry, matching QSGGeometry::ColoredPoint2D
    struct Vertex {
        float x;
        float y;
        uchar r;
        uchar g;
        uchar b;
        uchar a;
    };

    struct CachedShape {
        QGeoShape source;               ///< Shape the triangulation was made from
        Triangulation triangulation;
    };

    void _scheduleRebuild();
    void _updateCamera();
    /// @return Mercator to item coordinates, an invalid transform without a map
    bool _mercatorTransform(QTransform &transform) const;
    void _rebuild(const QTransform &mercatorToItem);
    const Triangulation &_triangulation(const GeoZoneMapItem *item);
    QColor _restrictionColor(int restriction) const;

    QPointer<QDeclarativeGeoMap> _mapView;
    QPointer<QQuickItem> _map;
    QPointer<GeoZoneMapModel> _model;
    QList<QColor> _restrictionColors;
    QColor _highlightColor = Qt::white;
    qreal _activeFillAlpha = 0.25;
    qreal _inactiveFillAlpha = 0.08;

    QHash<const GeoZoneMapItem*, CachedShape> _cache;
    bool _rebuildNeeded = true;
    QPointF _origin;                    ///< Mercator coordinates at the origin of the vertices
    double _scale = 0;                  ///< Pixels per mercator unit of the vertices
    QMatrix4x4 _matrix;                 ///< Vertices to item coordinates
    QList<Vertex> _fillVertices;
    QList<Vertex> _strokeVertices;
    bool _geometryDirty = false;
};
