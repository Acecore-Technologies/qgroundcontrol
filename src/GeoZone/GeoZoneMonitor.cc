/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMonitor.h"
#include "GeoZoneInfo.h"
#include "AudioOutput.h"
#include "GeoZone.h"
#include "GeoZoneAlertLog.h"
#include "GeoZoneGuidedCheck.h"
#include "GeoZoneManager.h"
#include "GeoZoneSource.h"
#include "GeoZoneSettings.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "QGCLoggingCategory.h"
#include "SettingsManager.h"
#include "Vehicle.h"

#include <QtCore/qapplicationstatic.h>

#include <algorithm>
#include <cmath>

QGC_LOGGING_CATEGORY(GeoZoneMonitorLog, "qgc.geozone.geozonemonitor")

Q_APPLICATION_STATIC(GeoZoneMonitor, _geoZoneMonitor);

GeoZoneMonitor::GeoZoneMonitor(QObject *parent)
    : GeoZoneMonitor(GeoZoneManager::instance(), parent)
{
    _alertLog = new GeoZoneAlertLog(this);
    (void) connect(this, &GeoZoneMonitor::alertEvent, _alertLog, &GeoZoneAlertLog::append);

    // Evaluated periodically, since a hovering or climbing vehicle doesn't report a new coordinate
    _evaluateTimer.setInterval(evaluateIntervalMSecs);
    (void) connect(&_evaluateTimer, &QTimer::timeout, this, &GeoZoneMonitor::_evaluateVehicles);

    (void) connect(this, &GeoZoneMonitor::alert, this, &GeoZoneMonitor::_speak);
    (void) connect(this, &GeoZoneMonitor::monitoringChanged, this, &GeoZoneMonitor::_updateEvaluateTimer);
    (void) connect(SettingsManager::instance()->geoZoneSettings()->alertsEnabled(), &Fact::rawValueChanged, this, &GeoZoneMonitor::_alertsEnabledChanged);

    MultiVehicleManager *const multiVehicleManager = MultiVehicleManager::instance();
    (void) connect(multiVehicleManager, &MultiVehicleManager::vehicleAdded, this, &GeoZoneMonitor::_vehicleAdded);
    (void) connect(multiVehicleManager, &MultiVehicleManager::vehicleRemoved, this, &GeoZoneMonitor::_vehicleRemoved);
    (void) connect(multiVehicleManager, &MultiVehicleManager::activeVehicleChanged, this, &GeoZoneMonitor::_activeVehicleChanged);
    for (int i = 0; i < multiVehicleManager->vehicles()->count(); i++) {
        _vehicleAdded(multiVehicleManager->vehicles()->value<Vehicle*>(i));
    }
    _activeVehicleChanged(multiVehicleManager->activeVehicle());
}

GeoZoneMonitor::GeoZoneMonitor(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _nearbyModel(new GeoZoneNearbyModel(this))
    , _guidedCheck(new GeoZoneGuidedCheck(this))
{
    _clock.start();
    // Zones of a source are about to be deleted. Their alert states are kept so reloading the source doesn't repeat alerts.
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMonitor::_forgetZones);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMonitor::monitoringChanged);
}

GeoZoneMonitor *GeoZoneMonitor::instance()
{
    return _geoZoneMonitor();
}

void GeoZoneMonitor::_vehicleAdded(Vehicle *vehicle)
{
    if (!vehicle || _trackers.contains(vehicle->id())) {
        return;
    }
    _vehicles.append(vehicle);
    _trackers.insert(vehicle->id(), Tracker());
    emit monitoringChanged();
}

void GeoZoneMonitor::_vehicleRemoved(Vehicle *vehicle)
{
    _vehicles.removeIf([vehicle](const QPointer<Vehicle> &monitored) { return !monitored || (monitored == vehicle); });
    if (vehicle) {
        removeVehicle(vehicle->id());
    }
    emit monitoringChanged();
}

void GeoZoneMonitor::_activeVehicleChanged(Vehicle *vehicle)
{
    setActiveVehicleId(vehicle ? vehicle->id() : noVehicle);
}

void GeoZoneMonitor::setActiveVehicleId(int vehicleId)
{
    if (vehicleId != _activeVehicleId) {
        _activeVehicleId = vehicleId;
        _publishNearby();
    }
}

void GeoZoneMonitor::removeVehicle(int vehicleId)
{
    const auto it = _trackers.find(vehicleId);
    if (it == _trackers.end()) {
        return;
    }
    const bool hadNearby = !it->nearby.isEmpty();
    _trackers.erase(it);
    if (hadNearby && (vehicleId == _activeVehicleId)) {
        _publishNearby();
    }
}

