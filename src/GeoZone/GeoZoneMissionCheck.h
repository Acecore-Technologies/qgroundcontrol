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

class GeoZone;
class GeoZoneManager;
class MissionController;

/// Checks whether the planned mission's flight path enters restricted geozones. Altitudes and zone
/// schedules are not considered, since the mission may be flown at any time.
class GeoZoneMissionCheck : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")

    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged)
    Q_PROPERTY(QList<QObject*>      crossedZones        READ crossedZones                                   NOTIFY crossedZonesChanged)
    Q_PROPERTY(QString              warningText         READ warningText                                    NOTIFY crossedZonesChanged)

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

    /// Re-evaluates immediately instead of after changes settle
    void update();

    /// Splits flight path segments into connected paths. Legs longer than maxStepDistance get intermediate
    /// points along the great circle.
    static QList<QList<QGeoCoordinate>> pathsFromSegments(const QList<std::pair<QGeoCoordinate, QGeoCoordinate>> &segments);

signals:
    void missionControllerChanged();
    void crossedZonesChanged();

private:
    void _scheduleUpdate();
    void _clearZones();
    QList<std::pair<QGeoCoordinate, QGeoCoordinate>> _missionSegments() const;

    GeoZoneManager *_manager = nullptr;
    QPointer<MissionController> _missionController;
    QList<GeoZone*> _crossedZones;
    QTimer _updateTimer;

    static constexpr int _updateDelayMSecs = 500;
};
