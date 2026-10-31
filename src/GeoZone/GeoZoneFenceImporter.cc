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

int GeoZoneFenceImporter::importZones(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours, double simplifyTolerance)
{
    const QList<GeoZone*> zones = _zonesToImport(restrictions, area, activeWithinHours);
    const int added = addExclusions(zones, simplifyTolerance);

    qCDebug(GeoZoneManagerLog) << "importZones zones:fences" << zones.count() << added;

    return added;
}

QVariantMap GeoZoneFenceImporter::preview(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours, double simplifyTolerance) const
{
    return exclusionsPreview(_zonesToImport(restrictions, area, activeWithinHours), simplifyTolerance);
}

QVariantMap GeoZoneFenceImporter::exclusionsPreview(const QList<GeoZone*> &zones, double simplifyTolerance) const
{
    if (!_geoFenceController) {
        return QVariantMap();
    }

    const Fences fences = _newFences(zones, simplifyTolerance);

    int pointCount = fences.circles.count();
    qsizetype maxPolygonVertexCount = 0;
    for (const QList<QGeoCoordinate> &polygon : fences.polygons) {
        pointCount += polygon.count();
        maxPolygonVertexCount = qMax(maxPolygonVertexCount, polygon.count());
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

GeoZoneFenceImporter::Fences GeoZoneFenceImporter::_newFences(const QList<GeoZone*> &zones, double simplifyTolerance) const
{
    QList<QList<QGeoCoordinate>> existingPolygons;
    QmlObjectListModel *const polygons = _geoFenceController->polygons();
    for (int i = 0; i < polygons->count(); i++) {
        existingPolygons.append(polygons->value<QGCFencePolygon*>(i)->coordinateList());
    }
    QList<QGeoCircle> existingCircles;
    QmlObjectListModel *const circles = _geoFenceController->circles();
    for (int i = 0; i < circles->count(); i++) {
        QGCFenceCircle *const circle = circles->value<QGCFenceCircle*>(i);
        existingCircles.append(QGeoCircle(circle->center(), circle->radius()->rawValue().toDouble()));
    }

    Fences fences;
    for (const GeoZone *zone : zones) {
        for (const QList<QGeoCoordinate> &vertices : zone->exclusionPolygons(simplifyTolerance)) {
            if (!existingPolygons.contains(vertices) && !fences.polygons.contains(vertices)) {
                fences.polygons.append(vertices);
            }
        }
        for (const QGeoCircle &geoCircle : zone->exclusionCircles()) {
            if (!existingCircles.contains(geoCircle) && !fences.circles.contains(geoCircle)) {
                fences.circles.append(geoCircle);
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

    const Fences fences = _newFences(zones, simplifyTolerance);

    for (const QList<QGeoCoordinate> &vertices : fences.polygons) {
        QGCFencePolygon *const polygon = new QGCFencePolygon(false /* inclusion */, _geoFenceController);
        polygon->appendVertices(vertices);
        _geoFenceController->polygons()->append(polygon);
    }
    for (const QGeoCircle &geoCircle : fences.circles) {
        _geoFenceController->circles()->append(new QGCFenceCircle(geoCircle.center(), geoCircle.radius(), false /* inclusion */, _geoFenceController));
    }

    const int added = fences.polygons.count() + fences.circles.count();
    if (added > 0) {
        _geoFenceController->clearAllInteractive();
    }

    return added;
}
