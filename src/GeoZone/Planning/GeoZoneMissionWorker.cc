/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMissionWorker.h"
#include "MissionController.h"

GeoZoneMissionWorker::GeoZoneMissionWorker(int delayMSecs, QObject *parent)
    : QObject(parent)
{
    _timer.setSingleShot(true);
    _timer.setInterval(delayMSecs);
    (void) connect(&_timer, &QTimer::timeout, this, &GeoZoneMissionWorker::triggered);
}

GeoZoneMissionWorker::~GeoZoneMissionWorker()
{
    // Running evaluations end on their own, their watchers are deleted with this
    _cancel.cancel();
}

MissionController *GeoZoneMissionWorker::missionController() const
{
    return _missionController;
}

bool GeoZoneMissionWorker::setMissionController(MissionController *missionController)
{
    if (missionController == _missionController) {
        return false;
    }
    if (_missionController) {
        (void) disconnect(_missionController, nullptr, this, nullptr);
    }
    _missionController = missionController;
    if (_missionController) {
        // Emitted after the flight path or any item coordinate changes, or the mission is replaced
        (void) connect(_missionController, &MissionController::recalcTerrainProfile, this, &GeoZoneMissionWorker::schedule);
        (void) connect(_missionController, &MissionController::visualItemsChanged, this, &GeoZoneMissionWorker::schedule);
        (void) connect(_missionController, &MissionController::missionTimeChanged, this, &GeoZoneMissionWorker::schedule);
        (void) connect(_missionController, &MissionController::plannedHomePositionChanged, this, &GeoZoneMissionWorker::schedule);
    }
    return true;
}

void GeoZoneMissionWorker::setActive(bool active)
{
    _active = active;
    if (!_active) {
        _timer.stop();
    }
}

void GeoZoneMissionWorker::schedule()
{
    if (_active) {
        _timer.start();
    }
}

void GeoZoneMissionWorker::trigger()
{
    _timer.stop();
    emit triggered();
}

void GeoZoneMissionWorker::invalidate()
{
    _generation++;
    _running = 0;
    _cancel.cancel();
    _cancel = GeoZoneCancelToken();
}

void GeoZoneMissionWorker::stop()
{
    _timer.stop();
    invalidate();
}
