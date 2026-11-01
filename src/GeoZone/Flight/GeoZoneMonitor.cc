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
#include "GeoZoneAlertHistory.h"
#include "GeoZoneAlertLog.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneGuidedCheck.h"
#include "GeoZoneInfo.h"
#include "GeoZoneManager.h"
#include "GeoZonePlanning.h"
#include "GeoZoneSettings.h"
#include "GeoZoneSource.h"
#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include <QtCore/qapplicationstatic.h>

#include <algorithm>
#include <cmath>

QGC_LOGGING_CATEGORY(GeoZoneMonitorLog, "qgc.geozone.geozonemonitor")

Q_APPLICATION_STATIC(GeoZoneMonitor, _geoZoneMonitor);

namespace {

/// Speaks with the audio output of the application. The alert popup shows the alerts.
class ApplicationAudioSink : public GeoZoneAlertArbiter::AudioSink
{
public:
    void say(const QString &text) override { AudioOutput::instance()->say(text); }
};

} // namespace

GeoZoneMonitor::GeoZoneMonitor(QObject *parent)
    : GeoZoneMonitor(GeoZoneManager::instance(), parent)
{
    _alertLog = new GeoZoneAlertLog(this);
    (void) connect(this, &GeoZoneMonitor::alertEvent, _alertLog, &GeoZoneAlertLog::append);
    // Alerts of earlier sessions today, before those of this session are logged
    _alertHistory->loadLog(_alertLog->filePath(GeoZoneEnvironment::currentDateTimeUtc().date()));
    _arbiter.setAudioSink(std::make_shared<ApplicationAudioSink>());

    // Evaluated periodically, since a hovering or climbing vehicle doesn't report a new coordinate
    _evaluateTimer.setInterval(evaluateIntervalMSecs);
    (void) connect(&_evaluateTimer, &QTimer::timeout, this, &GeoZoneMonitor::_evaluateVehicles);
    (void) connect(this, &GeoZoneMonitor::monitoringChanged, this, &GeoZoneMonitor::_updateEvaluateTimer);

    MultiVehicleManager *const multiVehicleManager = MultiVehicleManager::instance();
    (void) connect(multiVehicleManager, &MultiVehicleManager::vehicleAdded, this, &GeoZoneMonitor::addVehicle);
    (void) connect(multiVehicleManager, &MultiVehicleManager::vehicleRemoved, this, &GeoZoneMonitor::removeVehicle);
    (void) connect(multiVehicleManager, &MultiVehicleManager::activeVehicleChanged, this, &GeoZoneMonitor::setActiveVehicle);
    for (int i = 0; i < multiVehicleManager->vehicles()->count(); i++) {
        (void) addVehicle(multiVehicleManager->vehicles()->value<Vehicle*>(i));
    }
    setActiveVehicle(multiVehicleManager->activeVehicle());
}

GeoZoneMonitor::GeoZoneMonitor(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _nearbyModel(new GeoZoneNearbyModel(this))
    , _guidedCheck(new GeoZoneGuidedCheck(this))
    , _alertHistory(new GeoZoneAlertHistory(_manager, this))
{
    _elapsed.start();
    _arbiter.setClock([this]() { return _now(); });
    _muteTimer.setSingleShot(true);
    (void) connect(&_muteTimer, &QTimer::timeout, this, &GeoZoneMonitor::_updateMuted);

    (void) connect(this, &GeoZoneMonitor::alertEvent, _alertHistory, &GeoZoneAlertHistory::append);
    // Alerts acknowledged in the history are no longer repeated
    (void) connect(_alertHistory, &GeoZoneAlertHistory::alertAcknowledged, this, [this](int alertId) { _arbiter.acknowledge(alertId); });
    (void) connect(_alertHistory, &GeoZoneAlertHistory::allAcknowledged, this, [this]() { _arbiter.acknowledgeAll(); });

    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMonitor::_forgetZones);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMonitor::_zonesChanged);

    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    for (Fact *const fact : { settings->alertsEnabled(), settings->audioAlerts(), settings->approachDistance(),
                              settings->approachVerticalDistance(), settings->approachTime() }) {
        (void) connect(fact, &Fact::rawValueChanged, this, &GeoZoneMonitor::_applicationSettingsChanged);
    }
}

