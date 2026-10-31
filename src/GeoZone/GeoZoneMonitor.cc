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
#include "QmlObjectListModel.h"
#include "QGCLoggingCategory.h"
#include "Vehicle.h"

#include <QtCore/qapplicationstatic.h>
#include <QtCore/QSet>

#include <cmath>
#include <QtPositioning/QGeoCircle>

QGC_LOGGING_CATEGORY(GeoZoneMonitorLog, "qgc.geozone.geozonemonitor")

Q_APPLICATION_STATIC(GeoZoneMonitor, _geoZoneMonitor);

GeoZoneMonitor::GeoZoneMonitor(QObject *parent)
    : GeoZoneMonitor(GeoZoneManager::instance(), parent)
{
    // Evaluated periodically, since a hovering or climbing vehicle doesn't report a new coordinate
    _evaluateTimer.setInterval(evaluateIntervalMSecs);
    (void) connect(&_evaluateTimer, &QTimer::timeout, this, &GeoZoneMonitor::_evaluateVehicle);

    (void) connect(this, &GeoZoneMonitor::alert, this, &GeoZoneMonitor::_showAlert);
    (void) connect(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged, this, &GeoZoneMonitor::_activeVehicleChanged);
    (void) connect(this, &GeoZoneMonitor::monitoringChanged, this, &GeoZoneMonitor::_updateEvaluateTimer);
    _activeVehicleChanged(MultiVehicleManager::instance()->activeVehicle());
}

void GeoZoneMonitor::_updateEvaluateTimer()
{
    if (monitoring()) {
        if (!_evaluateTimer.isActive()) {
            _evaluateTimer.start();
            _evaluateVehicle();
        }
    } else {
        _evaluateTimer.stop();
    }
}

void GeoZoneMonitor::_evaluateVehicle()
{
    if (_vehicle && _vehicle->coordinate().isValid()) {
        evaluate(vehiclePosition(_vehicle, _vehicle->coordinate()));
    }
}

GeoZoneMonitor::GeoZoneMonitor(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager)
{
    // Zones are about to be deleted, so forget them
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMonitor::_reset);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMonitor::monitoringChanged);
}

bool GeoZoneMonitor::monitoring() const
{
    return _vehicle && (_manager->zones()->count() > 0);
}

QVariantList GeoZoneMonitor::nearbyZones() const
{
    QVariantList zones;
    for (const Nearby &nearby : _nearby) {
        zones.append(QVariantMap{
            { QStringLiteral("zone"), QVariant::fromValue(static_cast<QObject*>(nearby.zone)) },
            { QStringLiteral("inside"), nearby.inside },
            { QStringLiteral("distance"), nearby.distance },
        });
    }
    return zones;
}

int GeoZoneMonitor::alertRestriction() const
{
    return _nearby.isEmpty() ? -1 : static_cast<int>(_nearby.first().zone->restriction());
}

bool GeoZoneMonitor::inside() const
{
    return !_nearby.isEmpty() && _nearby.first().inside;
}

void GeoZoneMonitor::_setNearby(const QList<Nearby> &nearby)
{
    if (nearby != _nearby) {
        _nearby = nearby;
        emit nearbyZonesChanged();
    }
}

GeoZoneMonitor *GeoZoneMonitor::instance()
{
    return _geoZoneMonitor();
}

void GeoZoneMonitor::_activeVehicleChanged(Vehicle *vehicle)
{
    _reset();
    _vehicle = vehicle;
    emit monitoringChanged();
}

GeoZone::Position GeoZoneMonitor::vehiclePosition(Vehicle *vehicle, const QGeoCoordinate &coordinate)
{
    GeoZone::Position position{ coordinate };
    if (!vehicle) {
        return position;
    }

    const double altitudeAMSL = vehicle->altitudeAMSL()->rawValue().toDouble();
    if (qIsNaN(altitudeAMSL)) {
        return position;
    }
    position.altitudeAMSL = altitudeAMSL;

    // Without terrain data limits above ground can't be excluded. The height above home is not used instead
    // since it misses zones over rising ground.
    const double heightAboveTerrain = vehicle->altitudeAboveTerr()->rawValue().toDouble();
    if (!qIsNaN(heightAboveTerrain)) {
        position.terrainAMSL = altitudeAMSL - heightAboveTerrain;
    }
    return position;
}

void GeoZoneMonitor::_reset()
{
    _states.clear();
    _setNearby({});
}

void GeoZoneMonitor::evaluate(const GeoZone::Position &position)
{
    if (!position.coordinate.isValid()) {
        return;
    }

    static const QList<GeoZone::Restriction> restrictions = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional };
    const QGeoRectangle area = QGeoCircle(position.coordinate, approachDistance + hysteresisDistance).boundingGeoRectangle();

    QSet<const GeoZone*> nearbyZones;
    QList<Nearby> nearby;
    for (GeoZone *zone : _manager->zonesInArea(restrictions, area)) {
        if (!zone->active()) {
            continue;
        }

        const State previous = _states.value(zone, State::Away);
        const double insideDistance = zone->distanceTo(position, (previous == State::Inside) ? hysteresisDistance : 0);
        const double nearDistance = zone->distanceTo(position, approachVerticalDistance + ((previous != State::Away) ? hysteresisDistance : 0));
        const double distance = std::min(insideDistance, nearDistance);
        State state = State::Away;
        if ((insideDistance <= 0) || ((previous == State::Inside) && (insideDistance <= hysteresisDistance))) {
            state = State::Inside;
        } else if ((nearDistance <= approachDistance) || ((previous != State::Away) && (nearDistance <= (approachDistance + hysteresisDistance)))) {
            state = State::Near;
        }
        if (state == State::Away) {
            continue;
        }

        nearbyZones.insert(zone);
        _states[zone] = state;
        nearby.append(Nearby{ zone, state == State::Inside, static_cast<int>(std::lround(distance)) });
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

    // Zones the vehicle is in first, then the most restrictive, then the closest
    std::stable_sort(nearby.begin(), nearby.end(), [](const Nearby &a, const Nearby &b) {
        if (a.inside != b.inside) {
            return a.inside;
        }
        const int severityA = GeoZone::restrictionSeverity(a.zone->restriction());
        const int severityB = GeoZone::restrictionSeverity(b.zone->restriction());
        return (severityA != severityB) ? (severityA < severityB) : (a.distance < b.distance);
    });
    _setNearby(nearby);
}

void GeoZoneMonitor::_showAlert(const QString &message)
{
    // MainWindow shows the message without blocking the user
    AudioOutput::instance()->say(message);
}
