/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include "GeoZone.h"
#include "GeoZoneInfo.h"
#include "GeoZoneMissionPath.h"
#include "GeoZonePlanAudit.h"

class GeoFenceController;
class GeoZoneCatalog;
class GeoZoneManager;
class GeoZoneMissionWorker;
class GeoZoneSource;
class MissionController;
class PlanMasterController;
class Vehicle;

/// Checks whether the planned mission's flight path enters restricted geozones, considering the planned
/// altitudes. U-space airspaces and no restriction zones are reported separately, they never hide
/// restricted zones. With a planned start in GeoZoneManager, only zones active while the mission flies through them
/// are reported, estimating the times from the mission duration with a tolerance, see
/// GeoZonePathEvaluator::defaultScheduleToleranceSecs, otherwise zone schedules are not considered since
/// the mission may be flown at any time. Zones authorised for the whole flight, or from now on without a planned
/// start, are listed as information. See GeoZonePathEvaluator.
/// The returns the vehicle would fly from along the mission after a failsafe or a fence breach, see
/// GeoZoneMissionPath::contingencyPaths, are checked too. Restricted zones only they enter are listed apart as
/// contingency zones, which don't need a confirmation for the upload, with the items they would be flown from.
/// The mission is checked again when the return parameters of the vehicle, the rally points or the breach return
/// point change.
/// Without flightNow, the zones relevant to the mission are compared with the geozone data the open plan was saved
/// with, see GeoZonePlanning::planAudit and GeoZonePlanAudit.
class GeoZoneMissionCheck : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")
    Q_MOC_INCLUDE("GeoFenceController.h")

    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged FINAL)
    Q_PROPERTY(QVariantList         crossedZones        READ crossedZonesInfo                               NOTIFY crossedZonesChanged FINAL)
    Q_PROPERTY(QString              warningText         READ warningText                                    NOTIFY crossedZonesChanged FINAL)
    Q_PROPERTY(QGeoRectangle        crossedZonesBoundingBox READ crossedZonesBoundingBox                        NOTIFY crossedZonesChanged FINAL)
    Q_PROPERTY(QVariantList         uSpaceZones         READ uSpaceZonesInfo                                NOTIFY crossedZonesChanged FINAL)     ///< U-space airspaces the mission enters
    Q_PROPERTY(QVariantList         permittedZones      READ permittedZonesInfo                             NOTIFY crossedZonesChanged FINAL)     ///< No restriction zones the mission enters
    Q_PROPERTY(QVariantList         authorisedZones     READ authorisedZonesInfo                            NOTIFY crossedZonesChanged FINAL)     ///< Zones entered with an authorisation covering the flight, see GeoZoneAuthorisations
    Q_PROPERTY(QString              infoText            READ infoText                                       NOTIFY crossedZonesChanged FINAL)
    Q_PROPERTY(int                  inactiveZoneCount   READ inactiveZoneCount                              NOTIFY crossedZonesChanged FINAL)     ///< Zones entered which aren't active during the planned flight
    Q_PROPERTY(bool                 flightNow           READ flightNow          WRITE setFlightNow          NOTIFY flightNowChanged FINAL)        ///< Checks a flight starting now instead of at the planned start, like a mission on the vehicle
    Q_PROPERTY(GeoFenceController*  geoFenceController  READ geoFenceController WRITE setGeoFenceController NOTIFY geoFenceControllerChanged FINAL)   ///< Fence expected to exclude the prohibited zones
    Q_PROPERTY(QVariantList         unfencedZones       READ unfencedZonesInfo                              NOTIFY crossedZonesChanged FINAL)     ///< Prohibited zones entered which the fence doesn't exclude
    Q_PROPERTY(QString              fenceText           READ fenceText                                      NOTIFY crossedZonesChanged FINAL)
    Q_PROPERTY(QVariantList         contingencyZones    READ contingencyZonesInfo                           NOTIFY crossedZonesChanged FINAL)     ///< Restricted zones only returns from along the mission enter
    Q_PROPERTY(QString              contingencyText     READ contingencyText                                NOTIFY crossedZonesChanged FINAL)
    Q_PROPERTY(QString              auditText           READ auditText                                      NOTIFY auditChanged FINAL)            ///< How the geozone data changed since the plan was saved, empty without changes
    Q_PROPERTY(QStringList          auditDetails        READ auditDetails                                   NOTIFY auditChanged FINAL)            ///< One line per change

