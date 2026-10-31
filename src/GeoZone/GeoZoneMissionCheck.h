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

class GeoZoneManager;
class MissionController;

/// Checks whether the planned mission's flight path enters restricted geozones, considering the planned
/// altitudes. U-space airspaces and no restriction zones are reported separately, they never hide
/// restricted zones. Zone schedules are not considered, since the mission may be flown at any time.
class GeoZoneMissionCheck : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")

    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged)
    Q_PROPERTY(QList<QObject*>      crossedZones        READ crossedZones                                   NOTIFY crossedZonesChanged)
    Q_PROPERTY(QString              warningText         READ warningText                                    NOTIFY crossedZonesChanged)
    Q_PROPERTY(QGeoRectangle        crossedZonesBoundingBox READ crossedZonesBoundingBox                        NOTIFY crossedZonesChanged)
    Q_PROPERTY(QList<QObject*>      uSpaceZones         READ uSpaceZones                                    NOTIFY crossedZonesChanged)     ///< U-space airspaces the mission enters
    Q_PROPERTY(QList<QObject*>      permittedZones      READ permittedZones                                 NOTIFY crossedZonesChanged)     ///< No restriction zones the mission enters
    Q_PROPERTY(QString              infoText            READ infoText                                       NOTIFY crossedZonesChanged)

public:
    /// @param manager Zones to check, GeoZoneManager::instance() when null
    explicit GeoZoneMissionCheck(QObject *parent = nullptr, GeoZoneManager *manager = nullptr);

    static constexpr int maxListedZones = 5;
    static constexpr double maxStepDistance = 1000;     ///< Meters between points along long legs
    static constexpr int maxStepsPerSegment = 10000;

    MissionController *missionController() const;
    void setMissionController(MissionController *missionController);

    QList<QObject*> crossedZones() const;
    QString warningText() const;
    QList<QObject*> uSpaceZones() const { return QList<QObject*>(_uSpaceZones.cbegin(), _uSpaceZones.cend()); }
    QList<QObject*> permittedZones() const { return QList<QObject*>(_permittedZones.cbegin(), _permittedZones.cend()); }
    /// U-space airspaces and no restriction zones entered, which don't prevent the flight
    QString infoText() const;
    QGeoRectangle crossedZonesBoundingBox() const;

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

private:
    void _scheduleUpdate();
    void _clearZones();
    QList<Segment> _missionSegments() const;
    /// Survey, corridor and structure scan areas
    QList<GeoZone::Area> _missionAreas() const;

    GeoZoneManager *_manager = nullptr;
    QPointer<MissionController> _missionController;
    QList<GeoZone*> _crossedZones;
    QList<GeoZone*> _uSpaceZones;
    QList<GeoZone*> _permittedZones;
    QTimer _updateTimer;

    static constexpr int _updateDelayMSecs = 500;
};
