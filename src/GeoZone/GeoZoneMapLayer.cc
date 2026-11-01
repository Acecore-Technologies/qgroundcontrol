/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMapLayer.h"
#include "GeoZone.h"
#include "GeoZoneMapModel.h"
#include "GeoZoneShape.h"
#include "QmlObjectListModel.h"

#include <QtGui/QPainterPath>
#include <QtGui/private/qtriangulator_p.h>
#include <QtLocation/private/qdeclarativegeomap_p.h>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoPolygon>
#include <QtQuick/QSGGeometryNode>
#include <QtQuick/QSGTransformNode>
#include <QtQuick/QSGVertexColorMaterial>

#include <QtCore/QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {

constexpr int kCircleSegments = 72;
constexpr double kMaxZoomDrift = 1.41;      ///< Scale change from the vertices, half a zoom level, before they are rebuilt
constexpr double kMaxOriginOffset = 1e5;    ///< Pixels from the item to the origin of the vertices before they are rebuilt

/// Keeps a ring on the side of the antimeridian of its first vertex
QList<QPointF> unwrapped(const QList<QPointF> &ring, double referenceX)
{
    QList<QPointF> result = ring;
    for (QPointF &point : result) {
        if ((point.x() - referenceX) > 0.5) {
            point.rx() -= 1;
        } else if ((point.x() - referenceX) < -0.5) {
            point.rx() += 1;
        }
    }
    return result;
}

QList<QPointF> toMercator(const QList<QGeoCoordinate> &ring)
{
    QList<QPointF> points;
    points.reserve(ring.count());
    for (const QGeoCoordinate &coordinate : ring) {
        points.append(GeoZoneShape::toMercator(coordinate));
    }
    return points;
}

} // namespace

GeoZoneMapLayer::GeoZoneMapLayer(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    // A picture in picture map is smaller than the window
    setClip(true);
}

GeoZoneMapLayer::~GeoZoneMapLayer()
{
}

void GeoZoneMapLayer::setMap(QQuickItem *map)
{
    if (map == _map) {
        return;
    }
    if (_mapView) {
        (void) disconnect(_mapView, nullptr, this, nullptr);
    }
    _map = map;
    _mapView = qobject_cast<QDeclarativeGeoMap*>(map);
    if (_mapView) {
        const auto cameraChanged = [this]() { polish(); };
        (void) connect(_mapView, &QDeclarativeGeoMap::visibleRegionChanged, this, cameraChanged);
        (void) connect(_mapView, &QDeclarativeGeoMap::zoomLevelChanged, this, cameraChanged);
        (void) connect(_mapView, &QDeclarativeGeoMap::centerChanged, this, cameraChanged);
        (void) connect(_mapView, &QDeclarativeGeoMap::bearingChanged, this, cameraChanged);
    }
    emit mapChanged();
    _scheduleRebuild();
}

void GeoZoneMapLayer::setModel(GeoZoneMapModel *model)
{
    if (model == _model) {
        return;
    }
    if (_model) {
        (void) disconnect(_model->polygons(), nullptr, this, nullptr);
        (void) disconnect(_model->circles(), nullptr, this, nullptr);
    }
    _model = model;
    _cache.clear();
    if (_model) {
        for (QmlObjectListModel *list : { _model->polygons(), _model->circles() }) {
            (void) connect(list, &QAbstractItemModel::rowsInserted, this, &GeoZoneMapLayer::_scheduleRebuild);
            (void) connect(list, &QAbstractItemModel::rowsRemoved, this, &GeoZoneMapLayer::_scheduleRebuild);
            (void) connect(list, &QAbstractItemModel::modelReset, this, &GeoZoneMapLayer::_scheduleRebuild);
        }
    }
    emit modelChanged();
    _scheduleRebuild();
}

QVariantList GeoZoneMapLayer::restrictionColors() const
{
    QVariantList colors;
    for (const QColor &color : _restrictionColors) {
        colors.append(color);
    }
    return colors;
}