public:
    explicit GeoZoneMissionCheck(QObject *parent = nullptr);
    /// @param manager Zones to check, GeoZoneManager::instance() when null
    explicit GeoZoneMissionCheck(GeoZoneManager *manager, QObject *parent = nullptr);

    static constexpr int maxListedZones = 5;

    MissionController *missionController() const;
    void setMissionController(MissionController *missionController);

    /// Restricted zones the mission enters, most restrictive first
    const QList<const GeoZone*> &crossedZones() const { return _crossedZones; }
    QVariantList crossedZonesInfo() const { return GeoZoneInfo::toVariantList(_crossedZones); }
    QString warningText() const;
    const QList<const GeoZone*> &uSpaceZones() const { return _uSpaceZones; }
    QVariantList uSpaceZonesInfo() const { return GeoZoneInfo::toVariantList(_uSpaceZones); }
    const QList<const GeoZone*> &permittedZones() const { return _permittedZones; }
    QVariantList permittedZonesInfo() const { return GeoZoneInfo::toVariantList(_permittedZones); }
    const QList<const GeoZone*> &authorisedZones() const { return _authorisedZones; }
    QVariantList authorisedZonesInfo() const { return GeoZoneInfo::toVariantList(_authorisedZones); }
    /// U-space airspaces and no restriction zones entered, which don't prevent the flight
    QString infoText() const;
    QGeoRectangle crossedZonesBoundingBox() const;
    int inactiveZoneCount() const { return _inactiveZoneCount; }
    bool flightNow() const { return _flightNow; }
    void setFlightNow(bool flightNow);
    GeoFenceController *geoFenceController() const;
    void setGeoFenceController(GeoFenceController *geoFenceController);
    const QList<const GeoZone*> &unfencedZones() const { return _unfencedZones; }
    QVariantList unfencedZonesInfo() const { return GeoZoneInfo::toVariantList(_unfencedZones); }
    QString fenceText() const;
    /// Restricted zones the returns from along the mission enter but the mission doesn't, most restrictive first
    const QList<const GeoZone*> &contingencyZones() const { return _contingencyZones; }
    QVariantList contingencyZonesInfo() const { return GeoZoneInfo::toVariantList(_contingencyZones); }
    /// Which returns from which items enter the contingency zones, like "A return to launch from items 3–7 would enter X."
    QString contingencyText() const;
    /// @return Labels of the contingency paths entering the zone with the items they start from, like "a return to
    ///         launch from items 3–7"
    QStringList contingencyLabels(const GeoZone *zone) const { return _contingencyLabels.value(zone); }

    /// Zones the mission and the returns from along it enter, whether or not they apply then, see GeoZonePlanAudit
    const QList<const GeoZone*> &relevantZones() const { return _relevantZones; }
    /// @return The record of the zones relevant to the mission and the sources covering it, for the plan file
    GeoZonePlanAudit::Record auditRecord() const;
    QString auditText() const { return _auditText; }
    QStringList auditDetails() const { return _auditDetails; }
    /// Accepts the changes of the geozone data since the plan was saved, until it changes again
    Q_INVOKABLE void acknowledgeAudit();

    /// @return true if exclusion fences of the controller cover the whole zone, like the fences GeoZoneFenceImporter
    ///         adds for it with any simplification, see GeoZoneFences::fencesCover
    static bool fenceExcludes(GeoFenceController *geoFenceController, const GeoZone *zone);

    /// @return Start and end of the planned flight from the planned start of the manager and the mission duration,
    ///         invalid without a planned start
    static std::pair<QDateTime, QDateTime> plannedFlightTimes(const GeoZoneManager *manager, const MissionController *missionController);

    /// Re-evaluates immediately on this thread instead of after changes settle on another one
    Q_INVOKABLE void update();
    /// Sets how long changes settle before the mission is checked again, half a second by default
    void setUpdateDelay(int msecs);

    /// Zones a mission path enters
    struct Evaluation {
        QList<const GeoZone*> zones;                    ///< Most restrictive first
        QHash<const GeoZone*, QStringList> pathLabels;  ///< Labels of the paths entering zones the drawn flight path doesn't
        QList<const GeoZone*> inactiveZones;            ///< Zones of zones which aren't active while the mission flies through them
        QDateTime start;                                ///< Of the flight evaluated
        QDateTime end;
        QList<const GeoZone*> contingencyZones;         ///< Entered by contingency paths while active, most restrictive first
        QHash<const GeoZone*, QStringList> contingencyLabels;   ///< Of the contingency paths entering them with their items
        QList<const GeoZone*> relevantZones;            ///< Entered by any path whatever their schedule, see relevantZones()
        GeoZoneBox area;                                ///< Of the paths
    };

    /// @return Zones the path or its areas enter whatever their authorisations, for a flight from start to end or at
    ///         any time without start. Can run on any thread with a copy of the catalog.
    static Evaluation evaluate(const GeoZoneCatalog &catalog, const GeoZoneMissionPath &path, const QDateTime &start = QDateTime(),
                               const QDateTime &end = QDateTime());
    /// @return The labels of the contingency paths the evaluator result has entering zones while active, grouped by
    ///         label with the items they start from, like "a return to launch from items 3–7", by zone
    static QHash<const GeoZone*, QStringList> groupContingencies(const GeoZoneMissionPath &path, const GeoZonePathEvaluator::Result &result);
    /// @return Bounds of the flight path and pattern areas of the mission, invalid without, see GeoZoneManager::setMissionArea
    static QGeoRectangle missionArea(const GeoZoneMissionPath &path);

    /// @return Labels of the paths besides the drawn flight path entering the zone, when only those do
    QStringList pathLabels(const GeoZone *zone) const { return _pathLabels.value(zone); }

