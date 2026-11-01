/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMonitor.h"
#include "ADSBVehicle.h"
#include "ADSBVehicleManager.h"
#include "AudioOutput.h"
#include "GeoZone.h"
#include "GeoZoneActiveVehicleView.h"
#include "GeoZoneAlertHistory.h"
#include "GeoZoneAlertLog.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneGuidedCheck.h"
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
class ApplicationAudioSink : public GeoZoneMonitor::AudioSink
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
    _audioSink = std::make_shared<ApplicationAudioSink>();

    // Evaluated periodically, since a hovering or climbing vehicle doesn't report a new coordinate
    _evaluateTimer.setInterval(evaluateIntervalMSecs);
    (void) connect(&_evaluateTimer, &QTimer::timeout, this, &GeoZoneMonitor::evaluateVehicles);
    (void) connect(this, &GeoZoneMonitor::monitoringChanged, this, &GeoZoneMonitor::_updateEvaluateTimer);

    // The traffic is checked less often than the vehicles, it changes slower relative to the zones
    _adsbTraffic = true;
    _trafficTimer.setInterval(GeoZoneTrafficMonitor::evaluateIntervalMSecs);
    (void) connect(&_trafficTimer, &QTimer::timeout, this, &GeoZoneMonitor::_evaluateADSBTraffic);
    (void) connect(_traffic, &GeoZoneTrafficMonitor::enabledChanged, this, &GeoZoneMonitor::_updateTrafficTimer);
    (void) connect(this, &GeoZoneMonitor::monitoringChanged, this, &GeoZoneMonitor::_updateTrafficTimer);
    _updateTrafficTimer();

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
    , _activeView(new GeoZoneActiveVehicleView(this))
    , _traffic(new GeoZoneTrafficMonitor(this))
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

    // Zones are muted for the active vehicle
    (void) connect(_activeView, &GeoZoneActiveVehicleView::activeMonitorChanged, this, &GeoZoneMonitor::zoneMutesChanged);

    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMonitor::_forgetZones);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMonitor::_zonesChanged);

    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    for (Fact *const fact : { settings->alertsEnabled(), settings->audioAlerts(), settings->approachDistance(),
                              settings->approachVerticalDistance(), settings->approachTime(), settings->trafficEnabled(),
                              settings->trafficRadius(), settings->trafficAlerts() }) {
        (void) connect(fact, &Fact::rawValueChanged, this, &GeoZoneMonitor::_applicationSettingsChanged);
    }
    _traffic->setEnabled(this->settings().traffic.enabled);
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
    result.traffic.enabled = settings->trafficEnabled()->rawValue().toBool();
    result.traffic.radius = settings->trafficRadius()->rawValue().toDouble();
    result.traffic.alerts = static_cast<GeoZoneTrafficMonitor::AlertMode>(settings->trafficAlerts()->rawValue().toInt());
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
    _traffic->setEnabled(settings().traffic.enabled);
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

    (void) connect(monitor, &GeoZoneVehicleMonitor::fenceBreachStarted, this, [this, monitor](const QString &text) {
        emit fenceBreach(_labelled(monitor->vehicleId(), text));
    });
    (void) connect(monitor, &GeoZoneVehicleMonitor::positionUnavailableChanged, this, [this, monitor]() {
        _positionAvailabilityChanged(monitor);
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
    const int muteCount = _arbiter.zoneMuteCount();
    _arbiter.removeVehicle(monitor->key());
    _zoneMutesChanged(muteCount);
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
    _activeView->setMonitor(vehicleMonitor(vehicle));
}

void GeoZoneMonitor::setActiveVehicleId(int vehicleId)
{
    if (!_activeVehicle && (vehicleId == _activeVehicleId)) {
        return;
    }
    _activeVehicle = nullptr;
    _activeVehicleId = vehicleId;
    _activeView->setMonitor(positionMonitor(vehicleId));
}

void GeoZoneMonitor::_resolveActive()
{
    _activeView->setMonitor(_activeVehicle ? vehicleMonitor(_activeVehicle) : positionMonitor(_activeVehicleId));
}

void GeoZoneMonitor::_updateTrafficTimer()
{
    if (_adsbTraffic && _traffic->enabled() && (_manager->zoneCount() > 0)) {
        if (!_trafficTimer.isActive()) {
            _trafficTimer.start();
        }
    } else {
        _trafficTimer.stop();
        _traffic->clear();
    }
}

void GeoZoneMonitor::_evaluateADSBTraffic()
{
    const QmlObjectListModel *const adsbVehicles = ADSBVehicleManager::instance()->adsbVehicles();
    QList<GeoZoneTrafficMonitor::Aircraft> aircraft;
    aircraft.reserve(adsbVehicles->count());
    for (int i = 0; i < adsbVehicles->count(); i++) {
        const ADSBVehicle *const adsbVehicle = adsbVehicles->value<const ADSBVehicle*>(i);
        if (!adsbVehicle) {
            continue;
        }
        GeoZoneTrafficMonitor::Aircraft plane;
        plane.address = adsbVehicle->icaoAddress();
        plane.callsign = adsbVehicle->callsign();
        plane.coordinate = adsbVehicle->coordinate();
        plane.heading = adsbVehicle->heading();
        plane.speed = adsbVehicle->velocity();
        plane.verticalSpeed = adsbVehicle->verticalVel();
        aircraft.append(plane);
    }
    evaluateTraffic(aircraft);
}

void GeoZoneMonitor::evaluateTraffic(const QList<GeoZoneTrafficMonitor::Aircraft> &aircraft)
{
    // The traffic around the active vehicle, where it is now
    const GeoZoneVehicleMonitor *const active = _activeView->monitor();
    if (!active || active->positionUnavailable() || !active->position().coordinate.isValid()) {
        _traffic->clear();
        return;
    }
    const QList<GeoZoneTrafficMonitor::Entry> entries = _traffic->evaluate(aircraft, active->position(), settings().traffic.radius, _context());
    if (entries.isEmpty() || !settings().alertsEnabled || (settings().traffic.alerts == GeoZoneTrafficMonitor::NoAlerts)) {
        return;
    }
    for (const GeoZoneTrafficMonitor::Entry &entry : entries) {
        _raiseTraffic(entry);
    }
    _announce();
}

void GeoZoneMonitor::_raiseTraffic(const GeoZoneTrafficMonitor::Entry &entry)
{
    const AlertEvent event = _alertEvent(entry.zone, AlertEvent::Traffic, true, 0, qQNaN(), entry.position, noVehicle, entry.aircraft.label());
    const int severity = trafficSeverity + GeoZone::restrictionSeverity(event.restriction);
    emit alertEvent(event);
    emit alert(event.message, severity, event.id);
    if (settings().traffic.alerts == GeoZoneTrafficMonitor::SpeakAlerts) {
        // Not about a vehicle, so never a breach nor muted with a zone
        GeoZoneAlertArbiter::Alert announced;
        announced.id = event.id;
        announced.zoneKey = event.zoneKey;
        announced.severity = severity;
        announced.inside = true;
        announced.message = event.message;
        _arbiter.submit(announced);
    }
}

void GeoZoneMonitor::_updateEvaluateTimer()
{
    if (monitoring()) {
        if (!_evaluateTimer.isActive()) {
            _evaluateTimer.start();
            evaluateVehicles();
        }
    } else {
        _evaluateTimer.stop();
    }
}

void GeoZoneMonitor::evaluateVehicles()
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

void GeoZoneMonitor::positionLost(int vehicleId)
{
    GeoZoneVehicleMonitor *const monitor = positionMonitor(vehicleId);
    if (!monitor) {
        return;
    }
    monitor->setPositionUnavailable(true);
    _announce();
}

void GeoZoneMonitor::_evaluate(GeoZoneVehicleMonitor *monitor, const GeoZone::Position &position, const Velocity &velocity)
{
    // A position is known again, resuming the breaches before the vehicle may leave their zones
    monitor->setPositionUnavailable(false);
    _raise(monitor, monitor->evaluate(position, velocity, _context()));
}

void GeoZoneMonitor::_raise(GeoZoneVehicleMonitor *monitor, const QList<GeoZoneAlertTracker::Alert> &alerts)
{
    for (const GeoZoneAlertTracker::Alert &zoneAlert : alerts) {
        const AlertEvent::Kind kind = zoneAlert.inside ? AlertEvent::Entry : AlertEvent::Approach;
        _submit(monitor, _alertEvent(zoneAlert.zone, kind, zoneAlert.inside, zoneAlert.distance, zoneAlert.timeToEntry, monitor->position(), monitor->vehicleId()), zoneAlert.zone);
    }
    // Breaches and zone mutes end when the vehicle leaves the zone. Without a current position the zones it is in
    // aren't known, the nearby zones are those of its last position.
    if (!monitor->positionUnavailable()) {
        const int muteCount = _arbiter.zoneMuteCount();
        _arbiter.setInsideZones(monitor->key(), monitor->insideZoneKeys());
        _zoneMutesChanged(muteCount);
    }
}

void GeoZoneMonitor::_submit(GeoZoneVehicleMonitor *monitor, const AlertEvent &event, const GeoZone *zone)
{
    const int severity = GeoZone::restrictionSeverity(event.restriction);
    _alertSubjects.insert(event.id, AlertSubject{ monitor->key(), event.zoneKey });
    (void) _alertSubjects.remove(event.id - maxActionAlerts);
    emit alertEvent(event);
    // A zone muted until the vehicle leaves it is logged but not shown
    if (!_arbiter.zoneMuted(monitor->key(), event.zoneKey)) {
        emit alert(event.message, severity, event.id);
    }

    GeoZoneAlertArbiter::Alert announced;
    announced.id = event.id;
    announced.vehicleKey = monitor->key();
    announced.zoneKey = event.zoneKey;
    announced.severity = severity;
    // A lost position is as urgent as an entry
    announced.inside = (event.kind != AlertEvent::Approach);
    announced.breach = (event.kind == AlertEvent::Entry) && (event.restriction == GeoZone::Prohibited);
    announced.message = event.message;
    if (announced.breach) {
        announced.repeatMessage = _labelled(monitor->vehicleId(), tr("Still inside geozone %1 (%2)").arg(zone->displayName(), zone->restrictionText()));
    }
    _arbiter.submit(announced);
}

void GeoZoneMonitor::_positionAvailabilityChanged(GeoZoneVehicleMonitor *monitor)
{
    const bool lost = monitor->positionUnavailable();
    _arbiter.setSuspended(monitor->key(), lost);
    if (!lost || monitor->nearby().isEmpty() || !settings().alertsEnabled) {
        return;
    }
    // The vehicle may still be where it was last seen: in or near the most restrictive zone around it
    const GeoZoneAlertTracker::Nearby &nearby = monitor->nearby().constFirst();
    _submit(monitor, _alertEvent(nearby.zone, AlertEvent::PositionLost, nearby.inside, nearby.inside ? 0 : nearby.distance, qQNaN(), monitor->position(), monitor->vehicleId()), nearby.zone);
}

void GeoZoneMonitor::_announce()
{
    const GeoZoneAlertArbiter::Announcement announcement = _arbiter.tick();
    if (!announcement.text.isEmpty()) {
        qCDebug(GeoZoneMonitorLog) << "Announced:" << announcement.text;
        // Speech is one output of the announcements, the repeats are shown also without it
        if (settings().audioAlerts && _audioSink) {
            _audioSink->say(announcement.text);
        }
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

QVariantMap GeoZoneMonitor::alertActions(int alertId) const
{
    const auto subject = _alertSubjects.constFind(alertId);
    const GeoZoneVehicleMonitor *const monitor = (subject != _alertSubjects.cend()) ? _monitorByKey(subject->vehicleKey) : nullptr;
    if (!monitor) {
        return {};
    }
    const QList<GeoZoneAlertTracker::Nearby> &nearby = monitor->nearby();
    const auto zone = std::find_if(nearby.cbegin(), nearby.cend(), [&subject](const GeoZoneAlertTracker::Nearby &candidate) {
        return candidate.zone->key() == subject->zoneKey;
    });
    const bool known = (zone != nearby.cend()) && !monitor->positionUnavailable();
    return QVariantMap{
        { QStringLiteral("vehicleId"), monitor->vehicleId() },
        { QStringLiteral("zoneKey"), subject->zoneKey },
        { QStringLiteral("activeVehicle"), monitor == _activeView->monitor() },
        { QStringLiteral("inside"), known && zone->inside },
        { QStringLiteral("zoneMuted"), _arbiter.zoneMuted(subject->vehicleKey, subject->zoneKey) },
        { QStringLiteral("climbHeight"), known ? _climbHeight(monitor, zone->zone) : qQNaN() },
    };
}

void GeoZoneMonitor::muteZone(int alertId)
{
    const auto subject = _alertSubjects.constFind(alertId);
    if ((subject == _alertSubjects.cend()) || !_monitorByKey(subject->vehicleKey)) {
        return;
    }
    const int muteCount = _arbiter.zoneMuteCount();
    _arbiter.muteZone(subject->vehicleKey, subject->zoneKey);
    _zoneMutesChanged(muteCount);
}

QStringList GeoZoneMonitor::mutedZoneKeys() const
{
    const GeoZoneVehicleMonitor *const monitor = _activeView->monitor();
    return monitor ? QStringList(_arbiter.mutedZones(monitor->key()).values()) : QStringList();
}

void GeoZoneMonitor::_zoneMutesChanged(int previousCount)
{
    if (_arbiter.zoneMuteCount() != previousCount) {
        emit zoneMutesChanged();
    }
}

GeoZoneVehicleMonitor *GeoZoneMonitor::_monitorByKey(int key) const
{
    const auto it = std::find_if(_monitors.cbegin(), _monitors.cend(), [key](const GeoZoneVehicleMonitor *monitor) { return monitor->key() == key; });
    return (it != _monitors.cend()) ? *it : nullptr;
}

double GeoZoneMonitor::_climbHeight(const GeoZoneVehicleMonitor *monitor, const GeoZone *zone)
{
    const GeoZone::Position &position = monitor->position();
    if (std::isnan(position.altitudeAMSL)) {
        return qQNaN();
    }
    // The volumes over the vehicle, or all of them when it is beside the zone
    const QList<GeoZone::Volume> &volumes = zone->data().volumes;
    QList<qsizetype> indexes;
    for (qsizetype i = 0; i < volumes.count(); i++) {
        if (zone->volumeContains(i, position.coordinate)) {
            indexes.append(i);
        }
    }
    if (indexes.isEmpty()) {
        for (qsizetype i = 0; i < volumes.count(); i++) {
            indexes.append(i);
        }
    }
    double ceiling = qQNaN();
    for (const qsizetype i : std::as_const(indexes)) {
        const double upper = GeoZone::limitAMSL(volumes[i], true, position);
        if (std::isnan(upper)) {
            return qQNaN();
        }
        ceiling = std::isnan(ceiling) ? upper : (std::max)(ceiling, upper);
    }
    const double height = ceiling + climbClearance - position.altitudeAMSL;
    return (height > 0) ? height : qQNaN();
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

void GeoZoneMonitor::_forgetZones(GeoZoneSource *source)
{
    // Zones of the source are about to be deleted. Their alert states are kept so reloading the source doesn't repeat alerts.
    _policy.reset();
    for (GeoZoneVehicleMonitor *const monitor : std::as_const(_monitors)) {
        monitor->forgetZones(source->key());
    }
    _traffic->forgetZones(source->key());
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

QString GeoZoneMonitor::alertMessage(const AlertEvent &event, const QString &zoneName, const QString &restrictionText)
{
    switch (event.kind) {
    case AlertEvent::Entry:
        return tr("Entered geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::PositionLost:
        return event.inside ? tr("Position lost inside geozone %1 (%2)").arg(zoneName, restrictionText) :
                              tr("Position lost near geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Traffic:
        return tr("Traffic %1 entered geozone %2 (%3)").arg(event.traffic, zoneName, restrictionText);
    case AlertEvent::Approach:
        break;
    }
    if (!std::isnan(event.timeToEntry)) {
        return tr("Approaching geozone %1 (%2), entry in about %3 s").arg(zoneName, restrictionText).arg(std::lround(event.timeToEntry));
    }
    return tr("Approaching geozone %1 (%2)").arg(zoneName, restrictionText);
}

GeoZoneMonitor::AlertEvent GeoZoneMonitor::_alertEvent(const GeoZone *zone, AlertEvent::Kind kind, bool inside, double distance, double timeToEntry, const GeoZone::Position &position, int vehicleId, const QString &traffic)
{
    AlertEvent event;
    event.id = _nextAlertId++;
    event.time = GeoZoneEnvironment::currentDateTimeUtc();
    event.vehicleId = vehicleId;
    event.position = position;
    event.kind = kind;
    event.inside = inside;
    event.distance = distance;
    event.zoneIdentifier = zone->identifier();
    event.zoneName = zone->name();
    event.restriction = zone->restriction();
    event.sourceText = zone->sourceText();
    event.zoneKey = zone->key();
    event.timeToEntry = timeToEntry;
    event.traffic = traffic;
    event.message = _labelled(vehicleId, alertMessage(event, zone->displayName(), zone->restrictionText()));
    qCDebug(GeoZoneMonitorLog) << event.message << "distance" << distance << "time to entry" << timeToEntry;
    return event;
}
