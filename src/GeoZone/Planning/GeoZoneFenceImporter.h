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
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include <optional>

#include "GeoFenceController.h"
#include "GeoZoneBox.h"
#include "GeoZoneCatalog.h"
#include "GeoZoneFenceChoiceModel.h"
#include "GeoZoneFenceUpload.h"
#include "GeoZoneFences.h"
#include "GeoZonePolicy.h"

class GeoZone;
class GeoZoneMissionWorker;
class Vehicle;

/// Adds loaded geozones to a GeoFence as exclusion fences. Polygon holes are dropped, so the whole outer
/// boundary is excluded, and fences identical to existing ones are not added again.
/// Selections fit zones into the fence storage of the plan's vehicle, the zones most relevant for the planned flight
/// first, see GeoZoneFences::select, and replace the fences from geozones. The fence can then be uploaded and read
/// back from the vehicle to verify it, see GeoZoneFenceUpload.
/// The zones of a selection are prepared once, see GeoZoneFences::prepare, on the thread pool for updateSelection,
/// so that including or leaving out zones selects again quickly.
class GeoZoneFenceImporter : public QObject
{
    Q_OBJECT

    Q_PROPERTY(GeoFenceController*  geoFenceController  READ geoFenceController WRITE setGeoFenceController NOTIFY geoFenceControllerChanged FINAL)
    Q_PROPERTY(QVariantMap              capacity        READ capacityInfo       NOTIFY selectionChanged FINAL)  ///< See capacityInfo
    Q_PROPERTY(GeoZoneFenceChoiceModel* choices         READ choices            CONSTANT FINAL)                 ///< The selected zones and the first maxListedChoices zones left out, in priority order
    Q_PROPERTY(int                      selectedCount   READ selectedCount      NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int                      leftOutCount    READ leftOutCount       NOTIFY selectionChanged FINAL)  ///< All zones left out, also those not listed
    Q_PROPERTY(bool                     busy            READ busy               NOTIFY busyChanged FINAL)       ///< While zones are prepared for updateSelection
    Q_PROPERTY(GeoZoneFenceUpload*      upload          READ upload             CONSTANT FINAL)                 ///< Of the plan's fence to its vehicle
    Q_PROPERTY(QString                  staleText       READ staleText          NOTIFY staleChanged FINAL)      ///< Why the plan's fences from geozones are outdated, empty when they aren't, see GeoZoneFenceTags::staleZones

public:
    explicit GeoZoneFenceImporter(QObject *parent = nullptr);

    static constexpr int maxListedChoices = 500;    ///< Zones left out listed by choices

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

    /// Selects zones of GeoZoneManager like select, preparing them on the thread pool: the selection changes once they
    /// are prepared, unless another selection starts meanwhile
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
    int selectedCount() const { return _selectedCount; }
    int leftOutCount() const { return static_cast<int>(_selection.choices.count()) - _selectedCount; }
    bool busy() const { return _busy; }
    GeoZoneFenceChoiceModel *choices() const { return _choices; }

    /// Includes or leaves out the zone with the key, see GeoZone::key, and selects again
    Q_INVOKABLE void setZoneIncluded(const QString &key, bool included);

    /// Replaces the fences from geozones in the plan with those of the selected zones
    /// @return Number of fence polygons and circles added
    Q_INVOKABLE int applySelection();

    /// Replaces the fences from geozones in the plan with a selection made with the settings they were last selected
    /// with, see GeoZoneFenceTags::importSettings, or, for fences without them, of their zones still loaded,
    /// simplified up to rebuildSimplifyTolerance
    /// @return Number of fence polygons and circles added
    Q_INVOKABLE int rebuild();
    static constexpr double rebuildSimplifyTolerance = 50;  ///< Meters

    QString staleText() const { return _staleText; }

