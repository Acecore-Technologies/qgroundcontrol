/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMonitor.h"
#include "AudioOutput.h"
#include "GeoZone.h"
#include "GeoZoneManager.h"
#include "MultiVehicleManager.h"
#include "QGCApplication.h"
#include "QGCLoggingCategory.h"
#include "Vehicle.h"

#include <QtCore/qapplicationstatic.h>
#include <QtCore/QSet>
#include <QtPositioning/QGeoCircle>

QGC_LOGGING_CATEGORY(GeoZoneMonitorLog, "qgc.geozone.geozonemonitor")

Q_APPLICATION_STATIC(GeoZoneMonitor, _geoZoneMonitor);

GeoZoneMonitor::GeoZoneMonitor(QObject *parent)
    : GeoZoneMonitor(GeoZoneManager::instance(), parent)
{
    (void) connect(this, &GeoZoneMonitor::alert, this, &GeoZoneMonitor::_showAlert);
    (void) connect(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged, this, &GeoZoneMonitor::_activeVehicleChanged);
    _activeVehicleChanged(MultiVehicleManager::instance()->activeVehicle());
}

GeoZoneMonitor::GeoZoneMonitor(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager)
{
    // Zones are about to be deleted, so forget them
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMonitor::_reset);
}

GeoZoneMonitor *GeoZoneMonitor::instance()
{
    return _geoZoneMonitor();
}

void GeoZoneMonitor::_activeVehicleChanged(Vehicle *vehicle)
{
    if (_vehicle) {
        (void) disconnect(_vehicle, &Vehicle::coordinateChanged, this, &GeoZoneMonitor::_vehicleCoordinateChanged);
    }
    _reset();
    _vehicle = vehicle;
    if (_vehicle) {
        (void) connect(_vehicle, &Vehicle::coordinateChanged, this, &GeoZoneMonitor::_vehicleCoordinateChanged);
    }
}

void GeoZoneMonitor::_vehicleCoordinateChanged(const QGeoCoordinate &coordinate)
{
    if (_lastEvaluate.isValid() && (_lastEvaluate.elapsed() < minEvaluateIntervalMSecs)) {
        return;
    }
    _lastEvaluate.start();
    evaluate(coordinate);
}

void GeoZoneMonitor::_reset()
{
    _states.clear();
    _lastEvaluate.invalidate();
}

void GeoZoneMonitor::evaluate(const QGeoCoordinate &coordinate)
{
    if (!coordinate.isValid()) {
        return;
    }

    static const QList<GeoZone::Restriction> restrictions = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional };
    const QGeoRectangle area = QGeoCircle(coordinate, approachDistance + hysteresisDistance).boundingGeoRectangle();

    QSet<const GeoZone*> nearbyZones;
    for (const GeoZone *zone : _manager->zonesInArea(restrictions, area)) {
        if (!zone->active()) {
            continue;
        }

        const State previous = _states.value(zone, State::Away);
        const double distance = zone->distanceTo(coordinate);
        State state = State::Away;
        if ((distance <= 0) || ((previous == State::Inside) && (distance <= hysteresisDistance))) {
            state = State::Inside;
        } else if ((distance <= approachDistance) || ((previous != State::Away) && (distance <= (approachDistance + hysteresisDistance)))) {
            state = State::Near;
        }
        if (state == State::Away) {
            continue;
        }

        nearbyZones.insert(zone);
        _states[zone] = state;
        if (state <= previous) {
            continue;
        }

        const QString name = zone->name().isEmpty() ? zone->identifier() : zone->name();
        const QString message = (state == State::Inside) ?
            tr("Entered geozone %1 (%2)").arg(name, zone->restrictionText()) :
            tr("Approaching geozone %1 (%2)").arg(name, zone->restrictionText());
        qCDebug(GeoZoneMonitorLog) << message << "distance" << distance;
        emit alert(message);
    }

    // Zones out of range or no longer active can alert again
    for (auto it = _states.begin(); it != _states.end();) {
        it = nearbyZones.contains(it.key()) ? std::next(it) : _states.erase(it);
    }
}

void GeoZoneMonitor::_showAlert(const QString &message)
{
    // A modal dialog would get in the way while flying
    qgcApp()->showCriticalVehicleMessage(message);
    AudioOutput::instance()->say(message);
}
