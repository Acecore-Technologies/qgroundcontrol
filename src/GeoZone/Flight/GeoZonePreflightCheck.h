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
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>

#include <optional>

#include "GeoZoneAwareness.h"
#include "GeoZoneFenceUpload.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneRemoteId.h"

class GeoZone;
class GeoZoneManager;
class GeoZoneMonitor;
class GeoZoneVehicleMonitor;
class PlanMasterController;
class Vehicle;

/// The automatic geozone items of the pre-flight checklist, see GeoZonePreFlightCheckList.qml, for the mission and fence
/// of the vehicle in the Fly View's plan controller:
/// - data: no geozone source is outdated, and zones are loaded
/// - mission: it enters no restricted zone, or only those authorised for the planned time, see GeoZoneMissionCheck
/// - fence: the vehicle's fence is the one an upload from geozones verified, built from geozone data which hasn't
///   changed since, and it excludes the prohibited zones the mission enters, see GeoZoneFenceUpload::verification. The
///   zones its import left out are listed as a warning.
/// - QNH: set when limits published as flight levels are near the mission altitudes
/// - Remote ID: ready, with the operator ID set, when the mission enters U-space airspace, see GeoZoneRemoteId
/// - take-off position: the vehicle on the ground isn't inside a prohibited zone, or a restricted zone the pilot hasn't
///   confirmed the permission for, see GeoZoneVehicleMonitor::groundWarning
/// - geo-awareness: available for the vehicle, see GeoZoneAwareness
/// - permits: the pilot acknowledged the permits of the zones requiring an authorisation or a notification which the
///   mission enters or the vehicle is in, see GeoZonePermits and permitZones
/// Each item is a map with its state and the text explaining it.
class GeoZonePreflightCheck : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("PlanMasterController.h")

    Q_PROPERTY(PlanMasterController* planController READ planController WRITE setPlanController NOTIFY planControllerChanged FINAL)
    Q_PROPERTY(QVariantMap  dataItem        READ dataItem       NOTIFY itemsChanged FINAL)
    Q_PROPERTY(QVariantMap  missionItem     READ missionItem    NOTIFY itemsChanged FINAL)
    Q_PROPERTY(QVariantMap  fenceItem       READ fenceItem      NOTIFY itemsChanged FINAL)
    Q_PROPERTY(QVariantMap  qnhItem         READ qnhItem        NOTIFY itemsChanged FINAL)
    Q_PROPERTY(QVariantMap  remoteIdItem    READ remoteIdItem   NOTIFY itemsChanged FINAL)
    Q_PROPERTY(QVariantMap  takeoffItem     READ takeoffItem    NOTIFY itemsChanged FINAL)
    Q_PROPERTY(QVariantMap  awarenessItem   READ awarenessItem  NOTIFY itemsChanged FINAL)
    Q_PROPERTY(QVariantMap  permitItem      READ permitItem     NOTIFY itemsChanged FINAL)
    /// Zones of the permit item, as maps with zone (GeoZoneInfo), name, restrictionText, atVehicle (the vehicle is in
    /// it, otherwise the mission enters it) and permitted
    Q_PROPERTY(QVariantList permitZones     READ permitZones    NOTIFY itemsChanged FINAL)