GeoZoneMonitor *GeoZoneMonitor::instance()
{
    return _geoZoneMonitor();
}

GeoZoneMonitor::Settings GeoZoneMonitor::Settings::fromApplication()
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    Settings result;
    result.alertsEnabled = settings->alertsEnabled()->rawValue().toBool();
    result.audioAlerts = settings->audioAlerts()->rawValue().toBool();
    result.limits.distance = settings->approachDistance()->rawValue().toDouble();
    result.limits.verticalDistance = settings->approachVerticalDistance()->rawValue().toDouble();
    result.limits.time = settings->approachTime()->rawValue().toDouble();
    return result;
}

const GeoZoneMonitor::Settings &GeoZoneMonitor::settings() const
{
    if (_fixedSettings) {
        return *_fixedSettings;
    }
    if (!_settings) {
        _settings = Settings::fromApplication();
    }
    return *_settings;
}

void GeoZoneMonitor::setSettings(const Settings &settings)
{
    const Settings previous = this->settings();
    _fixedSettings = settings;
    _settingsChanged(previous);
}

void GeoZoneMonitor::_applicationSettingsChanged()
{
    if (_fixedSettings) {
        return;
    }
    const Settings previous = settings();
    _settings.reset();
    _settingsChanged(previous);
}

void GeoZoneMonitor::_settingsChanged(const Settings &previous)
{
    if (settings().alertsEnabled != previous.alertsEnabled) {
        _resetMonitors();
        emit monitoringChanged();
    }
}

void GeoZoneMonitor::_resetMonitors()
{
    for (GeoZoneVehicleMonitor *const monitor : std::as_const(_monitors)) {
        monitor->reset();
    }
    _arbiter.clear();
}

GeoZoneVehicleMonitor::Context GeoZoneMonitor::_context() const
{
    if (!_policy) {
        // Authorised zones don't alert while the authorisation is valid
        _policy = GeoZonePolicy::alerts().excludingAuthorised(_manager->planning()->authorisations());
    }
    return GeoZoneVehicleMonitor::Context{ &_manager->catalog(), *_policy, settings().limits };
}

GeoZoneVehicleMonitor *GeoZoneMonitor::_createMonitor(Vehicle *vehicle, int vehicleId)
{
    GeoZoneVehicleMonitor *const monitor = new GeoZoneVehicleMonitor(vehicle, vehicleId, _nextVehicleKey++, [this]() { return _now(); }, this);
    _monitors.append(monitor);

    // The state of the active vehicle is published
    (void) connect(monitor, &GeoZoneVehicleMonitor::nearbyChanged, this, [this, monitor]() {
        if (monitor == _activeMonitor()) {
            _publishNearby();
        }
    });
    (void) connect(monitor, &GeoZoneVehicleMonitor::verticalBandsChanged, this, [this, monitor]() {
        if (monitor == _activeMonitor()) {
            emit verticalBandsChanged();
        }
    });
    (void) connect(monitor, &GeoZoneVehicleMonitor::predictionChanged, this, [this, monitor]() {
        if (monitor == _activeMonitor()) {
            emit predictionChanged();
        }
    });
    (void) connect(monitor, &GeoZoneVehicleMonitor::positionUnavailableChanged, this, [this, monitor]() {
        if (monitor == _activeMonitor()) {
            emit positionUnavailableChanged();
        }
    });
    (void) connect(monitor, &GeoZoneVehicleMonitor::fenceStatusChanged, this, [this, monitor]() {
        if (monitor == _activeMonitor()) {
            emit fenceStatusChanged();
        }
    });
    (void) connect(monitor, &GeoZoneVehicleMonitor::fenceBreachStarted, this, [this, monitor](const QString &text) {
        emit fenceBreach(_labelled(monitor->vehicleId(), text));
    });
    _resolveActive();
    return monitor;
}

void GeoZoneMonitor::_removeMonitor(GeoZoneVehicleMonitor *monitor)
{
    if (!monitor || !_monitors.contains(monitor)) {
        return;
    }
    (void) _monitors.removeOne(monitor);
    _arbiter.removeVehicle(monitor->key());
    (void) monitor->disconnect(this);
    // Removed while the vehicle emits a signal, or from a slot of the monitor
    monitor->deleteLater();
    _resolveActive();
    if (monitor->followsVehicle()) {
        emit monitoringChanged();
    }
}

