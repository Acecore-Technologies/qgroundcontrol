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
#include <QtCore/QTimer>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include "GeoZone.h"
#include "GeoZoneInfo.h"
#include "GeoZoneMapFilter.h"

class GeoZoneManager;
class GeoZoneShape;
class GeoZoneSource;
class MissionController;
class QmlObjectListModel;

/// A GeoZoneShape displayed on one map, at that map's level of detail
class GeoZoneMapItem : public QObject
{
    Q_OBJECT

    Q_PROPERTY(GeoZoneInfo      zone        READ zoneInfo   CONSTANT FINAL)
    Q_PROPERTY(int              restriction READ restriction CONSTANT FINAL)               ///< GeoZone::Restriction of the zone
    Q_PROPERTY(bool             active      READ active     NOTIFY activeChanged FINAL)    ///< The zone is active now
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
    /// Polygon rings at the item's level of detail, see GeoZoneShape::mercatorRings
    QList<QList<QPointF>> mercatorRings() const;
    int zoomLevel() const { return _zoomLevel; }
    QGeoCoordinate center() const { return _center; }
    double radius() const { return _radius; }
    bool highlighted() const { return _highlighted; }
    void setHighlighted(bool highlighted);

    void setZoomLevel(int zoomLevel);

signals:
    void zoomLevelChanged();
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
};

/// Selects the GeoZone shapes and labels shown on one map, drawn by GeoZoneMapLayer. Only shapes in or near the view
/// are shown, limited to maxShapeCount preferring the largest, with polygons simplified for the zoom level, which
/// keeps the vertices drawn on each frame and the triangulation of new shapes bounded for large files. The filters of
/// GeoZoneDisplay hide zones, except the kept and highlighted zones, like those the mission check or the flight alerts
/// report. The altitude filter compares the zones with the altitudes of the mission of missionController, or with the
/// altitude of the active vehicle with vehicleAltitudes.
class GeoZoneMapModel : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")
    Q_MOC_INCLUDE("QmlObjectListModel.h")

    Q_PROPERTY(QmlObjectListModel*  polygons        READ polygons           CONSTANT FINAL)                       ///< GeoZoneMapItem polygons
    Q_PROPERTY(QmlObjectListModel*  circles         READ circles            CONSTANT FINAL)                       ///< GeoZoneMapItem circles
    Q_PROPERTY(QVariantList         labelZones      READ labelZones         NOTIFY labelZonesChanged FINAL)       ///< GeoZoneInfo of the zones to label
    Q_PROPERTY(int                  hiddenZoneCount READ hiddenZoneCount    NOTIFY hiddenZoneCountChanged FINAL)  ///< Zones in view not shown because of the limit
    Q_PROPERTY(QVariantList         highlightedZones READ highlightedZones  WRITE setHighlightedZones NOTIFY highlightedZonesChanged FINAL)  ///< GeoZoneInfo of the zones drawn highlighted, which filters don't hide
    Q_PROPERTY(QVariantList         keptZones       READ keptZones          WRITE setKeptZones      NOTIFY keptZonesChanged FINAL)      ///< GeoZoneInfo of zones the filters don't hide
    Q_PROPERTY(int                  filteredZoneCount READ filteredZoneCount NOTIFY filteredZoneCountChanged FINAL)                 ///< Zones in view hidden by the filters
    Q_PROPERTY(MissionController*   missionController READ missionController WRITE setMissionController NOTIFY missionControllerChanged FINAL)  ///< Mission whose altitudes the altitude filter uses
    Q_PROPERTY(bool                 vehicleAltitudes READ vehicleAltitudes  WRITE setVehicleAltitudes NOTIFY vehicleAltitudesChanged FINAL)     ///< The altitude filter uses the altitude of the active vehicle

public:
    static constexpr int maxShapeCount = 10000;
    static constexpr double viewMargin = 0.25;  ///< Shapes are prepared this fraction of the view size beyond each edge
    static constexpr int vehicleAltitudeIntervalMSecs = 1000;   ///< Between reads of the altitude of the active vehicle
    static constexpr int missionAltitudeDelayMSecs = 500;       ///< Mission changes settle this long before its altitudes are read
    static constexpr double altitudeTolerance = 1;              ///< Meters the altitudes change by before the zones are filtered again

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
    QVariantList keptZones() const { return _keptZones; }
    void setKeptZones(const QVariantList &zones);
    int filteredZoneCount() const { return _filteredZoneCount; }
    MissionController *missionController() const;
    void setMissionController(MissionController *missionController);
    bool vehicleAltitudes() const { return _vehicleAltitudes; }
    void setVehicleAltitudes(bool vehicleAltitudes);

    /// Altitudes the altitude filter compares the zones with, from the mission or the active vehicle
    const GeoZoneAltitudeRange &altitudeRange() const { return _altitudeRange; }
    /// Sets the altitudes the altitude filter compares the zones with, until the mission or the vehicle replace them
    void setAltitudeRange(const GeoZoneAltitudeRange &altitudeRange);

    /// @return true if the map shows the zone: it passes the filters, or is kept or highlighted
    bool isShown(const GeoZone *zone);

    /// @return Zones the map shows whose horizontal projection contains the coordinate at any altitude, the most
    ///         restrictive and active first, see GeoZoneManager::zonesAt
    Q_INVOKABLE QVariantList zonesAt(const QGeoCoordinate &coordinate);

signals:
    void hiddenZoneCountChanged(int hiddenZoneCount);
    void highlightedZonesChanged();
    void labelZonesChanged();
    void keptZonesChanged();
    void filteredZoneCountChanged();
    void missionControllerChanged();
    void vehicleAltitudesChanged();

private slots:
    void _clear();
    void _removeSource(GeoZoneSource *source);
    void _refresh();
    void _updateActive();
    void _updateFilter();
    void _updateAltitudes();

private:
    void _syncItems(QmlObjectListModel *model, const QList<const GeoZoneShape*> &shapes, int zoomLevel);
    void _setHiddenZoneCount(int hiddenZoneCount);
    void _setFilteredZoneCount(int filteredZoneCount);
    /// Drops the filter results, after the filters, altitudes, times or zones changed
    void _invalidateFilter();
    void _updateTimers();
    /// @return Altitudes of the mission or the active vehicle, invalid without them
    GeoZoneAltitudeRange _readAltitudes() const;

    GeoZoneManager *_manager = nullptr;
    QmlObjectListModel *_polygons = nullptr;
    QmlObjectListModel *_circles = nullptr;
    QVariantList _labelZones;
    QHash<const GeoZoneShape*, GeoZoneMapItem*> _items;
    int _hiddenZoneCount = 0;
    QVariantList _highlightedZones;
    QSet<const GeoZone*> _highlightedZoneSet;
    QVariantList _keptZones;
    QSet<const GeoZone*> _keptZoneSet;
    int _filteredZoneCount = 0;

    GeoZoneMapFilter _filter;
    QHash<const GeoZone*, bool> _accepted;      ///< Filter results of the zones met since the filters last changed
    QDateTime _filterTime;                      ///< UTC time the schedule filter looks ahead from
    GeoZoneAltitudeRange _altitudeRange;
    QPointer<MissionController> _missionController;
    bool _vehicleAltitudes = false;
    QTimer _altitudeTimer;                      ///< Reads the altitudes of the mission after changes settle, or of the vehicle periodically
    QTimer _scheduleTimer;                      ///< Filters zones active soon again as time passes

    QGeoRectangle _visibleRegion;
    double _zoomLevel = 0;
    bool _showLabels = false;
};
