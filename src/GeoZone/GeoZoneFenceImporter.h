/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include "GeoFenceController.h"

class GeoZone;

/// Adds loaded geozones to a GeoFence as exclusion fences. Polygon holes are dropped, so the whole outer
/// boundary is excluded, and fences identical to existing ones are not added again.
class GeoZoneFenceImporter : public QObject
{
    Q_OBJECT

    Q_PROPERTY(GeoFenceController* geoFenceController READ geoFenceController WRITE setGeoFenceController NOTIFY geoFenceControllerChanged)

public:
    explicit GeoZoneFenceImporter(QObject *parent = nullptr);

    static constexpr int px4MaxFencePoints = 64;                ///< PX4 dataman fence storage (DM_KEY_FENCE_POINTS_MAX)
    static constexpr int arduPilotMaxPolygonVertices = 255;     ///< ArduPilot stores the polygon vertex count in 8 bits

    GeoFenceController *geoFenceController() const { return _geoFenceController; }
    void setGeoFenceController(GeoFenceController *geoFenceController);

    /// Adds exclusion fences for the zones of GeoZoneManager
    ///     @param restrictions: GeoZone::Restriction values of the zones to import
    ///     @param area: Only zones intersecting this area are imported. Invalid to import zones anywhere.
    ///     @param activeWithinHours: Only zones active now (0) or at some time in the next hours. Negative to ignore zone schedules.
    ///     @param simplifyTolerance: Meters. When positive, polygons are reduced to fewer vertices which still enclose the zone.
    /// @return Number of fence polygons and circles added
    Q_INVOKABLE int importZones(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours = -1, double simplifyTolerance = 0);

    /// Summarizes the fences importZones would add with the same arguments
    /// @return Map with fenceCount, pointCount (polygon vertices plus one per circle), totalPointCount (including the
    ///         existing fence) and limitWarning (empty unless the vehicle likely can't store the resulting fence)
    Q_INVOKABLE QVariantMap preview(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours = -1, double simplifyTolerance = 0) const;

    /// Adds exclusion fences for the zones
    /// @return Number of fence polygons and circles added
    int addExclusions(const QList<GeoZone*> &zones, double simplifyTolerance = 0);

    /// Summarizes the fences addExclusions would add, see preview
    QVariantMap exclusionsPreview(const QList<GeoZone*> &zones, double simplifyTolerance = 0) const;

signals:
    void geoFenceControllerChanged();

private:
    /// Fences for the zones which don't already exist in the fence
    struct Fences {
        QList<QList<QGeoCoordinate>> polygons;
        QList<QGeoCircle> circles;
    };

    Fences _newFences(const QList<GeoZone*> &zones, double simplifyTolerance) const;
    static QList<GeoZone*> _zonesToImport(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours);

    QPointer<GeoFenceController> _geoFenceController;
};
