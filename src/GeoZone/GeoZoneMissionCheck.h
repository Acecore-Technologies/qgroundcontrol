/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include "GeoZone.h"
#include "GeoZoneInfo.h"

class GeoFenceController;
class GeoZoneManager;
class GeoZoneSource;
class MissionController;

/// Checks whether the planned mission's flight path enters restricted geozones, considering the planned
/// altitudes. U-space airspaces and no restriction zones are reported separately, they never hide
/// restricted zones. With a planned start in GeoZoneManager, only zones active at some time during the mission
/// are reported, otherwise zone schedules are not considered since the mission may be flown at any time.
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
    static constexpr double maxStepDistance = 1000;     ///< Meters between points along long legs
    static constexpr int maxStepsPerSegment = 10000;

    MissionController *missionController() const;
    void setMissionController(MissionController *missionController);

    /// Restricted zones the mission enters, most restrictive first
    const QList<GeoZone*> &crossedZones() const { return _crossedZones; }
    QVariantList crossedZonesInfo() const { return GeoZoneInfo::toVariantList(_crossedZones); }
    QString warningText() const;
    const QList<GeoZone*> &uSpaceZones() const { return _uSpaceZones; }
    QVariantList uSpaceZonesInfo() const { return GeoZoneInfo::toVariantList(_uSpaceZones); }
    const QList<GeoZone*> &permittedZones() const { return _permittedZones; }
    QVariantList permittedZonesInfo() const { return GeoZoneInfo::toVariantList(_permittedZones); }
    /// U-space airspaces and no restriction zones entered, which don't prevent the flight
    QString infoText() const;
    QGeoRectangle crossedZonesBoundingBox() const;
    int inactiveZoneCount() const { return _inactiveZoneCount; }
    bool flightNow() const { return _flightNow; }
    void setFlightNow(bool flightNow);
    GeoFenceController *geoFenceController() const;
    void setGeoFenceController(GeoFenceController *geoFenceController);
    const QList<GeoZone*> &unfencedZones() const { return _unfencedZones; }
    QVariantList unfencedZonesInfo() const { return GeoZoneInfo::toVariantList(_unfencedZones); }
    QString fenceText() const;

    /// @return true if exclusion fences of the controller cover the whole zone, using the outer boundaries of its polygons
    static bool fenceExcludes(GeoFenceController *geoFenceController, const GeoZone *zone);

    /// @return Start and end of the planned flight from the planned start of the manager and the mission duration,
    ///         invalid without a planned start
    static std::pair<QDateTime, QDateTime> plannedFlightTimes(const GeoZoneManager *manager, const MissionController *missionController);

    /// Re-evaluates immediately instead of after changes settle
    Q_INVOKABLE void update();

    /// Straight flight path segment between two mission items
    struct Segment {
        QGeoCoordinate from;
        QGeoCoordinate to;
        double fromAltitudeAMSL = qQNaN();
        double toAltitudeAMSL = qQNaN();
        QList<double> terrainAMSL;  ///< Ground elevation every terrainSpacing meters from the start, the last sample at the end
        double terrainSpacing = 0;
    };

    /// Splits flight path segments into connected paths with a position at each terrain sample, or along the great
    /// circle every maxStepDistance without terrain samples. Altitudes are interpolated.
    static QList<QList<GeoZone::Position>> pathsFromSegments(const QList<Segment> &segments);

signals:
    void missionControllerChanged();
    void crossedZonesChanged();
    void flightNowChanged();
    void geoFenceControllerChanged();

private:
    void _scheduleUpdate();
    void _clearZones(GeoZoneSource *source);
    QList<Segment> _missionSegments() const;
    /// Survey, corridor and structure scan areas
    QList<GeoZone::Area> _missionAreas() const;

    GeoZoneManager *_manager = nullptr;
    QPointer<MissionController> _missionController;
    QList<GeoZone*> _crossedZones;
    QList<GeoZone*> _uSpaceZones;
    QList<GeoZone*> _permittedZones;
    int _inactiveZoneCount = 0;
    bool _flightNow = false;
    QPointer<GeoFenceController> _geoFenceController;
    QList<GeoZone*> _unfencedZones;
    QTimer _updateTimer;

    static constexpr int _updateDelayMSecs = 500;
};