public:
    /// Checks the vehicles of GeoZoneMonitor::instance()
    explicit GeoZonePreflightCheck(QObject *parent = nullptr);
    /// @param manager Zones and sources checked, GeoZoneManager::instance() when null
    explicit GeoZonePreflightCheck(GeoZoneManager *manager, QObject *parent = nullptr);

    enum State {
        Checking,       ///< Waiting for the mission check, not passed
        NotApplicable,  ///< Nothing to check, passed
        Passed,
        Warning,        ///< Failed, the pilot may accept it
        Failed,         ///< Failed, the pilot can't accept it
    };
    Q_ENUM(State)

    struct Item {
        State state = Checking;
        QString text;   ///< Why, or what passed

        QVariantMap toMap() const;
        friend bool operator==(const Item &, const Item &) = default;
    };

    /// Meters vertically from the mission altitudes within which flight level limits need the local QNH
    static constexpr double qnhVerticalMargin = 300;
    /// Meters horizontally around the mission within which flight level limits are near
    static constexpr double qnhHorizontalMargin = 1000;

    PlanMasterController *planController() const;
    void setPlanController(PlanMasterController *planController);

    const Item &data() const { return _data; }
    const Item &mission() const { return _mission; }
    const Item &fence() const { return _fence; }
    const Item &qnh() const { return _qnh; }
    const Item &remoteId() const { return _remoteId; }
    QVariantMap dataItem() const { return _data.toMap(); }
    QVariantMap missionItem() const { return _mission.toMap(); }
    QVariantMap fenceItem() const { return _fence.toMap(); }
    QVariantMap qnhItem() const { return _qnh.toMap(); }
    QVariantMap remoteIdItem() const { return _remoteId.toMap(); }
    const Item &takeoff() const { return _takeoff; }
    QVariantMap takeoffItem() const { return _takeoff.toMap(); }
    const Item &awareness() const { return _awareness; }
    QVariantMap awarenessItem() const { return _awareness.toMap(); }
    const Item &permit() const { return _permit; }
    QVariantMap permitItem() const { return _permit.toMap(); }
    QVariantList permitZones() const;

    /// Monitor of the vehicle whose take-off position is checked, none by default
    void setMonitor(GeoZoneMonitor *monitor);

    /// Checks again now instead of after the changes settle
    Q_INVOKABLE void update();
    /// The mission check, of the plan controller's mission and fence
    const GeoZoneMissionCheck *missionCheck() const { return _missionCheck; }

    /// @param zoneCount Zones loaded
    /// @param validityWarning Sources with outdated data, see GeoZoneSourceRepository::validityWarning
    /// @param dataInvalid The outdated data is treated as invalid, see GeoZoneSourceRepository::dataInvalid
    static Item dataItem(int zoneCount, const QString &validityWarning, bool dataInvalid = false);
    /// @param itemCount Mission items, 0 without mission
    static Item missionItem(int itemCount, const QList<GeoZoneMissionCheck::Entry> &entries, const QDateTime &plannedStart);
    /// What the fence item checks
    struct FenceInput {
        GeoZoneFenceUpload::FenceSnapshot vehicleFence;     ///< Stored by the vehicle
        std::optional<GeoZoneFenceUpload::Verification> verification;
        QList<GeoZoneSourceVersion> sources;                ///< Current versions of the sources
        QList<const GeoZone*> unfenced;                     ///< Prohibited zones the mission enters which the fence doesn't exclude
    };
    static Item fenceItem(const FenceInput &input);
    /// @param flightLevelZones Zones with flight level limits near the mission altitudes
    /// @param qnhHPa QNH setting
    static Item qnhItem(const QList<const GeoZone*> &flightLevelZones, double qnhHPa);
    /// @param uSpace U-space airspaces the mission enters
    static Item remoteIdItem(const QList<const GeoZone*> &uSpace, const GeoZoneRemoteId::Status &status);

    /// @param monitor Of the vehicle, null without
    static Item takeoffItem(const GeoZoneVehicleMonitor *monitor);
    static Item awarenessItem(const GeoZoneAwareness::Status &status);
    /// Zone of the permit item
    struct PermitZone {
        const GeoZone *zone = nullptr;
        bool atVehicle = false;
        bool permitted = false;

        friend bool operator==(const PermitZone &, const PermitZone &) = default;
    };
    static Item permitItem(const QList<PermitZone> &zones);

    /// @return Zones with a flight level limit within qnhVerticalMargin of the altitudes of the mission path, any when
    ///         they are unknown, and within qnhHorizontalMargin of it
    static QList<const GeoZone*> flightLevelZonesNear(const GeoZoneCatalog &catalog, const GeoZoneMissionPath &path);

signals:
    void planControllerChanged();
    void itemsChanged();

private:
    void _connectPlan();
    void _connectVehicle();
    /// Follows the monitor of the vehicle, which may be added after the vehicle
    void _connectMonitor();
    GeoZoneVehicleMonitor *_vehicleMonitor() const;
    void _scheduleUpdate();

    GeoZoneManager *_manager = nullptr;
    GeoZoneMissionCheck *_missionCheck = nullptr;
    QPointer<PlanMasterController> _planController;
    QPointer<Vehicle> _vehicle;
    QObject *_vehicleConnections = nullptr;     ///< Receiver of the vehicle's signals, replaced to disconnect them
    QPointer<GeoZoneMonitor> _monitor;
    QObject *_monitorConnections = nullptr;     ///< Receiver of the signals of the vehicle's monitor
    QTimer _updateTimer;

    Item _data;
    Item _mission;
    Item _fence;
    Item _qnh;
    Item _remoteId;
    Item _takeoff;
    Item _awareness;
    Item _permit;
    QList<PermitZone> _permitZones;
};
