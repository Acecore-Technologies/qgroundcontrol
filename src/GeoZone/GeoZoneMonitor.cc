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
#include "GeoZoneSettings.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "QGCLoggingCategory.h"
#include "SettingsManager.h"
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
    (void) connect(SettingsManager::instance()->geoZoneSettings()->alertsEnabled(), &Fact::rawValueChanged, this, &GeoZoneMonitor::_alertsEnabledChanged);
    _activeVehicleChanged(MultiVehicleManager::instance()->activeVehicle());
}

void GeoZoneMonitor::_alertsEnabledChanged()
{
    _reset();
    emit monitoringChanged();
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
    // Zones are about to be deleted. Their alert states are kept so reloading a source doesn't repeat alerts.
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMonitor::_forgetZones);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMonitor::monitoringChanged);
}

bool GeoZoneMonitor::monitoring() const
{
    return _vehicle && (_manager->zones()->count() > 0) && SettingsManager::instance()->geoZoneSettings()->alertsEnabled()->rawValue().toBool();
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
    return std::any_of(_nearby.cbegin(), _nearby.cend(), [](const Nearby &nearby) { return nearby.inside; });
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

void GeoZoneMonitor::_forgetZones()
{
    _setNearby({});
}

QString GeoZoneMonitor::_zoneKey(const GeoZone *zone)
{
    return QStringLiteral("%1\x1f%2\x1f%3\x1f%4").arg(zone->identifier(), zone->name(), zone->sourceText().section(QStringLiteral(" ("), 0, 0)).arg(static_cast<int>(zone->restriction()));
}

void GeoZoneMonitor::evaluate(const GeoZone::Position &position)
{
    if (!position.coordinate.isValid()) {
        return;
    }

    // U-space airspace requires a U-space flight authorisation, no restriction zones are only shown on the map
    static const QList<GeoZone::Restriction> restrictions = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::USpace };
    GeoZoneSettings *const settings = SettingsManager::instance()->geoZoneSettings();
    const double approachDistance = settings->approachDistance()->rawValue().toDouble();
    const double approachVerticalDistance = settings->approachVerticalDistance()->rawValue().toDouble();
    const QGeoRectangle area = QGeoCircle(position.coordinate, approachDistance + hysteresisDistance).boundingGeoRectangle();

    QSet<QString> nearbyZones;
    QList<Nearby> nearby;
    for (GeoZone *zone : _manager->zonesInArea(restrictions, area)) {
        if (!zone->active()) {
            continue;
        }

        const QString key = _zoneKey(zone);
        const State previous = _states.value(key, State::Away);
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

        nearbyZones.insert(key);
        _states[key] = state;
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

    // The most restrictive first, so being inside large U-space airspace doesn't hide a prohibited zone ahead,
    // then zones the vehicle is in, then the closest
    std::stable_sort(nearby.begin(), nearby.end(), [](const Nearby &a, const Nearby &b) {
        const int severityA = GeoZone::restrictionSeverity(a.zone->restriction());
        const int severityB = GeoZone::restrictionSeverity(b.zone->restriction());
        if (severityA != severityB) {
            return severityA < severityB;
        }
        if (a.inside != b.inside) {
            return a.inside;
        }
        return a.distance < b.distance;
    });
    _setNearby(nearby);
}

void GeoZoneMonitor::_showAlert(const QString &message)
{
    // MainWindow shows the message without blocking the user
    if (SettingsManager::instance()->geoZoneSettings()->audioAlerts()->rawValue().toBool()) {
        AudioOutput::instance()->say(message);
    }
}