void GeoZoneMonitor::_updateEvaluateTimer()
{
    if (monitoring()) {
        if (!_evaluateTimer.isActive()) {
            _evaluateTimer.start();
            _evaluateVehicles();
        }
    } else {
        _evaluateTimer.stop();
    }
}

void GeoZoneMonitor::_evaluateVehicles()
{
    const qint64 msecs = _clock.elapsed();
    for (const QPointer<Vehicle> &vehicle : std::as_const(_vehicles)) {
        if (vehicle && vehicle->coordinate().isValid()) {
            const GeoZone::Position position = vehiclePosition(vehicle, vehicle->coordinate());
            const Velocity velocity = _trackers[vehicle->id()].velocity.add(msecs, position);
            evaluate(position, vehicle->id(), velocity);
        }
    }
}

void GeoZoneMonitor::_alertsEnabledChanged()
{
    _resetTrackers();
    emit monitoringChanged();
}

bool GeoZoneMonitor::monitoring() const
{
    return !_vehicles.isEmpty() && (_manager->zoneCount() > 0) && SettingsManager::instance()->geoZoneSettings()->alertsEnabled()->rawValue().toBool();
}

void GeoZoneMonitor::_publishNearby()
{
    _nearbyModel->setItems(_activeNearby());
    emit nearbyZonesChanged();
    emit verticalBandsChanged();
}

const QList<GeoZoneMonitor::Nearby> &GeoZoneMonitor::_activeNearby() const
{
    static const QList<Nearby> none;
    const auto it = _trackers.constFind(_activeVehicleId);
    return (it != _trackers.constEnd()) ? it->nearby : none;
}

QVariantList GeoZoneMonitor::nearbyZones() const
{
    QVariantList zones;
    for (const Nearby &nearby : _activeNearby()) {
        zones.append(QVariantMap{
            { QStringLiteral("zone"), QVariant::fromValue(GeoZoneInfo(nearby.zone)) },
            { QStringLiteral("inside"), nearby.inside },
            { QStringLiteral("distance"), nearby.distance },
            { QStringLiteral("timeToEntry"), nearby.timeToEntry },
        });
    }
    return zones;
}

int GeoZoneMonitor::alertRestriction() const
{
    const QList<Nearby> &nearby = _activeNearby();
    return nearby.isEmpty() ? -1 : static_cast<int>(nearby.first().zone->restriction());
}

bool GeoZoneMonitor::inside() const
{
    const QList<Nearby> &nearby = _activeNearby();
    return std::any_of(nearby.cbegin(), nearby.cend(), [](const Nearby &zone) { return zone.inside; });
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
    // since it misses zones over rising ground. A disarmed vehicle is on the ground.
    const double heightAboveTerrain = vehicle->altitudeAboveTerr()->rawValue().toDouble();
    if (!qIsNaN(heightAboveTerrain)) {
        position.terrainAMSL = altitudeAMSL - heightAboveTerrain;
    } else if (!vehicle->armed()) {
        position.terrainAMSL = altitudeAMSL;
    }
    return position;
}

const GeoZoneMonitor::Tracker *GeoZoneMonitor::_activeTracker() const
{
    const auto it = _trackers.constFind(_activeVehicleId);
    return (it != _trackers.constEnd()) ? &it.value() : nullptr;
}

QVariantList GeoZoneMonitor::verticalBands() const
{
    QVariantList bands;
    const Tracker *const tracker = _activeTracker();
    if (!tracker) {
        return bands;
    }
    for (const GeoZoneVerticalBand &band : tracker->bands) {
        bands.append(QVariantMap{
            { QStringLiteral("restriction"), static_cast<int>(band.zone->restriction()) },
            { QStringLiteral("name"), band.zone->displayName() },
            { QStringLiteral("limitsText"), band.zone->verticalLimitsText() },
            { QStringLiteral("floor"), band.floor },
            { QStringLiteral("ceiling"), band.ceiling },
        });
    }
    return bands;
}

double GeoZoneMonitor::altitudeAMSL() const
{
    const Tracker *const tracker = _activeTracker();
    return tracker ? tracker->position.altitudeAMSL : qQNaN();
}

double GeoZoneMonitor::terrainAMSL() const
{
    const Tracker *const tracker = _activeTracker();
    return tracker ? tracker->position.terrainAMSL : qQNaN();
}

void GeoZoneMonitor::_resetTrackers()
{
    const bool hadNearby = !_activeNearby().isEmpty();
    for (Tracker &tracker : _trackers) {
        tracker = Tracker();
    }
    if (hadNearby) {
        _publishNearby();
    }
}