void GeoZoneMapLayer::setRestrictionColors(const QVariantList &colors)
{
    QList<QColor> restrictionColors;
    for (const QVariant &color : colors) {
        restrictionColors.append(color.value<QColor>());
    }
    if (restrictionColors != _restrictionColors) {
        _restrictionColors = restrictionColors;
        emit stylesChanged();
        _scheduleRebuild();
    }
}

void GeoZoneMapLayer::setHighlightColor(const QColor &color)
{
    if (color != _highlightColor) {
        _highlightColor = color;
        emit stylesChanged();
        _scheduleRebuild();
    }
}

void GeoZoneMapLayer::setActiveFillAlpha(qreal alpha)
{
    if (alpha != _activeFillAlpha) {
        _activeFillAlpha = alpha;
        emit stylesChanged();
        _scheduleRebuild();
    }
}

void GeoZoneMapLayer::setInactiveFillAlpha(qreal alpha)
{
    if (alpha != _inactiveFillAlpha) {
        _inactiveFillAlpha = alpha;
        emit stylesChanged();
        _scheduleRebuild();
    }
}

void GeoZoneMapLayer::_scheduleRebuild()
{
    _rebuildNeeded = true;
    polish();
}

void GeoZoneMapLayer::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    polish();
}

void GeoZoneMapLayer::updatePolish()
{
    _updateCamera();
}

bool GeoZoneMapLayer::_mercatorTransform(QTransform &transform) const
{
    if (!_mapView || (width() <= 0) || (height() <= 0)) {
        return false;
    }

    // The map is flat, so mercator coordinates map to the item by an affine transform found from three points
    const QGeoCoordinate center = _mapView->center();
    if (!center.isValid()) {
        return false;
    }
    const QPointF mercatorCenter = GeoZoneShape::toMercator(center);
    const double step = 100.0 / GeoZoneShape::mercatorPixels(_mapView->zoomLevel());
    const QPointF centerPoint = mapFromItem(_mapView, _mapView->fromCoordinate(center, false));
    const QPointF eastPoint = mapFromItem(_mapView, _mapView->fromCoordinate(GeoZoneShape::fromMercator(mercatorCenter + QPointF(step, 0)), false));
    const QPointF southPoint = mapFromItem(_mapView, _mapView->fromCoordinate(GeoZoneShape::fromMercator(mercatorCenter + QPointF(0, step)), false));
    for (const QPointF &point : { centerPoint, eastPoint, southPoint }) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            return false;
        }
    }

    const double m11 = (eastPoint.x() - centerPoint.x()) / step;
    const double m12 = (eastPoint.y() - centerPoint.y()) / step;
    const double m21 = (southPoint.x() - centerPoint.x()) / step;
    const double m22 = (southPoint.y() - centerPoint.y()) / step;
    transform = QTransform(m11, m12, m21, m22,
                           centerPoint.x() - ((m11 * mercatorCenter.x()) + (m21 * mercatorCenter.y())),
                           centerPoint.y() - ((m12 * mercatorCenter.x()) + (m22 * mercatorCenter.y())));
    return transform.isInvertible();
}

