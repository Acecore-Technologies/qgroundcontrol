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
#include <QtCore/QSet>
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

    /// Shapes shown, drawn by GeoZoneMapLayer
    const QSet<const GeoZoneShape*> &shapes() const { return _shapes; }
    /// Zoom level the polygons are drawn at, see GeoZoneShape::mercatorRings
    int detailLevel() const { return _detailLevel; }
    /// @return true if the zone is one of the highlightedZones
    bool highlighted(const GeoZone *zone) const { return _highlightedZoneSet.contains(zone); }
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
    /// The shapes, their detail level, or whether their zones are active or highlighted changed. The shapes left out
    /// may be deleted once it returns.
    void shapesChanged();
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
    void _setHiddenZoneCount(int hiddenZoneCount);
    void _setFilteredZoneCount(int filteredZoneCount);
    /// Drops the filter results, after the filters, altitudes, times or zones changed
    void _invalidateFilter();
    void _updateTimers();
    /// @return Altitudes of the mission or the active vehicle, invalid without them
    GeoZoneAltitudeRange _readAltitudes() const;

    GeoZoneManager *_manager = nullptr;
    QSet<const GeoZoneShape*> _shapes;
    int _detailLevel = 0;
    QVariantList _labelZones;
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