GeoZoneVehicleMonitor *GeoZoneMonitor::addVehicle(Vehicle *vehicle)
{
    if (!vehicle) {
        return nullptr;
    }
    if (GeoZoneVehicleMonitor *const existing = vehicleMonitor(vehicle)) {
        return existing;
    }
    GeoZoneVehicleMonitor *const monitor = _createMonitor(vehicle, vehicle->id());
    // A vehicle deleted without being removed, its address may be reused by the next one
    (void) connect(vehicle, &QObject::destroyed, monitor, [this, monitor]() { _removeMonitor(monitor); });
    emit monitoringChanged();
    return monitor;
}

void GeoZoneMonitor::removeVehicle(Vehicle *vehicle)
{
    _removeMonitor(vehicleMonitor(vehicle));
}

GeoZoneVehicleMonitor *GeoZoneMonitor::vehicleMonitor(const Vehicle *vehicle) const
{
    if (!vehicle) {
        return nullptr;
    }
    const auto it = std::find_if(_monitors.cbegin(), _monitors.cend(), [vehicle](const GeoZoneVehicleMonitor *monitor) {
        return monitor->followsVehicle() && (monitor->vehicle() == vehicle);
    });
    return (it != _monitors.cend()) ? *it : nullptr;
}

GeoZoneVehicleMonitor *GeoZoneMonitor::positionMonitor(int vehicleId) const
{
    const auto it = std::find_if(_monitors.cbegin(), _monitors.cend(), [vehicleId](const GeoZoneVehicleMonitor *monitor) {
        return !monitor->followsVehicle() && (monitor->vehicleId() == vehicleId);
    });
    return (it != _monitors.cend()) ? *it : nullptr;
}

void GeoZoneMonitor::removePositionMonitor(int vehicleId)
{
    _removeMonitor(positionMonitor(vehicleId));
}

void GeoZoneMonitor::setActiveVehicle(Vehicle *vehicle)
{
    if ((vehicle == _activeVehicle) && (vehicle || (_activeVehicleId == noVehicle))) {
        return;
    }
    _activeVehicle = vehicle;
    _activeVehicleId = vehicle ? vehicle->id() : noVehicle;
    _active = vehicleMonitor(vehicle);
    _publishActive();
}

void GeoZoneMonitor::setActiveVehicleId(int vehicleId)
{
    if (!_activeVehicle && (vehicleId == _activeVehicleId)) {
        return;
    }
    _activeVehicle = nullptr;
    _activeVehicleId = vehicleId;
    _active = positionMonitor(vehicleId);
    _publishActive();
}

void GeoZoneMonitor::_resolveActive()
{
    GeoZoneVehicleMonitor *const active = _activeVehicle ? vehicleMonitor(_activeVehicle) : positionMonitor(_activeVehicleId);
    if (active != _active) {
        _active = active;
        _publishActive();
    }
}

void GeoZoneMonitor::_publishNearby()
{
    _nearbyModel->setItems(_activeNearby());
    emit nearbyZonesChanged();
}

void GeoZoneMonitor::_publishActive()
{
    _publishNearby();
    emit verticalBandsChanged();
    emit predictionChanged();
    emit positionUnavailableChanged();
    emit fenceStatusChanged();
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
    const GeoZoneVehicleMonitor::Context context = _context();
    const QList<QPointer<GeoZoneVehicleMonitor>> monitors(_monitors.cbegin(), _monitors.cend());
    for (const QPointer<GeoZoneVehicleMonitor> &monitor : monitors) {
        if (monitor && monitor->followsVehicle() && _monitors.contains(monitor)) {
            _raise(monitor, monitor->update(context));
        }
    }
    _announce();
}

void GeoZoneMonitor::evaluate(Vehicle *vehicle, const GeoZone::Position &position, const Velocity &velocity)
{
    GeoZoneVehicleMonitor *const monitor = vehicleMonitor(vehicle);
    if (!monitor || !position.coordinate.isValid()) {
        return;
    }
    _evaluate(monitor, position, velocity);
    _announce();
}

