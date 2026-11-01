/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include "GeoFenceController.h"

#include <optional>

class GeoZone;

/// Adds loaded geozones to a GeoFence as exclusion fences. Polygon holes are dropped, so the whole outer
/// boundary is excluded, and fences identical to existing ones are not added again.
class GeoZoneFenceImporter : public QObject
{
    Q_OBJECT

    Q_PROPERTY(GeoFenceController* geoFenceController READ geoFenceController WRITE setGeoFenceController NOTIFY geoFenceControllerChanged FINAL)

public:
    explicit GeoZoneFenceImporter(QObject *parent = nullptr);

    static constexpr int px4MaxFencePoints = 64;                ///< PX4 dataman fence storage (DM_KEY_FENCE_POINTS_MAX)
    static constexpr int arduPilotMaxPolygonVertices = 255;     ///< ArduPilot stores the polygon vertex count in 8 bits
    static constexpr double fenceBoundaryTolerance = 1;         ///< Meters zones may extend past fences covering them, for rounding

    /// Exclusion fences of a zone
    struct ZoneFences {
        QList<QList<QGeoCoordinate>> polygons;
        QList<QGeoCircle> circles;

        bool isEmpty() const { return polygons.isEmpty() && circles.isEmpty(); }
    };

    /// @return The exclusion fences importing the zone adds: the outer boundaries of its polygon volumes, simplified
    ///         outward when the tolerance is positive, see GeoZone::exclusionPolygons, and its circles
    static ZoneFences zoneFences(const GeoZone *zone, double simplifyTolerance = 0);

    /// @return The exclusion polygons and circles of the GeoFence
    static ZoneFences exclusionFences(GeoFenceController *geoFenceController);

    /// @return true if each of the fences importing the zone without simplification adds lies within one of the fences,
    ///         up to fenceBoundaryTolerance, so that the fences exclude the whole zone. Fences importing it with any
    ///         simplification add enclose those. False for zones importing adds no fence for.
    static bool fencesCover(const ZoneFences &fences, const GeoZone *zone);

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
    int addExclusions(const QList<const GeoZone*> &zones, double simplifyTolerance = 0);
    /// Same as addExclusions, for GeoZoneInfo values from QML
    Q_INVOKABLE int addZones(const QVariantList &zones, double simplifyTolerance = 0);

    /// Summarizes the fences addExclusions would add, see preview
    QVariantMap exclusionsPreview(const QList<const GeoZone*> &zones, double simplifyTolerance = 0) const;

signals:
    void geoFenceControllerChanged();

private:
    /// Fences for the zones which don't already exist in the fence, with the names of their zones
    struct Fences {
        QList<QList<QGeoCoordinate>> polygons;
        QStringList polygonNames;
        QList<QGeoCircle> circles;
        QStringList circleNames;
    };

    /// Arguments of a preview, whose fences an import with the same arguments reuses
    struct PreviewKey {
        QVariantList restrictions;
        QGeoRectangle area;
        int activeWithinHours = 0;
        double simplifyTolerance = 0;
        qsizetype polygonCount = 0;     ///< Existing fences, which new ones are compared with
        qsizetype circleCount = 0;

        friend bool operator==(const PreviewKey &, const PreviewKey &) = default;
    };

    Fences _newFences(const QList<const GeoZone*> &zones, double simplifyTolerance) const;
    Fences _fencesFor(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours, double simplifyTolerance) const;
    QVariantMap _preview(const Fences &fences) const;
    int _addFences(const Fences &fences);
    static QList<const GeoZone*> _zonesToImport(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours);

    QPointer<GeoFenceController> _geoFenceController;
    mutable std::optional<std::pair<PreviewKey, Fences>> _previewFences;
    mutable QElapsedTimer _previewAge;

    static constexpr qint64 _previewReuseMSecs = 60 * 1000;    ///< Zone schedules make older previews inexact
};