void GeoZoneMonitor::_forgetZones(GeoZoneSource *source)
{
    bool activeChanged = false;
    for (auto it = _trackers.begin(); it != _trackers.end(); ++it) {
        const qsizetype removedBands = it->bands.removeIf([source](const GeoZoneVerticalBand &band) { return band.zone->sourceKey() == source->key(); });
        if ((it->nearby.removeIf([source](const Nearby &nearby) { return nearby.zone->sourceKey() == source->key(); }) + removedBands) > 0) {
            activeChanged = activeChanged || (it.key() == _activeVehicleId);
        }
    }
    if (activeChanged) {
        _publishNearby();
    }
}

QString GeoZoneMonitor::_vehicleLabel(int vehicleId) const
{
    if (vehicleId == noVehicle) {
        return QString();
    }
    const qsizetype vehicleCount = std::count_if(_trackers.keyBegin(), _trackers.keyEnd(), [](int id) { return id != noVehicle; });
    return (vehicleCount > 1) ? tr("Vehicle %1").arg(vehicleId) : QString();
}

GeoZoneMonitor::AlertEvent GeoZoneMonitor::_alertEvent(const GeoZoneAlertTracker::Alert &alert, const GeoZone::Position &position, int vehicleId) const
{
    const GeoZone *const zone = alert.zone;
    const QString name = zone->displayName();
    QString message;
    if (alert.inside) {
        message = tr("Entered geozone %1 (%2)").arg(name, zone->restrictionText());
    } else if (!std::isnan(alert.timeToEntry)) {
        message = tr("Approaching geozone %1 (%2), entry in about %3 s").arg(name, zone->restrictionText()).arg(std::lround(alert.timeToEntry));
    } else {
        message = tr("Approaching geozone %1 (%2)").arg(name, zone->restrictionText());
    }
    const QString vehicleLabel = _vehicleLabel(vehicleId);
    if (!vehicleLabel.isEmpty()) {
        message = QStringLiteral("%1: %2").arg(vehicleLabel, message);
    }
    qCDebug(GeoZoneMonitorLog) << message << "distance" << alert.distance << "time to entry" << alert.timeToEntry;

    AlertEvent event;
    event.time = QDateTime::currentDateTimeUtc();
    event.vehicleId = vehicleId;
    event.position = position;
    event.inside = alert.inside;
    event.distance = alert.distance;
    event.zoneIdentifier = zone->identifier();
    event.zoneName = zone->name();
    event.restriction = zone->restriction();
    event.sourceText = zone->sourceText();
    event.message = message;
    event.timeToEntry = alert.timeToEntry;
    return event;
}

void GeoZoneMonitor::evaluate(const GeoZone::Position &position, int vehicleId, const Velocity &velocity)
{
    if (!position.coordinate.isValid()) {
        return;
    }

    GeoZoneSettings *const settings = SettingsManager::instance()->geoZoneSettings();
    GeoZoneAlertTracker::Limits limits;
    limits.distance = settings->approachDistance()->rawValue().toDouble();
    limits.verticalDistance = settings->approachVerticalDistance()->rawValue().toDouble();
    limits.time = settings->approachTime()->rawValue().toDouble();

    Tracker &tracker = _trackers[vehicleId];
    const GeoZoneAlertTracker::Update update = tracker.alerts.update(_manager->catalog(), position, velocity, limits);
    if (update.nearby != tracker.nearby) {
        tracker.nearby = update.nearby;
        if (vehicleId == _activeVehicleId) {
            _publishNearby();
        }
    }

    // Zones above and below are only shown for the active vehicle
    if (vehicleId == _activeVehicleId) {
        const QList<GeoZoneVerticalBand> bands = GeoZoneVerticalBand::at(_manager->catalog(), position);
        const bool altitudeChanged = (std::isnan(position.altitudeAMSL) != std::isnan(tracker.position.altitudeAMSL)) ||
                                     (std::abs(position.altitudeAMSL - tracker.position.altitudeAMSL) >= 0.5) ||
                                     (std::isnan(position.terrainAMSL) != std::isnan(tracker.position.terrainAMSL)) ||
                                     (std::abs(position.terrainAMSL - tracker.position.terrainAMSL) >= 0.5);
        if ((bands != tracker.bands) || altitudeChanged) {
            tracker.bands = bands;
            tracker.position = position;
            emit verticalBandsChanged();
        }
    }

    for (const GeoZoneAlertTracker::Alert &zoneAlert : update.alerts) {
        const AlertEvent event = _alertEvent(zoneAlert, position, vehicleId);
        emit alertEvent(event);
        emit alert(event.message, GeoZone::restrictionSeverity(event.restriction));
    }
}

void GeoZoneMonitor::_speak(const QString &message)
{
    // MainWindow shows the message without blocking the user
    if (SettingsManager::instance()->geoZoneSettings()->audioAlerts()->rawValue().toBool()) {
        AudioOutput::instance()->say(message);
    }
}