void GeoZoneMonitor::evaluate(const GeoZone::Position &position, int vehicleId, const Velocity &velocity)
{
    if (!position.coordinate.isValid()) {
        return;
    }
    GeoZoneVehicleMonitor *monitor = positionMonitor(vehicleId);
    if (!monitor) {
        monitor = _createMonitor(nullptr, vehicleId);
    }
    _evaluate(monitor, position, velocity);
    _announce();
}

void GeoZoneMonitor::_evaluate(GeoZoneVehicleMonitor *monitor, const GeoZone::Position &position, const Velocity &velocity)
{
    _raise(monitor, monitor->evaluate(position, velocity, _context()));
}

void GeoZoneMonitor::_raise(GeoZoneVehicleMonitor *monitor, const QList<GeoZoneAlertTracker::Alert> &alerts)
{
    for (const GeoZoneAlertTracker::Alert &zoneAlert : alerts) {
        const AlertEvent event = _alertEvent(zoneAlert, monitor->position(), monitor->vehicleId());
        const int severity = GeoZone::restrictionSeverity(event.restriction);
        emit alertEvent(event);
        emit alert(event.message, severity, event.id);

        GeoZoneAlertArbiter::Alert spoken;
        spoken.id = event.id;
        spoken.vehicleKey = monitor->key();
        spoken.zoneKey = event.zoneKey;
        spoken.severity = severity;
        spoken.inside = event.inside;
        spoken.breach = event.inside && (event.restriction == GeoZone::Prohibited);
        spoken.message = event.message;
        spoken.repeatMessage = _labelled(monitor->vehicleId(), tr("Still inside geozone %1 (%2)").arg(zoneAlert.zone->displayName(), zoneAlert.zone->restrictionText()));
        _arbiter.submit(spoken);
    }
    // Breaches end when the vehicle leaves the zone
    _arbiter.setInsideZones(monitor->key(), monitor->insideZoneKeys());
}

void GeoZoneMonitor::_announce()
{
    _arbiter.setEnabled(settings().audioAlerts);
    const GeoZoneAlertArbiter::Announcement announcement = _arbiter.tick();
    if (!announcement.text.isEmpty()) {
        qCDebug(GeoZoneMonitorLog) << "Spoken:" << announcement.text;
    }
    for (const GeoZoneAlertArbiter::Alert &repeated : announcement.repeated) {
        emit alertRepeated(repeated.repeatMessage, repeated.severity, repeated.id);
    }
    _updateMuted();
}

void GeoZoneMonitor::acknowledgeAlert(int alertId)
{
    _alertHistory->acknowledgeAlert(alertId);
    _arbiter.acknowledge(alertId);
}

void GeoZoneMonitor::muteAlerts(int seconds)
{
    const qint64 msecs = (seconds > 0) ? (qint64{ seconds } * 1000) : qint64{ GeoZoneAlertArbiter::defaultMuteMSecs };
    _arbiter.mute(msecs);
    // Notified after the arbiter's clock passed the end
    _muteTimer.start(static_cast<int>(msecs) + 100);
    _updateMuted();
}

void GeoZoneMonitor::unmuteAlerts()
{
    _arbiter.unmute();
    _muteTimer.stop();
    _updateMuted();
}

void GeoZoneMonitor::_updateMuted()
{
    const bool muted = _arbiter.muted();
    if (muted != _alertsMuted) {
        _alertsMuted = muted;
        emit alertsMutedChanged();
    }
}

bool GeoZoneMonitor::monitoring() const
{
    const bool vehicles = std::any_of(_monitors.cbegin(), _monitors.cend(), [](const GeoZoneVehicleMonitor *monitor) { return monitor->followsVehicle(); });
    return vehicles && (_manager->zoneCount() > 0) && settings().alertsEnabled;
}

std::optional<GeoZoneAlertTracker::Entry> GeoZoneMonitor::_activeEntry() const
{
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor();
    return monitor ? monitor->entry() : std::nullopt;
}

QVariantList GeoZoneMonitor::predictedPath() const
{
    const std::optional<GeoZoneAlertTracker::Entry> entry = _activeEntry();
    if (!entry) {
        return {};
    }
    return { QVariant::fromValue(_activeMonitor()->entryFrom()), QVariant::fromValue(entry->position.coordinate) };
}