    /// Applies the selection and uploads the plan's fence to the vehicle, see GeoZoneFenceUpload
    Q_INVOKABLE void uploadToVehicle();
    GeoZoneFenceUpload *upload() const { return _upload; }

    /// @return Map with text ("Vehicle fence: 40 of 64 points"), note (where the limits come from), warning (fence
    ///         parameters which keep the vehicle from enforcing exclusion fences), usedItems, maxItems, usedBytes,
    ///         storageBytes, keptItems, circles, supported and estimated
    QVariantMap capacityInfo() const;

signals:
    void geoFenceControllerChanged();
    void selectionChanged();
    void busyChanged();
    void staleChanged();

private:
    /// Fences for the zones which don't already exist in the fence, with their zones
    struct Fences {
        QList<QList<QGeoCoordinate>> polygons;
        QList<const GeoZone*> polygonZones;
        QList<QGeoCircle> circles;
        QList<const GeoZone*> circleZones;
    };

    /// Zones of the catalog in an area, for updateSelection
    struct AreaQuery {
        GeoZoneBox area;
        GeoZonePolicy policy;
    };

    /// What preparing a selection needs, gathered on the GUI thread
    struct PrepareInput {
        GeoZoneCatalog catalog;                     ///< Keeps its zones while they are prepared
        std::optional<AreaQuery> query;             ///< Zones of the catalog, otherwise zones
        QList<const GeoZone*> zones;
        QList<GeoZonePathEvaluator::Path> paths;    ///< Of the planned flight
        QList<GeoZone::Area> areas;
        QList<QList<QGeoCoordinate>> lines;
        QDateTime now;
        QList<double> simplifyTolerances;
        bool circlesAsPolygons = false;
    };

    struct Prepared {
        GeoZoneCatalog catalog;
        GeoZoneFences::Prepared fences;
        QHash<QString, qsizetype> indexes;          ///< Of the entries, by zone key
    };

    static Prepared _prepare(const PrepareInput &input);

    Fences _newFences(const QList<const GeoZone*> &zones, double simplifyTolerance) const;
    int _addFences(const Fences &fences);
    void _setSimplifyTolerances(double maxSimplifyTolerance);
    /// Prepares the zones of the last selection for the vehicle, on the thread pool unless now
    void _prepareSelection(bool now);
    /// Prepares the zones again when a selection was prepared for another vehicle or is being prepared
    void _finishSelection();
    void _applyPrepared(const Prepared &prepared);
    void _select();
    void _updateCapacity();
    void _setBusy(bool busy);
    GeoZoneFenceChoiceModel::Row _row(const GeoZoneFences::Choice &choice) const;
    QString _capacityNote() const;
    QString _parameterWarning() const;
    Vehicle *_vehicle() const;
    void _connectVehicle();
    /// Checks the fences from geozones against the zones, see GeoZoneFenceTags::staleZones
    void _updateStale();

    QPointer<GeoFenceController> _geoFenceController;
    std::optional<AreaQuery> _query;                    ///< Of the last updateSelection
    QList<const GeoZone*> _zones;                       ///< Of the last select
    QList<double> _simplifyTolerances = { 0 };
    QHash<QString, bool> _included;                     ///< Zones included or left out by the user, by key
    Prepared _prepared;
    GeoZoneFences::Capacity _capacity;
    GeoZoneFences::Selection _selection;
    int _selectedCount = 0;
    bool _busy = false;
    QObject *_vehicleConnections = nullptr;
    QObject *_fenceConnections = nullptr;
    QJsonObject _settings;                              ///< Of the last updateSelection, see GeoZoneFenceTags::importSettings
    QString _staleText;
    GeoZoneMissionWorker *const _worker = nullptr;
    GeoZoneFenceChoiceModel *const _choices = nullptr;
    GeoZoneFenceUpload *const _upload = nullptr;

    static constexpr double _simplifySteps[] = { 0, 10, 25, 50, 100, 250, 500 };   ///< Meters, tried up to the maximum tolerance
};
