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
class GeoZoneAuthorisations;
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
/// the mission may be flown at any time. Zones authorised for the whole flight, or now without a planned start, are
/// listed as information, those whose authorisation is for another time are warned about with a note. See
/// GeoZonePathEvaluator.
/// The returns the vehicle would fly from along the mission after a failsafe or a fence breach, see
/// GeoZoneMissionPath::contingencyPaths, are checked too. Restricted zones only they enter are listed apart as
/// contingency zones, which don't need a confirmation for the upload, with the items they would be flown from.
/// The mission is checked again when the return parameters of the vehicle, the rally points or the breach return
/// point change.
/// The zones relevant to the mission are compared with the geozone data the open plan was saved with, see
/// GeoZonePlanning::planAudit and GeoZonePlanAudit.
class GeoZoneMissionCheck : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")
    Q_MOC_INCLUDE("GeoFenceController.h")

    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged FINAL)
    Q_PROPERTY(QVariantList         zones               READ zonesInfo                                      NOTIFY zonesChanged FINAL)            ///< The entries as maps of the GeoZoneInfo "zone", the Kinds "kind", the "labels" and the "approximationTexts"
    Q_PROPERTY(QString              warningText         READ warningText                                    NOTIFY zonesChanged FINAL)
    Q_PROPERTY(QGeoRectangle        crossedZonesBoundingBox READ crossedZonesBoundingBox                        NOTIFY zonesChanged FINAL)
    Q_PROPERTY(QString              infoText            READ infoText                                       NOTIFY zonesChanged FINAL)
    Q_PROPERTY(GeoFenceController*  geoFenceController  READ geoFenceController WRITE setGeoFenceController NOTIFY geoFenceControllerChanged FINAL)   ///< Fence expected to exclude the prohibited zones
    Q_PROPERTY(QString              fenceText           READ fenceText                                      NOTIFY zonesChanged FINAL)
    Q_PROPERTY(QString              contingencyText     READ contingencyText                                NOTIFY zonesChanged FINAL)
    Q_PROPERTY(QString              auditText           READ auditText                                      NOTIFY auditChanged FINAL)            ///< How the geozone data changed since the plan was saved, empty without changes
    Q_PROPERTY(QStringList          auditDetails        READ auditDetails                                   NOTIFY auditChanged FINAL)            ///< One line per change