QGeoCoordinate GeoZoneMonitor::predictedEntry() const
{
    const std::optional<GeoZoneAlertTracker::Entry> entry = _activeEntry();
    return entry ? entry->position.coordinate : QGeoCoordinate();
}

int GeoZoneMonitor::predictedEntryTime() const
{
    const std::optional<GeoZoneAlertTracker::Entry> entry = _activeEntry();
    return entry ? static_cast<int>(std::lround(entry->time)) : -1;
}

QVariant GeoZoneMonitor::predictedEntryZone() const
{
    const std::optional<GeoZoneAlertTracker::Entry> entry = _activeEntry();
    return entry ? QVariant::fromValue(GeoZoneInfo(entry->zone)) : QVariant();
}

const QList<GeoZoneMonitor::Nearby> &GeoZoneMonitor::_activeNearby() const
{
    static const QList<Nearby> none;
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor();
    return monitor ? monitor->nearby() : none;
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

QVariantList GeoZoneMonitor::verticalBands() const
{
    QVariantList bands;
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor();
    if (!monitor) {
        return bands;
    }
    for (const GeoZoneVerticalBand &band : monitor->verticalBands()) {
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
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor();
    return monitor ? monitor->bandsPosition().altitudeAMSL : qQNaN();
}

double GeoZoneMonitor::terrainAMSL() const
{
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor();
    return monitor ? monitor->bandsPosition().terrainAMSL : qQNaN();
}

bool GeoZoneMonitor::positionUnavailable() const
{
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor();
    return monitor && monitor->positionUnavailable();
}

bool GeoZoneMonitor::fenceBreached() const
{
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor();
    return monitor && monitor->fenceBreached();
}

bool GeoZoneMonitor::fenceStatusKnown() const
{
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor();
    return monitor && monitor->fenceStatusKnown();
}

QString GeoZoneMonitor::fenceBreachText() const
{
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor();
    return monitor ? monitor->fenceBreachText() : QString();
}

void GeoZoneMonitor::_forgetZones(GeoZoneSource *source)
{
    // Zones of the source are about to be deleted. Their alert states are kept so reloading the source doesn't repeat alerts.
    _policy.reset();
    for (GeoZoneVehicleMonitor *const monitor : std::as_const(_monitors)) {
        monitor->forgetZones(source->key());
    }
}

void GeoZoneMonitor::_zonesChanged(GeoZoneSource *source)
{
    _policy.reset();
    // Zones whose data changed alert again, their limits or schedule may differ
    if (source && !source->changedZoneKeys().isEmpty()) {
        for (GeoZoneVehicleMonitor *const monitor : std::as_const(_monitors)) {
            monitor->forgetStates(source->changedZoneKeys());
        }
    }
    emit monitoringChanged();
}

QString GeoZoneMonitor::_labelled(int vehicleId, const QString &message) const
{
    if (vehicleId == noVehicle) {
        return message;
    }
    const qsizetype vehicleCount = std::count_if(_monitors.cbegin(), _monitors.cend(), [](const GeoZoneVehicleMonitor *monitor) {
        return monitor->followsVehicle() || (monitor->vehicleId() != noVehicle);
    });
    return (vehicleCount > 1) ? QStringLiteral("%1: %2").arg(tr("Vehicle %1").arg(vehicleId), message) : message;
}

GeoZoneMonitor::AlertEvent GeoZoneMonitor::_alertEvent(const GeoZoneAlertTracker::Alert &alert, const GeoZone::Position &position, int vehicleId)
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
    message = _labelled(vehicleId, message);
    qCDebug(GeoZoneMonitorLog) << message << "distance" << alert.distance << "time to entry" << alert.timeToEntry;

    AlertEvent event;
    event.id = _nextAlertId++;
    event.time = GeoZoneEnvironment::currentDateTimeUtc();
    event.vehicleId = vehicleId;
    event.position = position;
    event.inside = alert.inside;
    event.distance = alert.distance;
    event.zoneIdentifier = zone->identifier();
    event.zoneName = zone->name();
    event.restriction = zone->restriction();
    event.sourceText = zone->sourceText();
    event.message = message;
    event.zoneKey = zone->key();
    event.timeToEntry = alert.timeToEntry;
    return event;
}