void GeoZoneMapLayer::_updateCamera()
{
    QTransform transform;
    if (!_mercatorTransform(transform)) {
        if (!_fillVertices.isEmpty() || !_strokeVertices.isEmpty()) {
            _fillVertices.clear();
            _strokeVertices.clear();
            _geometryDirty = true;
            emit vertexCountChanged();
            update();
        }
        return;
    }

    const double scale = std::hypot(transform.m11(), transform.m12());
    const QPointF originPoint = transform.map(_origin);
    const bool drifted = (_scale <= 0) || ((scale / _scale) > kMaxZoomDrift) || ((_scale / scale) > kMaxZoomDrift) ||
                         (std::abs(originPoint.x()) > kMaxOriginOffset) || (std::abs(originPoint.y()) > kMaxOriginOffset);
    if (_rebuildNeeded || drifted) {
        _rebuild(transform);
    }

    // Vertices are (mercator - origin) * scale, so the item point of a vertex is transform(origin) + transform' * vertex / scale
    const QPointF origin = transform.map(_origin);
    _matrix = QMatrix4x4(static_cast<float>(transform.m11() / _scale), static_cast<float>(transform.m21() / _scale), 0, static_cast<float>(origin.x()),
                         static_cast<float>(transform.m12() / _scale), static_cast<float>(transform.m22() / _scale), 0, static_cast<float>(origin.y()),
                         0, 0, 1, 0,
                         0, 0, 0, 1);
    update();
}

QColor GeoZoneMapLayer::_restrictionColor(int restriction) const
{
    return ((restriction >= 0) && (restriction < _restrictionColors.count())) ? _restrictionColors[restriction] : QColor(Qt::gray);
}

void GeoZoneMapLayer::_rebuild(const QTransform &mercatorToItem)
{
    _rebuildNeeded = false;
    _origin = mercatorToItem.inverted().map(QPointF(width() / 2, height() / 2));
    _scale = std::hypot(mercatorToItem.m11(), mercatorToItem.m12());
    _fillVertices.clear();
    _strokeVertices.clear();
    _geometryDirty = true;

    QList<const GeoZoneMapItem*> items;
    if (_model) {
        for (const QmlObjectListModel *list : { _model->polygons(), _model->circles() }) {
            for (int i = 0; i < list->count(); i++) {
                GeoZoneMapItem *const item = list->value<GeoZoneMapItem*>(i);
                (void) connect(item, &GeoZoneMapItem::geoShapeChanged, this, &GeoZoneMapLayer::_scheduleRebuild, Qt::UniqueConnection);
                (void) connect(item, &GeoZoneMapItem::activeChanged, this, &GeoZoneMapLayer::_scheduleRebuild, Qt::UniqueConnection);
                (void) connect(item, &GeoZoneMapItem::highlightedChanged, this, &GeoZoneMapLayer::_scheduleRebuild, Qt::UniqueConnection);
                items.append(item);
            }
        }
    }
    // The most restrictive zones are drawn last, on top
    std::stable_sort(items.begin(), items.end(), [](const GeoZoneMapItem *a, const GeoZoneMapItem *b) {
        return GeoZone::restrictionSeverity(a->zone()->restriction()) > GeoZone::restrictionSeverity(b->zone()->restriction());
    });

    // Items removed from the model are forgotten
    const QSet<const GeoZoneMapItem*> current(items.cbegin(), items.cend());
    for (auto it = _cache.begin(); it != _cache.end();) {
        it = current.contains(it.key()) ? std::next(it) : _cache.erase(it);
    }

    const auto premultiplied = [](const QColor &color, qreal alpha) {
        const qreal a = color.alphaF() * alpha;
        return std::array<uchar, 4>{ static_cast<uchar>(std::lround(color.redF() * a * 255)), static_cast<uchar>(std::lround(color.greenF() * a * 255)),
                                     static_cast<uchar>(std::lround(color.blueF() * a * 255)), static_cast<uchar>(std::lround(a * 255)) };
    };
    for (const GeoZoneMapItem *item : std::as_const(items)) {
        const Triangulation &triangulation = _triangulation(item);
        // Shapes are drawn on the copy of the world nearest to the view
        const double shiftX = std::round(_origin.x() - triangulation.center.x());
        const auto toVertex = [this, shiftX](const QPointF &mercator) {
            return QPointF((mercator.x() + shiftX - _origin.x()) * _scale, (mercator.y() - _origin.y()) * _scale);
        };

        const QColor restrictionColor = _restrictionColor(item->restriction());
        const auto fill = premultiplied(restrictionColor, item->active() ? _activeFillAlpha : _inactiveFillAlpha);
        for (const QPointF &mercator : triangulation.triangles) {
            const QPointF point = toVertex(mercator);
            _fillVertices.append(Vertex{ static_cast<float>(point.x()), static_cast<float>(point.y()), fill[0], fill[1], fill[2], fill[3] });
        }

        // Outlines as quads with square caps, which cover the joins
        const auto stroke = premultiplied(item->highlighted() ? _highlightColor : restrictionColor, 1);
        const double halfWidth = (item->highlighted() ? 3.0 : (item->active() ? 2.0 : 1.0)) / 2;
        const auto appendStrokeVertex = [this, &stroke](const QPointF &point) {
            _strokeVertices.append(Vertex{ static_cast<float>(point.x()), static_cast<float>(point.y()), stroke[0], stroke[1], stroke[2], stroke[3] });
        };
        for (const QList<QPointF> &ring : triangulation.rings) {
            for (qsizetype i = 0; i < ring.count(); i++) {
                const QPointF a = toVertex(ring[i]);
                const QPointF b = toVertex(ring[(i + 1) % ring.count()]);
                const QPointF direction = b - a;
                const double length = std::hypot(direction.x(), direction.y());
                if (length < 1e-6) {
                    continue;
                }
                const QPointF along = direction * (halfWidth / length);
                const QPointF across(-along.y(), along.x());
                const QPointF start = a - along;
                const QPointF end = b + along;
                appendStrokeVertex(start + across);
                appendStrokeVertex(start - across);
                appendStrokeVertex(end + across);
                appendStrokeVertex(end + across);
                appendStrokeVertex(start - across);
                appendStrokeVertex(end - across);
            }
        }
    }
    emit vertexCountChanged();
}

