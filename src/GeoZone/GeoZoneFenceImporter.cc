/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFenceImporter.h"
#include "GeoFenceController.h"
#include "GeoZone.h"
#include "GeoZoneManager.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

GeoZoneFenceImporter::GeoZoneFenceImporter(QObject *parent)
    : QObject(parent)
{
    (void) connect(GeoZoneManager::instance(), &GeoZoneManager::zonesChanged, this, [this]() { _previewFences.reset(); });
}

void GeoZoneFenceImporter::setGeoFenceController(GeoFenceController *geoFenceController)
{
    if (geoFenceController != _geoFenceController) {
        _geoFenceController = geoFenceController;
        emit geoFenceControllerChanged();
    }
}

QList<GeoZone*> GeoZoneFenceImporter::_zonesToImport(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours)
{
    QList<GeoZone::Restriction> restrictionList;
    for (const QVariant &restriction : restrictions) {
        restrictionList.append(static_cast<GeoZone::Restriction>(restriction.toInt()));
    }

    QDateTime activeFrom;
    QDateTime activeTo;
    if (activeWithinHours >= 0) {
        activeFrom = QDateTime::currentDateTimeUtc();
        if (activeWithinHours > 0) {
            activeTo = activeFrom.addSecs(activeWithinHours * 3600);
        }
    }

    return GeoZoneManager::instance()->zonesInArea(restrictionList, area, activeFrom, activeTo);
}

GeoZoneFenceImporter::Fences GeoZoneFenceImporter::_fencesFor(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours, double simplifyTolerance) const
{
    const PreviewKey key{ restrictions, area, activeWithinHours, simplifyTolerance, _geoFenceController->polygons()->count(), _geoFenceController->circles()->count() };
    if (_previewFences && (_previewFences->first == key) && (_previewAge.elapsed() < _previewReuseMSecs)) {
        return _previewFences->second;
    }

    // Simplifying many zones takes a while, so the fences of a preview are kept for the import
    _previewFences.emplace(key, _newFences(_zonesToImport(restrictions, area, activeWithinHours), simplifyTolerance));
    _previewAge.start();
    return _previewFences->second;
}

int GeoZoneFenceImporter::importZones(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours, double simplifyTolerance)
{
    if (!_geoFenceController) {
        return 0;
    }
    const int added = _addFences(_fencesFor(restrictions, area, activeWithinHours, simplifyTolerance));
    _previewFences.reset();

    qCDebug(GeoZoneManagerLog) << "importZones fences" << added;

    return added;
}

QVariantMap GeoZoneFenceImporter::preview(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours, double simplifyTolerance) const
{
    if (!_geoFenceController) {
        return QVariantMap();
    }
    return _preview(_fencesFor(restrictions, area, activeWithinHours, simplifyTolerance));
}

QVariantMap GeoZoneFenceImporter::exclusionsPreview(const QList<GeoZone*> &zones, double simplifyTolerance) const
{
    if (!_geoFenceController) {
        return QVariantMap();
    }
    return _preview(_newFences(zones, simplifyTolerance));
}

QVariantMap GeoZoneFenceImporter::_preview(const Fences &fences) const
{
    int pointCount = fences.circles.count();
    qsizetype maxPolygonVertexCount = 0;
    for (const QList<QGeoCoordinate> &polygon : fences.polygons) {
        pointCount += polygon.count();
        maxPolygonVertexCount = (std::max)(maxPolygonVertexCount, polygon.count());
    }

    QmlObjectListModel *const polygons = _geoFenceController->polygons();
    int totalPointCount = pointCount + _geoFenceController->circles()->count() + (_geoFenceController->breachReturnPoint().isValid() ? 1 : 0);
    for (int i = 0; i < polygons->count(); i++) {
        totalPointCount += polygons->value<QGCFencePolygon*>(i)->count();
    }

    QString limitWarning;
    const Vehicle *const vehicle = _geoFenceController->masterController()->managerVehicle();
    if ((pointCount > 0) && vehicle) {
        if (vehicle->px4Firmware() && (totalPointCount > px4MaxFencePoints)) {
            limitWarning = tr("The fence will have %1 points. PX4 vehicles can store at most %2 fence points, so the upload will likely be rejected. Import fewer zones or increase the simplification.")
                               .arg(totalPointCount).arg(px4MaxFencePoints);
        } else if (vehicle->apmFirmware() && (maxPolygonVertexCount > arduPilotMaxPolygonVertices)) {
            limitWarning = tr("A fence polygon will have %1 vertices. ArduPilot vehicles support at most %2 vertices per polygon and the total fence storage of many boards is much smaller. Increase the simplification.")
                               .arg(maxPolygonVertexCount).arg(arduPilotMaxPolygonVertices);
        }
    }

    return QVariantMap{
        { QStringLiteral("fenceCount"),         fences.polygons.count() + fences.circles.count() },
        { QStringLiteral("pointCount"),         pointCount },
        { QStringLiteral("totalPointCount"),    totalPointCount },
        { QStringLiteral("limitWarning"),       limitWarning },
    };
}