public:
    explicit GeoZoneMissionCheck(QObject *parent = nullptr);
    /// @param manager Zones to check, GeoZoneManager::instance() when null
    explicit GeoZoneMissionCheck(GeoZoneManager *manager, QObject *parent = nullptr);
    ~GeoZoneMissionCheck() override;

    static constexpr int maxListedZones = 5;

    /// How a zone the mission or the returns from along it enter applies to the planned flight
    enum Kind {
        Crossed = 0x01,     ///< Restricted zone the mission enters
        Unfenced = 0x02,    ///< Crossed prohibited zone which the fence doesn't exclude, with a fence supported
        USpace = 0x04,      ///< U-space airspace the mission enters
        Permitted = 0x08,   ///< No restriction zone the mission enters
        Authorised = 0x10,  ///< Entered with an authorisation covering the flight, see GeoZoneAuthorisations
        Contingency = 0x20, ///< Restricted zone only the returns from along the mission enter
        Inactive = 0x40,    ///< Entered while not active during the flight
    };
    Q_ENUM(Kind)
    Q_DECLARE_FLAGS(Kinds, Kind)

    /// Zone the check lists. A zone entered while inactive can also have a contingency entry.
    struct Entry {
        const GeoZone *zone = nullptr;
        Kinds kinds;
        /// Labels of the paths besides the drawn flight path entering the zone when only those do, or of the
        /// contingency paths entering it with the items they start from, like "a return to launch from items 3–7"
        QStringList labels;
        GeoZonePathEvaluator::Approximations approximations;    ///< Why the check of the zone entered is approximate
        /// Why the operator's authorisation of a crossed zone doesn't cover the flight, like "authorised from
        /// 2026-12-01", empty without
        QString authorisationNote;
        friend bool operator==(const Entry &a, const Entry &b) = default;
    };

    MissionController *missionController() const;
    void setMissionController(MissionController *missionController);

    /// Zones of each kind in the order of the evaluation, most restrictive first
    const QList<Entry> &entries() const { return _entries; }
    QVariantList zonesInfo() const;
    /// @return The zones of the entries of the kind
    QList<const GeoZone*> zones(Kind kind) const { return zones(_entries, kind); }
    static QList<const GeoZone*> zones(const QList<Entry> &entries, Kind kind);
    /// @return The first entry of the zone, null without
    const Entry *entry(const GeoZone *zone) const;
    /// Restricted zones the mission enters, with the paths entering them and the approximations
    QString warningText() const { return warningText(_entries); }
    static QString warningText(const QList<Entry> &entries);
    /// @return A line per entry of the kind with an authorisation note, like "CTR: authorised from 2026-12-01."
    static QStringList authorisationNotes(const QList<Entry> &entries, Kind kind);
    /// U-space airspaces and no restriction zones entered, which don't prevent the flight
    QString infoText() const;
    QGeoRectangle crossedZonesBoundingBox() const;
    GeoFenceController *geoFenceController() const;
    void setGeoFenceController(GeoFenceController *geoFenceController);
    /// Names the unfenced zones
    QString fenceText() const { return fenceText(_entries); }
    static QString fenceText(const QList<Entry> &entries);
    /// Which returns from which items enter the contingency zones, like "A return to launch from items 3–7 would enter X."
    QString contingencyText() const;

    /// Zones the mission and the returns from along it enter, whether or not they apply then, see GeoZonePlanAudit
    const QList<const GeoZone*> &relevantZones() const { return _relevantZones; }
    /// @return The record of the zones relevant to the mission and the sources covering it, for the plan file
    GeoZonePlanAudit::Record auditRecord() const;
    /// @return The record of the zones relevant to a mission and the manager's sources covering its area
    static GeoZonePlanAudit::Record auditRecord(const GeoZoneManager *manager, const QList<const GeoZone*> &relevantZones, const GeoZoneBox &area);
    QString auditText() const { return _auditText; }
    QStringList auditDetails() const { return _auditDetails; }
    /// Accepts the changes of the geozone data since the plan was saved, until it changes again
    Q_INVOKABLE void acknowledgeAudit();

    /// @return Why uploading the mission needs the pilot's confirmation, one paragraph per reason, empty when it doesn't:
    ///         the mission enters prohibited zones without an authorisation covering the flight, naming those the fence
    ///         doesn't exclude, the mission has items and the data is outdated and treated as invalid (GeoZoneSourceRepository::dataInvalid),
    ///         or the geozone data changed since the plan was saved and the changes weren't acknowledged, with the
    ///         changes. Of the last check, see update().
    Q_INVOKABLE QStringList uploadReasons() const;

    static constexpr int maxListedChanges = 5;   ///< Changes of the geozone data listed by uploadReasons

    /// @return true if exclusion fences of the controller cover the whole zone, like the fences GeoZoneFenceImporter
    ///         adds for it with any simplification, see GeoZoneFences::fencesCover
    static bool fenceExcludes(GeoFenceController *geoFenceController, const GeoZone *zone);

    /// @return Start and end of the planned flight from the planned start of the manager and the mission duration,
    ///         invalid without a planned start
    static std::pair<QDateTime, QDateTime> plannedFlightTimes(const GeoZoneManager *manager, const MissionController *missionController);

    /// @return true if the result is that of the current mission, zones and planned start: no change waits to be checked
    bool settled() const;
    /// @return A check of the planned flight of the controller's mission against the manager's zones whose result is
    ///         settled, like the Plan View's, null without
    static const GeoZoneMissionCheck *settledCheck(const GeoZoneManager *manager, const MissionController *missionController);

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
        QHash<const GeoZone*, GeoZonePathEvaluator::Approximations> approximations;    ///< Of the zones entered whose results are approximate
        /// Of the zones entered with an authorisation limiting the area or altitude: why the flight leaves its scope,
        /// empty when it stays within it, see GeoZonePathEvaluator::scopeIssue
        QHash<const GeoZone*, QString> scopeIssues;
    };

    /// @return Zones the path or its areas enter whatever their authorisations, for a flight from start to end or at
    ///         any time without start, with the scope issues of the authorisations. Can run on any thread with copies
    ///         of the catalog and the authorisations.
    static Evaluation evaluate(const GeoZoneCatalog &catalog, const GeoZoneMissionPath &path, const QDateTime &start = QDateTime(),
                               const QDateTime &end = QDateTime(), const GeoZoneAuthorisations *authorisations = nullptr);
    /// @return The evaluation of the planned flight of the controller's mission with the manager's zones and planned
    ///         start, on the calling thread without side effects, like setting the area of interest
    static Evaluation evaluatePlan(const GeoZoneManager *manager, MissionController *missionController);

    /// @return The zones of the evaluation by how they apply during its flight, unfenced ones only with a fence
    static QList<Entry> classify(const Evaluation &evaluation, const GeoZoneAuthorisations *authorisations,
                                 GeoFenceController *geoFenceController = nullptr);
    /// @return The labels of the contingency paths the evaluator result has entering zones while active, grouped by
    ///         label with the items they start from, like "a return to launch from items 3–7", by zone
    static QHash<const GeoZone*, QStringList> groupContingencies(const GeoZoneMissionPath &path, const GeoZonePathEvaluator::Result &result);
    /// @return Bounds of the flight path and pattern areas of the mission, invalid without, see GeoZoneManager::setMissionArea
    static QGeoRectangle missionArea(const GeoZoneMissionPath &path);

signals:
    void missionControllerChanged();
    void zonesChanged();
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
    /// Sorts the zones on the path by the policies of the planned flight
    void _apply(const Evaluation &evaluation);
    /// Compares the relevant zones with the data the plan was saved with
    void _updateAudit();

    GeoZoneManager *_manager = nullptr;
    GeoZoneMissionWorker *_worker = nullptr;    ///< Follows the mission, evaluates it and drops outdated results
    QList<Entry> _entries;
    QPointer<GeoFenceController> _geoFenceController;
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

Q_DECLARE_OPERATORS_FOR_FLAGS(GeoZoneMissionCheck::Kinds)