const GeoZoneMapLayer::Triangulation &GeoZoneMapLayer::_triangulation(const GeoZoneMapItem *item)
{
    const bool circle = item->shape()->isCircle();
    const QGeoShape source = circle ? QGeoShape(QGeoCircle(item->center(), item->radius())) : item->geoShape();
    auto it = _cache.find(item);
    if ((it != _cache.end()) && (it->source == source)) {
        return it->triangulation;
    }

    CachedShape cached;
    cached.source = source;
    if (circle) {
        cached.triangulation = triangulateCircle(item->center(), item->radius());
    } else {
        const QGeoPolygon polygon(source);
        QList<QList<QPointF>> rings = { toMercator(polygon.perimeter()) };
        for (int i = 0; i < polygon.holesCount(); i++) {
            rings.append(toMercator(polygon.holePath(i)));
        }
        cached.triangulation = triangulatePolygon(rings);
    }
    return _cache.insert(item, cached)->triangulation;
}

GeoZoneMapLayer::Triangulation GeoZoneMapLayer::triangulatePolygon(const QList<QList<QPointF>> &rings)
{
    Triangulation result;
    if (rings.isEmpty() || (rings.first().count() < 3)) {
        return result;
    }

    const double referenceX = rings.first().first().x();
    double minX = qInf();
    double minY = qInf();
    double maxX = -qInf();
    double maxY = -qInf();
    for (const QList<QPointF> &ring : rings) {
        const QList<QPointF> points = unwrapped(ring, referenceX);
        for (const QPointF &point : points) {
            minX = (std::min)(minX, point.x());
            minY = (std::min)(minY, point.y());
            maxX = (std::max)(maxX, point.x());
            maxY = (std::max)(maxY, point.y());
        }
        if (points.count() >= 3) {
            result.rings.append(points);
        }
    }
    result.center = QPointF((minX + maxX) / 2, (minY + maxY) / 2);
    const double extent = (std::max)(maxX - minX, maxY - minY);
    if (!(extent > 0)) {
        return result;
    }

    // The triangulator rounds to 1/32 of a unit, so the shape is scaled to about a million units
    const double scale = (1 << 20) / extent;
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const QList<QPointF> &ring : std::as_const(result.rings)) {
        path.moveTo((ring.first().x() - minX) * scale, (ring.first().y() - minY) * scale);
        for (qsizetype i = 1; i < ring.count(); i++) {
            path.lineTo((ring[i].x() - minX) * scale, (ring[i].y() - minY) * scale);
        }
        path.closeSubpath();
    }

    const QTriangleSet triangles = qTriangulate(path, QTransform(), 1, true);
    const int indexCount = triangles.indices.size();
    result.triangles.reserve(indexCount);
    const auto vertexAt = [&triangles, minX, minY, scale](quint32 index) {
        return QPointF(minX + (triangles.vertices[2 * index] / scale), minY + (triangles.vertices[(2 * index) + 1] / scale));
    };
    if (triangles.indices.type() == QVertexIndexVector::UnsignedInt) {
        const quint32 *const indices = static_cast<const quint32*>(triangles.indices.data());
        for (int i = 0; i < indexCount; i++) {
            result.triangles.append(vertexAt(indices[i]));
        }
    } else {
        const quint16 *const indices = static_cast<const quint16*>(triangles.indices.data());
        for (int i = 0; i < indexCount; i++) {
            result.triangles.append(vertexAt(indices[i]));
        }
    }
    return result;
}