namespace {

size_t _polygonHash(const QList<QGeoCoordinate> &polygon)
{
    size_t hash = 0;
    for (const QGeoCoordinate &coordinate : polygon) {
        hash = qHashMulti(hash, coordinate.latitude(), coordinate.longitude());
    }
    return hash;
}

size_t _circleHash(const QGeoCircle &circle)
{
    return qHashMulti(0, circle.center().latitude(), circle.center().longitude(), circle.radius());
}

/// Fences already in the fence or added, found by hash instead of comparing with each
template<typename Shape, size_t (*hashShape)(const Shape &)>
class ShapeSet
{
public:
    /// @return false if an equal shape is in the set
    bool insert(const Shape &shape)
    {
        const size_t hash = hashShape(shape);
        for (auto it = _shapes.constFind(hash); (it != _shapes.cend()) && (it.key() == hash); ++it) {
            if (it.value() == shape) {
                return false;
            }
        }
        _shapes.insert(hash, shape);
        return true;
    }

private:
    QMultiHash<size_t, Shape> _shapes;
};

} // namespace

GeoZoneFenceImporter::Fences GeoZoneFenceImporter::_newFences(const QList<GeoZone*> &zones, double simplifyTolerance) const
{
    ShapeSet<QList<QGeoCoordinate>, _polygonHash> polygonSet;
    QmlObjectListModel *const polygons = _geoFenceController->polygons();
    for (int i = 0; i < polygons->count(); i++) {
        (void) polygonSet.insert(polygons->value<QGCFencePolygon*>(i)->coordinateList());
    }
    ShapeSet<QGeoCircle, _circleHash> circleSet;
    QmlObjectListModel *const circles = _geoFenceController->circles();
    for (int i = 0; i < circles->count(); i++) {
        QGCFenceCircle *const circle = circles->value<QGCFenceCircle*>(i);
        (void) circleSet.insert(QGeoCircle(circle->center(), circle->radius()->rawValue().toDouble()));
    }

    Fences fences;
    for (const GeoZone *zone : zones) {
        const QString name = zone->displayName();
        for (const QList<QGeoCoordinate> &vertices : zone->exclusionPolygons(simplifyTolerance)) {
            if (polygonSet.insert(vertices)) {
                fences.polygons.append(vertices);
                fences.polygonNames.append(name);
            }
        }
        for (const QGeoCircle &geoCircle : zone->exclusionCircles()) {
            if (circleSet.insert(geoCircle)) {
                fences.circles.append(geoCircle);
                fences.circleNames.append(name);
            }
        }
    }
    return fences;
}

int GeoZoneFenceImporter::addExclusions(const QList<GeoZone*> &zones, double simplifyTolerance)
{
    if (!_geoFenceController) {
        return 0;
    }
    return _addFences(_newFences(zones, simplifyTolerance));
}

int GeoZoneFenceImporter::_addFences(const Fences &fences)
{
    // Appended in one batch each, so the fence list and map update once
    QList<QObject*> newPolygons;
    for (qsizetype i = 0; i < fences.polygons.count(); i++) {
        QGCFencePolygon *const polygon = new QGCFencePolygon(false /* inclusion */, _geoFenceController);
        polygon->appendVertices(fences.polygons[i]);
        polygon->setName(fences.polygonNames[i]);
        polygon->setGeoZone(true);
        newPolygons.append(polygon);
    }
    QList<QObject*> newCircles;
    for (qsizetype i = 0; i < fences.circles.count(); i++) {
        QGCFenceCircle *const circle = new QGCFenceCircle(fences.circles[i].center(), fences.circles[i].radius(), false /* inclusion */, _geoFenceController);
        circle->setName(fences.circleNames[i]);
        circle->setGeoZone(true);
        newCircles.append(circle);
    }
    if (!newPolygons.isEmpty()) {
        _geoFenceController->polygons()->append(newPolygons);
    }
    if (!newCircles.isEmpty()) {
        _geoFenceController->circles()->append(newCircles);
    }

    const int added = fences.polygons.count() + fences.circles.count();
    if (added > 0) {
        _geoFenceController->clearAllInteractive();
    }

    return added;
}
