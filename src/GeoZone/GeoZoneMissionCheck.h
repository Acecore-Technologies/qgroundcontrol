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
#include <QtCore/QTimer>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include "GeoZone.h"
#include "GeoZoneInfo.h"
#include "GeoZoneMissionPath.h"

class GeoFenceController;
class GeoZoneCatalog;
class GeoZoneManager;
class GeoZoneSource;
class MissionController;

/// Checks whether the planned mission's flight path enters restricted geozones, considering the planned
/// altitudes. U-space airspaces and no restriction zones are reported separately, they never hide
/// restricted zones. With a planned start in GeoZoneManager, only zones active while the mission flies through them
/// are reported, estimating the times from the mission duration with a tolerance, see
/// GeoZonePathEvaluator::defaultScheduleToleranceSecs, otherwise zone schedules are not considered since
/// the mission may be flown at any time. Zones authorised for the whole flight, or from now on without a planned
/// start, are listed as information. See GeoZonePathEvaluator.
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

    /// @return true if exclusion fences of the controller cover the whole zone, like the fences GeoZoneFenceImporter
    ///         adds for it with any simplification, see GeoZoneFenceImporter::fencesCover
    static bool fenceExcludes(GeoFenceController *geoFenceController, const GeoZone *zone);

    /// @return Start and end of the planned flight from the planned start of the manager and the mission duration,
    ///         invalid without a planned start
    static std::pair<QDateTime, QDateTime> plannedFlightTimes(const GeoZoneManager *manager, const MissionController *missionController);

    /// Re-evaluates immediately on this thread instead of after changes settle on another one
    Q_INVOKABLE void update();

    /// Zones a mission path enters
    struct Evaluation {
        QList<const GeoZone*> zones;                    ///< Most restrictive first
        QHash<const GeoZone*, QStringList> pathLabels;  ///< Labels of the paths entering zones the drawn flight path doesn't
        QList<const GeoZone*> inactiveZones;            ///< Zones of zones which aren't active while the mission flies through them
        QDateTime start;                                ///< Of the flight evaluated
        QDateTime end;
    };

    /// @return Zones the path or its areas enter whatever their authorisations, for a flight from start to end or at
    ///         any time without start. Can run on any thread with a copy of the catalog.
    static Evaluation evaluate(const GeoZoneCatalog &catalog, const GeoZoneMissionPath &path, const QDateTime &start = QDateTime(),
                               const QDateTime &end = QDateTime());

    /// @return Labels of the paths besides the drawn flight path entering the zone, when only those do
    QStringList pathLabels(const GeoZone *zone) const { return _pathLabels.value(zone); }

signals:
    void missionControllerChanged();
    void crossedZonesChanged();
    void flightNowChanged();
    void geoFenceControllerChanged();

private:
    void _scheduleUpdate();
    void _clearZones(GeoZoneSource *source);
    void _connectMissionItems();
    /// Evaluates the mission path on another thread, the result is dropped when the mission or zones change meanwhile
    void _updateConcurrently();
    /// @return Start and end of the flight checked, now with flightNow, invalid to check it at any time
    std::pair<QDateTime, QDateTime> _flightTimes() const;
    /// Sorts the zones on the path by the policies of the planned flight
    void _apply(const Evaluation &evaluation);

    GeoZoneManager *_manager = nullptr;
    QPointer<MissionController> _missionController;
    QList<const GeoZone*> _crossedZones;
    QList<const GeoZone*> _uSpaceZones;
    QList<const GeoZone*> _permittedZones;
    QList<const GeoZone*> _authorisedZones;
    int _inactiveZoneCount = 0;
    bool _flightNow = false;
    QPointer<GeoFenceController> _geoFenceController;
    QList<const GeoZone*> _unfencedZones;
    QHash<const GeoZone*, QStringList> _pathLabels;
    QTimer _updateTimer;
    quint64 _evaluation = 0;    ///< Incremented when an evaluation starts or the zones change, outdating running ones

    static constexpr int _updateDelayMSecs = 500;
};
