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
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include "GeoFenceController.h"
#include "GeoZoneFenceUpload.h"
#include "GeoZoneFences.h"

class GeoZone;
class Vehicle;

/// Adds loaded geozones to a GeoFence as exclusion fences. Polygon holes are dropped, so the whole outer
/// boundary is excluded, and fences identical to existing ones are not added again.
/// Selections fit zones into the fence storage of the plan's vehicle, the zones most relevant for the planned flight
/// first, see GeoZoneFences::select, and replace the fences from geozones. The fence can then be uploaded and read
/// back from the vehicle to verify it, see GeoZoneFenceUpload.
class GeoZoneFenceImporter : public QObject
{
    Q_OBJECT

    Q_PROPERTY(GeoFenceController*  geoFenceController  READ geoFenceController WRITE setGeoFenceController NOTIFY geoFenceControllerChanged FINAL)
    Q_PROPERTY(QVariantMap          capacity            READ capacityInfo       NOTIFY selectionChanged FINAL)      ///< See capacityInfo
    Q_PROPERTY(QVariantList         choices             READ choicesInfo        NOTIFY selectionChanged FINAL)      ///< See choicesInfo
    Q_PROPERTY(int                  selectedCount       READ selectedCount      NOTIFY selectionChanged FINAL)
    Q_PROPERTY(GeoZoneFenceUpload*  upload              READ upload             CONSTANT FINAL)                     ///< Of the plan's fence to its vehicle

public:
    explicit GeoZoneFenceImporter(QObject *parent = nullptr);

    static constexpr int maxListedChoices = 500;    ///< Zones left out listed by choicesInfo

    /// @return The exclusion polygons and circles of the GeoFence, see GeoZoneFences::fencesCover
    static GeoZoneFences::ZoneFences exclusionFences(GeoFenceController *geoFenceController);

    /// @return What the fence of the vehicle can store: PX4's dataman, ArduPilot's fence storage for the vehicle type or
    ///         on the SD card, a conservative estimate without a vehicle, nothing without fence support
    static GeoZoneFences::Capacity vehicleCapacity(Vehicle *vehicle);

    /// @return Storage of the plan fences selections keep: those not from geozones and the breach return point
    static GeoZoneFences::Usage keptUsage(GeoFenceController *geoFenceController);

    GeoFenceController *geoFenceController() const { return _geoFenceController; }
    void setGeoFenceController(GeoFenceController *geoFenceController);

    /// Adds exclusion fences for the zones
    /// @return Number of fence polygons and circles added
    int addExclusions(const QList<const GeoZone*> &zones, double simplifyTolerance = 0);
    /// Same as addExclusions, for GeoZoneInfo values from QML
    Q_INVOKABLE int addZones(const QVariantList &zones, double simplifyTolerance = 0);

    /// Selects zones of GeoZoneManager, see select
    ///     @param restrictions: GeoZone::Restriction values of the zones
    ///     @param area: Only zones intersecting this area. Invalid for zones anywhere.
    ///     @param activeWithinHours: Only zones active now (0) or at some time in the next hours. Negative to ignore zone schedules.
    ///     @param maxSimplifyTolerance: Meters fences may be simplified by to fit, see GeoZoneFences::zoneFences
    Q_INVOKABLE void updateSelection(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours = -1, double maxSimplifyTolerance = 0);

    /// Selects which of the zones fit into the fence of the plan's vehicle besides the fences kept, the zones entered by or
    /// near the planned flight first, see GeoZoneFences::select. Simplification steps up to the tolerance when needed.
    void select(const QList<const GeoZone*> &zones, double maxSimplifyTolerance = 0);
    const GeoZoneFences::Selection &selection() const { return _selection; }
    const GeoZoneFences::Capacity &capacity() const { return _capacity; }
    int selectedCount() const;

    /// Includes or leaves out the zone with the key, see GeoZone::key, and selects again
    Q_INVOKABLE void setZoneIncluded(const QString &key, bool included);

    /// Replaces the fences from geozones in the plan with those of the selected zones
    /// @return Number of fence polygons and circles added
    Q_INVOKABLE int applySelection();

    /// Applies the selection and uploads the plan's fence to the vehicle, see GeoZoneFenceUpload
    Q_INVOKABLE void uploadToVehicle();
    GeoZoneFenceUpload *upload() const { return _upload; }

    /// @return Map with text ("Vehicle fence: 40 of 64 points"), note (where the limits come from), warning (fence
    ///         parameters which keep the vehicle from enforcing exclusion fences), usedItems, maxItems, usedBytes,
    ///         storageBytes, keptItems, circles, supported and estimated
    QVariantMap capacityInfo() const;
    /// @return One map per zone, selected ones first: key, name, restriction, restrictionText, proximityText, points,
    ///         simplifyTolerance, selected, included and reason
    QVariantList choicesInfo() const;

signals:
    void geoFenceControllerChanged();
    void selectionChanged();

private:
    /// Fences for the zones which don't already exist in the fence, with the names of their zones
    struct Fences {
        QList<QList<QGeoCoordinate>> polygons;
        QStringList polygonNames;
        QList<QGeoCircle> circles;
        QStringList circleNames;
    };

    Fences _newFences(const QList<const GeoZone*> &zones, double simplifyTolerance) const;
    int _addFences(const Fences &fences);
    void _select();
    QString _capacityNote() const;
    QString _parameterWarning() const;
    Vehicle *_vehicle() const;
    void _connectVehicle();

    QPointer<GeoFenceController> _geoFenceController;
    QList<const GeoZone*> _zones;                       ///< Of the last selection
    QList<double> _simplifyTolerances = { 0 };
    QHash<QString, bool> _included;                     ///< Zones included or left out by the user, by key
    GeoZoneFences::Capacity _capacity;
    GeoZoneFences::Selection _selection;
    QObject *_vehicleConnections = nullptr;
    GeoZoneFenceUpload *const _upload = nullptr;

    static constexpr double _simplifySteps[] = { 0, 10, 25, 50, 100, 250, 500 };   ///< Meters, tried up to the maximum tolerance
};