signals:
    void missionControllerChanged();
    void crossedZonesChanged();
    void flightNowChanged();
    void geoFenceControllerChanged();
    void auditChanged();

private:
    void _clearZones(GeoZoneSource *source);
    /// Clears the zones of the mission checked, like when the mission controller changes
    void _clearResults();
    void _connectMissionItems();
    /// Connects the plan's rally points and breach return point, which the returns go to
    void _connectPlan();
    void _connectRallyPoints();
    /// Connects the return parameters of the vehicle, see GeoZoneReturnModel::parameterNames
    void _connectVehicle();
    /// Evaluates the mission path on another thread, the result is dropped when the mission or zones change meanwhile
    void _updateConcurrently();
    /// @return Start and end of the flight checked, now with flightNow, invalid to check it at any time
    std::pair<QDateTime, QDateTime> _flightTimes() const;
    /// Sorts the zones on the path by the policies of the planned flight
    void _apply(const Evaluation &evaluation);
    /// Compares the relevant zones with the data the plan was saved with
    void _updateAudit();

    GeoZoneManager *_manager = nullptr;
    GeoZoneMissionWorker *_worker = nullptr;    ///< Follows the mission, evaluates it and drops outdated results
    QList<const GeoZone*> _crossedZones;
    QList<const GeoZone*> _uSpaceZones;
    QList<const GeoZone*> _permittedZones;
    QList<const GeoZone*> _authorisedZones;
    int _inactiveZoneCount = 0;
    bool _flightNow = false;
    QPointer<GeoFenceController> _geoFenceController;
    QList<const GeoZone*> _unfencedZones;
    QHash<const GeoZone*, QStringList> _pathLabels;
    QList<const GeoZone*> _contingencyZones;
    QHash<const GeoZone*, QStringList> _contingencyLabels;
    QList<const GeoZone*> _relevantZones;
    GeoZoneBox _area;
    QString _auditText;
    QStringList _auditDetails;
    QPointer<PlanMasterController> _masterController;
    QPointer<Vehicle> _vehicle;
    QObject *_planConnections = nullptr;        ///< Receiver of the plan's signals, replaced to disconnect them
    QObject *_vehicleConnections = nullptr;     ///< Receiver of the vehicle's signals
    QObject *_rallyConnections = nullptr;       ///< Receiver of the rally points' signals

    static constexpr int _updateDelayMSecs = 500;
};