GeoZoneMapLayer::Triangulation GeoZoneMapLayer::triangulateCircle(const QGeoCoordinate &center, double radius)
{
    Triangulation result;
    if (!center.isValid() || !(radius > 0)) {
        return result;
    }
    QList<QGeoCoordinate> ring;
    for (int i = 0; i < kCircleSegments; i++) {
        ring.append(center.atDistanceAndAzimuth(radius, (360.0 * i) / kCircleSegments));
    }
    const QPointF mercatorCenter = GeoZoneShape::toMercator(center);
    const QList<QPointF> points = unwrapped(toMercator(ring), mercatorCenter.x());
    result.rings.append(points);
    result.center = mercatorCenter;
    result.triangles.reserve(points.count() * 3);
    for (qsizetype i = 0; i < points.count(); i++) {
        result.triangles.append(mercatorCenter);
        result.triangles.append(points[i]);
        result.triangles.append(points[(i + 1) % points.count()]);
    }
    return result;
}

QSGNode *GeoZoneMapLayer::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data)
{
    Q_UNUSED(data);

    QSGTransformNode *root = static_cast<QSGTransformNode*>(oldNode);
    if (!root) {
        root = new QSGTransformNode();
        for (int i = 0; i < 2; i++) {
            QSGGeometryNode *const node = new QSGGeometryNode();
            QSGGeometry *const geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            node->setGeometry(geometry);
            node->setMaterial(new QSGVertexColorMaterial());
            node->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
            root->appendChildNode(node);
        }
        _geometryDirty = true;
    }

    if (_geometryDirty) {
        _geometryDirty = false;
        const QList<Vertex> *const vertexLists[] = { &_fillVertices, &_strokeVertices };
        QSGNode *child = root->firstChild();
        for (const QList<Vertex> *vertices : vertexLists) {
            QSGGeometryNode *const node = static_cast<QSGGeometryNode*>(child);
            QSGGeometry *const geometry = node->geometry();
            geometry->allocate(static_cast<int>(vertices->count()));
            static_assert(sizeof(Vertex) == sizeof(QSGGeometry::ColoredPoint2D));
            if (!vertices->isEmpty()) {
                memcpy(geometry->vertexDataAsColoredPoint2D(), vertices->constData(), vertices->count() * sizeof(Vertex));
            }
            node->markDirty(QSGNode::DirtyGeometry);
            child = child->nextSibling();
        }
    }

    root->setMatrix(_matrix);
    root->markDirty(QSGNode::DirtyMatrix);
    return root;
}
