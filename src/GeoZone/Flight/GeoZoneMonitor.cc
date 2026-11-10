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
#include "GeoZoneAlertHistory.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneFenceWarning.h"
#include "GeoZoneGuidedCheck.h"
#include "GeoZoneInfo.h"
#include "GeoZoneIntendedLevel.h"
#include "GeoZoneManager.h"
#include "GeoZonePermits.h"
#include "GeoZonePlanning.h"
#include "GeoZonePolicy.h"
#include "GeoZoneSettings.h"
#include "GeoZoneSource.h"
#include "GeoZoneTerrain.h"
#include "GeoZoneText.h"
#include "GeoZoneVertical.h"
#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

#include <QtCore/QtMath>
#include <QtCore/qapplicationstatic.h>

#include <algorithm>
#include <cmath>
#include <utility>

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
    _alertHistory->startLog();
    _audioSink = std::make_shared<ApplicationAudioSink>();

    // Without zones, alerts or vehicles to evaluate, the geo-awareness is still followed
    _awarenessTimer.setInterval(evaluateIntervalMSecs);
    (void) connect(&_awarenessTimer, &QTimer::timeout, this, [this]() {
        if (!_evaluateTimer.isActive()) {
            _updateAwareness();
            _announce();
        }
    });
    _awarenessTimer.start();

    // Evaluated periodically, since a hovering or climbing vehicle doesn't report a new coordinate, with the traffic
    _evaluateTimer.setInterval(evaluateIntervalMSecs);
    (void) connect(&_evaluateTimer, &QTimer::timeout, this, &GeoZoneMonitor::evaluateVehicles);
    (void) connect(this, &GeoZoneMonitor::monitoringChanged, this, &GeoZoneMonitor::_updateEvaluateTimer);
    _adsbTraffic = true;

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
    , _guidedCheck(new GeoZoneGuidedCheck(this))
    , _alertHistory(new GeoZoneAlertHistory(_manager, this))
    , _alertModel(new GeoZoneAlertModel(this))
    , _awareness(new GeoZoneAwareness(this))
    , _permits(new GeoZonePermits(this))
{
    _elapsed.start();
    _arbiter.setClock([this]() { return _now(); });
    _muteTimer.setSingleShot(true);
    (void) connect(&_muteTimer, &QTimer::timeout, this, &GeoZoneMonitor::_updateMuted);

    (void) connect(this, &GeoZoneMonitor::alertEvent, _alertHistory, &GeoZoneAlertHistory::append);
    // Alerts acknowledged in the history are no longer repeated
    (void) connect(_alertHistory, &GeoZoneAlertHistory::alertAcknowledged, this, [this](int alertId) { _arbiter.acknowledge(alertId); });
    (void) connect(_alertHistory, &GeoZoneAlertHistory::allAcknowledged, this, [this]() { _arbiter.acknowledgeAll(); });

    // Zones are approved for the active vehicle, whose alerts offer its actions
    (void) connect(this, &GeoZoneMonitor::activeMonitorChanged, this, &GeoZoneMonitor::approvalsChanged);
    (void) connect(this, &GeoZoneMonitor::activeMonitorChanged, this, &GeoZoneMonitor::_updateAlertActions);
    // Fences stop or return the vehicle, shown above the geozone alerts
    (void) connect(this, &GeoZoneMonitor::fenceBreach, this, [this](const QString &message) { _show(_nextAlertId++, message, -1, GeoZoneAlertLevel::Warning); });
    (void) connect(this, &GeoZoneMonitor::approvalsChanged, this, &GeoZoneMonitor::_updateActiveLevel);

    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMonitor::_forgetZones);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMonitor::_zonesChanged);
    (void) connect(_manager->repository(), &GeoZoneSourceRepository::validityWarningChanged, this, &GeoZoneMonitor::_updateAwareness);
    (void) connect(this, &GeoZoneMonitor::activeMonitorChanged, this, &GeoZoneMonitor::_updateAwareness);

    // Permits acknowledged from QML are for the active vehicle's flight
    (void) connect(this, &GeoZoneMonitor::activeMonitorChanged, this, [this]() { _permits->setVehicleId(_activeMonitor ? _activeMonitor->vehicleId() : noVehicle); });
    (void) connect(_permits, &GeoZonePermits::acknowledged, this, [this](const GeoZonePermits::Permit &permit) {
        _logPermit(permit, AlertEvent::Permit, GeoZonePermits::originText(permit.origin));
        _permitsChanged(QString());
    });
    (void) connect(_permits, &GeoZonePermits::ended, this, [this](const GeoZonePermits::Permit &permit, const QString &reason) {
        _logPermit(permit, AlertEvent::PermitEnded, reason);
        _permitsChanged((reason == QStringLiteral("withdrawn")) ? permit.key : QString());
    });

    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    for (Fact *const fact : { settings->audioAlerts(), settings->approachExtraDistance(), settings->approachExtraTime(),
                              settings->trafficEnabled() }) {
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
    result.audioAlerts = settings->audioAlerts()->rawValue().toBool();
    result.extras.distance = settings->approachExtraDistance()->rawValue().toDouble();
    result.extras.time = settings->approachExtraTime()->rawValue().toDouble();
    result.trafficEnabled = settings->trafficEnabled()->rawValue().toBool();
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
    Q_UNUSED(previous);
    if (!settings().trafficEnabled) {
        _clearTraffic();
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
    return GeoZoneVehicleMonitor::Context{ &_manager->catalog(), *_policy, settings().extras, settings().limits };
}

GeoZoneVehicleMonitor *GeoZoneMonitor::_createMonitor(Vehicle *vehicle, int vehicleId)
{
    GeoZoneVehicleMonitor *const monitor = new GeoZoneVehicleMonitor(vehicle, vehicleId, _nextVehicleKey++, [this]() { return _now(); }, this);
    _monitors.append(monitor);

    (void) connect(monitor, &GeoZoneVehicleMonitor::fenceBreachStarted, this, [this, monitor](const QString &text) {
        const QString message = _labelled(monitor->vehicleId(), text);
        _logFenceEvent(monitor, AlertEvent::FenceBreach, _nextAlertId, message);
        emit fenceBreach(message);
    });
    if (GeoZoneFenceWarning *const fenceWarning = monitor->fenceWarning()) {
        (void) connect(fenceWarning, &GeoZoneFenceWarning::warningChanged, this, [this, monitor]() { _updateFenceWarning(monitor); });
    }
    (void) connect(monitor, &GeoZoneVehicleMonitor::positionUnavailableChanged, this, [this, monitor]() {
        _positionAvailabilityChanged(monitor);
        _updateAlertActions();
    });
    (void) connect(monitor, &GeoZoneVehicleMonitor::armingAttempted, this, [this, monitor]() { _armingAttempted(monitor); });
    (void) connect(monitor, &GeoZoneVehicleMonitor::armedChanged, this, [this, monitor]() {
        // Arming with the remote control or a script is an attempt too
        if (monitor->inFlight()) {
            _flying.insert(monitor->key());
            _armingAttempted(monitor);
        } else if (monitor->onGround() && _flying.remove(monitor->key())) {
            _permits->endFlight(monitor->vehicleId(), QStringLiteral("flight ended"));
        }
        _updateGroundStatus(monitor);
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
    (void) _armingAlerts.remove(monitor->key());
    (void) _awarenessTrackers.remove(monitor->key());
    (void) _flying.remove(monitor->key());
    if (monitor->followsVehicle()) {
        _permits->endFlight(monitor->vehicleId(), QStringLiteral("vehicle disconnected"));
    }
    const int fenceWarningId = _fenceWarningAlerts.take(monitor->key());
    if (fenceWarningId > 0) {
        _alertModel->removeIf([fenceWarningId](const GeoZoneAlertModel::Alert &alert) { return alert.alertId == fenceWarningId; });
    }
    const int approvalCount = _arbiter.approvalCount();
    _arbiter.removeVehicle(monitor->key());
    _approvalsChanged(approvalCount);
    (void) monitor->disconnect(this);
    // Removed while the vehicle emits a signal, or from a slot of the monitor
    monitor->deleteLater();
    _resolveActive();
    _updateAlertActions();
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
    _resolveActive();
}

void GeoZoneMonitor::setActiveVehicleId(int vehicleId)
{
    if (!_activeVehicle && (vehicleId == _activeVehicleId)) {
        return;
    }
    _activeVehicle = nullptr;
    _activeVehicleId = vehicleId;
    _resolveActive();
}

void GeoZoneMonitor::_resolveActive()
{
    GeoZoneVehicleMonitor *const monitor = _activeVehicle ? vehicleMonitor(_activeVehicle) : positionMonitor(_activeVehicleId);
    if (monitor != _activeMonitor) {
        _activeMonitor = monitor;
        emit activeMonitorChanged();
    }
    _updateActiveLevel();
}

QString GeoZoneMonitor::Aircraft::label() const
{
    // Callsigns are padded, some senders with null characters
    QString printable;
    for (const QChar character : callsign) {
        if (character.isPrint()) {
            printable.append(character);
        }
    }
    printable = printable.trimmed();
    return printable.isEmpty() ? QStringLiteral("%1").arg(address, 6, 16, QLatin1Char('0')).toUpper() : printable;
}

void GeoZoneMonitor::_evaluateADSBTraffic()
{
    const QmlObjectListModel *const adsbVehicles = ADSBVehicleManager::instance()->adsbVehicles();
    QList<Aircraft> aircraft;
    aircraft.reserve(adsbVehicles->count());
    for (int i = 0; i < adsbVehicles->count(); i++) {
        const ADSBVehicle *const adsbVehicle = adsbVehicles->value<const ADSBVehicle*>(i);
        if (!adsbVehicle) {
            continue;
        }
        Aircraft plane;
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

void GeoZoneMonitor::evaluateTraffic(const QList<Aircraft> &aircraft)
{
    // The traffic around the active vehicle, where it is now
    const GeoZoneVehicleMonitor *const active = _activeMonitor;
    if (!settings().trafficEnabled || !active || active->positionUnavailable() || !active->position().coordinate.isValid()) {
        _clearTraffic();
        return;
    }
    const GeoZone::Position &center = active->position();
    const GeoZoneVehicleMonitor::Context context = _context();

    // The closest aircraft within the radius
    QList<std::pair<double, const Aircraft*>> nearby;
    for (const Aircraft &candidate : aircraft) {
        if (!candidate.coordinate.isValid()) {
            continue;
        }
        const double distance = center.coordinate.distanceTo(candidate.coordinate);
        if (distance <= trafficRadius()) {
            nearby.append({ distance, &candidate });
        }
    }
    std::sort(nearby.begin(), nearby.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    if (nearby.count() > maxTrafficAircraft) {
        nearby.resize(maxTrafficAircraft);
    }

    // Aircraft beyond the radius or gone are forgotten, so they alert again when they come back
    QHash<quint32, GeoZoneAlertTracker> trackers;
    QList<ZoneTraffic> traffic;
    QList<AlertEvent> entries;
    for (const auto &[distance, plane] : std::as_const(nearby)) {
        Q_UNUSED(distance);
        GeoZoneAlertTracker tracker = _trafficTrackers.take(plane->address);
        // Without terrain data below the aircraft, the ground below the active vehicle is closer than none, which
        // would put aircraft at any height into zones limited above the ground
        GeoZone::Position position{ plane->coordinate, plane->coordinate.altitude(), GeoZoneTerrain::elevationAt(plane->coordinate) };
        if (std::isnan(position.terrainAMSL)) {
            position.terrainAMSL = center.terrainAMSL;
        }
        GeoZoneAlertTracker::Velocity velocity;
        if (!std::isnan(plane->heading) && !std::isnan(plane->speed)) {
            const double heading = qDegreesToRadians(plane->heading);
            velocity = GeoZoneAlertTracker::Velocity{ plane->speed * std::cos(heading), plane->speed * std::sin(heading), plane->verticalSpeed, true };
        }
        // The thresholds of the default profile at the aircraft's velocity
        const GeoZoneAlertTracker::Limits limits = context.limits.value_or(
            GeoZoneAlertThresholds::compute(GeoZoneAlertThresholds::profile(QString::fromLatin1(GeoZoneAlertThresholds::defaultClass)), { velocity }).limits());
        const GeoZoneAlertTracker::Update update = tracker.update(*context.catalog, context.policy, position, velocity, limits);
        for (const GeoZoneAlertTracker::Alert &zoneAlert : update.alerts) {
            if (zoneAlert.inside) {
                AlertEvent event = _alertEvent(zoneAlert.zone, AlertEvent::Traffic, true, 0, qQNaN(), position, noVehicle, plane->label());
                event.level = alertLevel(event);
                entries.append(event);
            }
        }
        if (!update.nearby.isEmpty()) {
            traffic.append(ZoneTraffic{ *plane, update.nearby.constFirst() });
        }
        trackers.insert(plane->address, std::move(tracker));
    }
    _trafficTrackers = std::move(trackers);

    // Inside first, then the most restrictive zones, then the closest
    std::stable_sort(traffic.begin(), traffic.end(), [](const ZoneTraffic &a, const ZoneTraffic &b) {
        if (a.nearby.inside != b.nearby.inside) {
            return a.nearby.inside;
        }
        const int severityA = GeoZone::restrictionSeverity(a.nearby.zone->restriction());
        const int severityB = GeoZone::restrictionSeverity(b.nearby.zone->restriction());
        return (severityA != severityB) ? (severityA < severityB) : (a.nearby.distance < b.nearby.distance);
    });
    _setTraffic(traffic);

    // Not about a vehicle, so neither spoken, repeated nor approved
    for (const AlertEvent &event : std::as_const(entries)) {
        _alertSubjects.insert(event.id, AlertSubject{ 0, event, false });
        (void) _alertSubjects.remove(event.id - maxActionAlerts);
        emit alertEvent(event);
        const int severity = trafficSeverity + GeoZoneAlertLevel::severity(event.level, GeoZone::restrictionSeverity(event.restriction));
        emit alert(event.message, severity, event.id);
        _show(event.id, event.message, severity, event.level);
    }
}

void GeoZoneMonitor::_setTraffic(const QList<ZoneTraffic> &traffic)
{
    if (traffic.isEmpty() && _traffic.isEmpty()) {
        return;
    }
    _traffic = traffic;
    emit trafficChanged();
}

void GeoZoneMonitor::_clearTraffic()
{
    _trafficTrackers.clear();
    _setTraffic({});
}

QVariantList GeoZoneMonitor::traffic() const
{
    QVariantList aircraft;
    for (const ZoneTraffic &traffic : _traffic) {
        aircraft.append(QVariantMap{
            { QStringLiteral("callsign"), traffic.aircraft.label() },
            { QStringLiteral("zone"), QVariant::fromValue(GeoZoneInfo(traffic.nearby.zone)) },
            { QStringLiteral("inside"), traffic.nearby.inside },
            { QStringLiteral("distance"), traffic.nearby.distance },
            { QStringLiteral("altitude"), traffic.aircraft.coordinate.altitude() },
        });
    }
    return aircraft;
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
        _clearTraffic();
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
    if (_adsbTraffic && settings().trafficEnabled) {
        _evaluateADSBTraffic();
    }
    _updateAwareness();
    _announce();
}

void GeoZoneMonitor::evaluate(Vehicle *vehicle, const GeoZone::Position &position, const Velocity &velocity)
{
    GeoZoneVehicleMonitor *const monitor = vehicleMonitor(vehicle);
    if (!monitor || !position.coordinate.isValid()) {
        return;
    }
    _evaluate(monitor, position, velocity);
    _updateAwareness();
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
    _updateAwareness();
    _announce();
}

void GeoZoneMonitor::positionLost(int vehicleId)
{
    GeoZoneVehicleMonitor *const monitor = positionMonitor(vehicleId);
    if (!monitor) {
        return;
    }
    monitor->setPositionUnavailable(true);
    _updateAwareness();
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
        // Like the pre-arm checks of the vehicle, a vehicle on the ground is told it is inside the zone
        const AlertEvent::Kind kind = !zoneAlert.inside ? AlertEvent::Approach
                                                        : (monitor->onGround() ? AlertEvent::OnGround
                                                                               : (zoneAlert.activated ? AlertEvent::Activated : AlertEvent::Entry));
        AlertEvent event = _alertEvent(zoneAlert.zone, kind, zoneAlert.inside, zoneAlert.distance, zoneAlert.timeToEntry, monitor->position(), monitor->vehicleId());
        event.level = alertLevel(event, zoneAlert.imminent);
        // The thresholds which raised it, logged with it
        if (!settings().limits) {
            const GeoZoneAlertThresholds::Result &thresholds = monitor->thresholds().result();
            event.warningDistance = thresholds.distance;
            event.warningVerticalDistance = thresholds.verticalDistance;
            event.warningTime = thresholds.time;
            event.groundSpeed = thresholds.groundSpeed;
            event.verticalSpeed = thresholds.verticalSpeed;
        }
        _submit(monitor, event, zoneAlert.zone);
    }
    // Breaches end when the vehicle leaves the zone, approvals when it leaves its approach range too. Without a
    // current position the zones it is in aren't known, the nearby zones are those of its last position.
    if (!monitor->positionUnavailable()) {
        const int approvalCount = _arbiter.approvalCount();
        _arbiter.setZones(monitor->key(), monitor->insideZoneKeys(), monitor->nearbyZoneKeys());
        _approvalsChanged(approvalCount);
    }
    _updateGroundStatus(monitor);
}

void GeoZoneMonitor::_submit(GeoZoneVehicleMonitor *monitor, const AlertEvent &raised, const GeoZone *zone)
{
    AlertEvent event = raised;
    const bool zoneAlert = (event.kind == AlertEvent::Approach) || (event.kind == AlertEvent::Entry) || (event.kind == AlertEvent::OnGround) ||
                           (event.kind == AlertEvent::Activated);
    // With a permit for the flight, the alerts are logged and an entry is shown as an advisory, not spoken
    const bool permitted = zoneAlert && _permits->covers(zone);
    if (permitted) {
        event.detail = QStringLiteral("permit");
        event.message = _labelled(monitor->vehicleId(), alertMessage(event, zone->displayName(), GeoZoneText::restriction(zone->restriction())));
    }
    // The pilot has the permission to fly in an approved zone
    const bool approved = _arbiter.zoneApproved(monitor->key(), event.zoneKey);
    if (permitted || approved) {
        event.level = GeoZoneAlertLevel::Advisory;
    }
    const int severity = GeoZoneAlertLevel::severity(event.level, GeoZone::restrictionSeverity(event.restriction));
    const bool asks = zoneAlert && asksPermission(event.restriction) && !permitted;
    _alertSubjects.insert(event.id, AlertSubject{ monitor->key(), event, asks, GeoZonePermits::applies(zone) ? GeoZonePermits::key(zone) : QString() });
    (void) _alertSubjects.remove(event.id - maxActionAlerts);
    emit alertEvent(event);
    if (permitted) {
        _alertHistory->acknowledgeAlert(event.id);
        if (event.kind != AlertEvent::Approach) {
            emit alert(event.message, severity, event.id);
            _show(event.id, event.message, severity, event.level);
        }
        return;
    }
    // A zone approved until the vehicle leaves it is logged but not shown, the pilot answered already
    if (approved) {
        _alertHistory->acknowledgeAlert(event.id);
    } else {
        emit alert(event.message, severity, event.id);
        _show(event.id, event.message, severity, event.level);
    }

    GeoZoneAlertArbiter::Alert announced;
    announced.id = event.id;
    announced.vehicleKey = monitor->key();
    announced.zoneKey = event.zoneKey;
    announced.severity = severity;
    announced.level = event.level;
    // A lost position is as urgent as an entry
    announced.inside = (event.kind != AlertEvent::Approach);
    announced.breach = ((event.kind == AlertEvent::Entry) || (event.kind == AlertEvent::OnGround) || (event.kind == AlertEvent::Activated)) &&
                       (event.restriction == GeoZone::Prohibited);
    announced.message = event.message;
    if (announced.breach) {
        announced.repeatMessage = _labelled(monitor->vehicleId(), tr("Still inside geozone %1 (%2)").arg(zone->displayName(), GeoZoneText::restriction(zone->restriction())));
    }
    _arbiter.submit(announced);
}

void GeoZoneMonitor::_updateFenceWarning(GeoZoneVehicleMonitor *monitor)
{
    const GeoZoneFenceWarning *const warning = monitor->fenceWarning();
    const int alertId = _fenceWarningAlerts.value(monitor->key());
    if (!warning->active()) {
        if (alertId > 0) {
            _fenceWarningAlerts.remove(monitor->key());
            _arbiter.acknowledge(alertId);
            _alertModel->removeIf([alertId](const GeoZoneAlertModel::Alert &alert) { return alert.alertId == alertId; });
        }
        return;
    }
    const QString message = _labelled(monitor->vehicleId(), warning->text());
    if (alertId == 0) {
        // Above the geozone alerts like the fence breaches, and more urgent than any of them
        const int id = _nextAlertId++;
        _fenceWarningAlerts.insert(monitor->key(), id);
        _logFenceEvent(monitor, AlertEvent::FenceWarning, id, message);
        emit fenceWarning(message);
        _show(id, message, -1);
        GeoZoneAlertArbiter::Alert announced;
        announced.id = id;
        announced.vehicleKey = monitor->key();
        announced.zoneKey = QStringLiteral("fence");
        announced.severity = -1;
        announced.inside = true;
        announced.message = message;
        _arbiter.submit(announced);
        _announce();
        return;
    }
    // The time and distance count down while the pilot keeps the alert, which keeps the popup open
    const QList<GeoZoneAlertModel::Alert> &shown = _alertModel->alerts();
    const auto row = std::find_if(shown.cbegin(), shown.cend(), [alertId](const GeoZoneAlertModel::Alert &alert) { return alert.alertId == alertId; });
    if ((row != shown.cend()) && (row->message != message)) {
        _show(alertId, message, -1);
    }
}

void GeoZoneMonitor::_logFenceEvent(GeoZoneVehicleMonitor *monitor, AlertEvent::Kind kind, int alertId, const QString &message)
{
    AlertEvent event;
    event.id = alertId;
    event.time = GeoZoneEnvironment::currentDateTimeUtc();
    event.vehicleId = monitor->vehicleId();
    event.position = monitor->position();
    event.kind = kind;
    event.zoneIdentifier = QStringLiteral("FENCE");
    event.message = message;
    const GeoZoneFenceWarning *const warning = monitor->fenceWarning();
    if (warning) {
        if (!event.position.coordinate.isValid()) {
            event.position = GeoZone::Position{ warning->position() };
        }
        event.detail = tr("Fence action: %1").arg(warning->actionText());
    }
    if ((kind == AlertEvent::FenceWarning) && warning && warning->breach()) {
        const GeoZoneFencePrediction::Breach &breach = *warning->breach();
        event.distance = breach.distance;
        event.timeToEntry = breach.time;
        switch (breach.boundary) {
        case GeoZoneFencePrediction::Boundary::Polygon:
            event.zoneName = breach.inclusion ? tr("Inclusion polygon %1").arg(breach.index + 1) : tr("Exclusion polygon %1").arg(breach.index + 1);
            break;
        case GeoZoneFencePrediction::Boundary::Circle:
            event.zoneName = breach.inclusion ? tr("Inclusion circle %1").arg(breach.index + 1) : tr("Exclusion circle %1").arg(breach.index + 1);
            break;
        case GeoZoneFencePrediction::Boundary::MaxDistance:
            event.zoneName = tr("Maximum distance from home");
            break;
        case GeoZoneFencePrediction::Boundary::MaxAltitude:
            event.zoneName = tr("Maximum altitude");
            break;
        case GeoZoneFencePrediction::Boundary::MinAltitude:
            event.zoneName = tr("Minimum altitude");
            break;
        }
    } else {
        event.zoneName = message;
    }
    qCDebug(GeoZoneMonitorLog) << message;
    emit alertEvent(event);
}

void GeoZoneMonitor::_positionAvailabilityChanged(GeoZoneVehicleMonitor *monitor)
{
    const bool lost = monitor->positionUnavailable();
    _arbiter.setSuspended(monitor->key(), lost);
    if (!lost || monitor->nearby().isEmpty()) {
        return;
    }
    // The vehicle may still be where it was last seen: in or near the most restrictive zone around it
    const GeoZoneAlertTracker::Nearby &nearby = monitor->nearby().constFirst();
    _submit(monitor, _alertEvent(nearby.zone, AlertEvent::PositionLost, nearby.inside, nearby.inside ? 0 : nearby.distance, qQNaN(), monitor->position(), monitor->vehicleId()), nearby.zone);
}

void GeoZoneMonitor::_armingAttempted(GeoZoneVehicleMonitor *monitor)
{
    if (!settings().alertsEnabled || monitor->positionUnavailable()) {
        return;
    }
    // The vehicle acknowledges the command before it reports being armed
    const qint64 now = _now();
    const auto last = _armingAlerts.constFind(monitor->key());
    if ((last != _armingAlerts.cend()) && ((now - *last) < armingAlertIntervalMSecs)) {
        return;
    }
    bool alerted = false;
    for (const GeoZoneAlertTracker::Nearby &nearby : monitor->nearby()) {
        if (!nearby.inside || !GeoZonePolicy::isRestricted(nearby.zone->restriction()) || _permits->covers(nearby.zone)) {
            continue;
        }
        AlertEvent event = _alertEvent(nearby.zone, AlertEvent::OnGround, true, 0, qQNaN(), monitor->position(), monitor->vehicleId());
        event.detail = QStringLiteral("arming");
        event.message = _labelled(monitor->vehicleId(), alertMessage(event, nearby.zone->displayName(), GeoZoneText::restriction(nearby.zone->restriction())));
        _submit(monitor, event, nearby.zone);
        alerted = true;
    }
    if (alerted) {
        _armingAlerts.insert(monitor->key(), now);
        _announce();
    }
}

int GeoZoneMonitor::groundLevel(GeoZone::Restriction restriction, bool permitted)
{
    if (permitted || !GeoZonePolicy::isRestricted(restriction)) {
        return GeoZoneIntendedLevel::Advisory;
    }
    return (restriction == GeoZone::Prohibited) ? GeoZoneIntendedLevel::Warning : GeoZoneIntendedLevel::Caution;
}

void GeoZoneMonitor::_updateGroundStatus(GeoZoneVehicleMonitor *monitor)
{
    QString warning;
    int level = GeoZoneIntendedLevel::Advisory;
    if (monitor->onGround() && !monitor->positionUnavailable() && settings().alertsEnabled) {
        // The most restrictive zone first
        for (const GeoZoneAlertTracker::Nearby &nearby : monitor->nearby()) {
            const GeoZone *const zone = nearby.zone;
            if (!nearby.inside || !GeoZonePolicy::isRestricted(zone->restriction())) {
                continue;
            }
            const bool approved = _arbiter.zoneApproved(monitor->key(), zone->key());
            AlertEvent event;
            event.kind = AlertEvent::OnGround;
            event.restriction = zone->restriction();
            event.detail = _permits->covers(zone) ? QStringLiteral("permit") : QString();
            warning = approved ? tr("On the ground inside geozone %1 (%2), permission confirmed").arg(zone->displayName(), GeoZoneText::restriction(zone->restriction())) :
                                 alertMessage(event, zone->displayName(), GeoZoneText::restriction(zone->restriction()));
            level = groundLevel(zone->restriction(), approved || !event.detail.isEmpty());
            break;
        }
    }
    monitor->setGroundStatus(warning, level);
}

GeoZoneAwareness::Status GeoZoneMonitor::awarenessStatus(const GeoZoneVehicleMonitor *monitor) const
{
    const bool positioned = monitor && !monitor->positionUnavailable() && monitor->position().coordinate.isValid();
    GeoZoneAwareness::Input input = GeoZoneAwareness::dataInput(_manager, positioned ? monitor->position().coordinate : QGeoCoordinate());
    if (monitor) {
        Vehicle *const vehicle = monitor->vehicle();
        const VehicleLinkManager *const links = vehicle ? vehicle->vehicleLinkManager() : nullptr;
        input.vehicle = true;
        input.linkLost = links && links->communicationLost();
        input.positionUnavailable = !positioned;
        input.gnss = monitor->gnss(_now());
    }
    return GeoZoneAwareness::evaluate(input);
}

void GeoZoneMonitor::_updateAwareness()
{
    const qint64 now = _now();
    for (GeoZoneVehicleMonitor *const monitor : std::as_const(_monitors)) {
        const GeoZoneAwareness::Status status = awarenessStatus(monitor);
        // Of flying vehicles, like those whose link is lost, which were flying when last seen
        if (const std::optional<GeoZoneAwareness::Tracker::Change> change = _awarenessTrackers[monitor->key()].update(status, monitor->inFlight(), now)) {
            _raiseAwarenessAlert(monitor, *change);
        }
    }
    _awareness->setStatus(awarenessStatus(_activeMonitor));
}

void GeoZoneMonitor::_raiseAwarenessAlert(GeoZoneVehicleMonitor *monitor, const GeoZoneAwareness::Tracker::Change &change)
{
    if (!settings().alertsEnabled) {
        return;
    }
    const int level = GeoZoneAwareness::alertLevel(change.to);
    AlertEvent event;
    event.id = _nextAlertId++;
    event.time = GeoZoneEnvironment::currentDateTimeUtc();
    event.vehicleId = monitor->vehicleId();
    event.position = monitor->position();
    event.kind = AlertEvent::Awareness;
    event.distance = qQNaN();
    event.zoneIdentifier = GeoZoneAwareness::stateCode(change.to);
    event.zoneName = change.status.reasons().join(QStringLiteral("; "));
    event.message = _labelled(monitor->vehicleId(), alertMessage(event, event.zoneName, QString()));
    qCDebug(GeoZoneMonitorLog) << event.message;
    const int severity = GeoZoneIntendedLevel::severity(static_cast<GeoZoneIntendedLevel::Level>(level));
    _alertSubjects.insert(event.id, AlertSubject{ monitor->key(), event, false });
    (void) _alertSubjects.remove(event.id - maxActionAlerts);
    emit alertEvent(event);
    emit alert(event.message, severity, event.id);
    _show(event.id, event.message, severity);

    GeoZoneAlertArbiter::Alert announced;
    announced.id = event.id;
    announced.vehicleKey = monitor->key();
    announced.zoneKey = QStringLiteral("geo-awareness");
    announced.severity = severity;
    announced.inside = (level != GeoZoneIntendedLevel::Advisory);
    announced.message = event.message;
    _arbiter.submit(announced);
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
        _show(repeated.id, repeated.repeatMessage, repeated.severity, repeated.level);
    }
    _updateMuted();
    _updateAlertActions();
    _updateActiveLevel();
}

void GeoZoneMonitor::_show(int alertId, const QString &message, int severity, GeoZoneAlertLevel::Level level)
{
    GeoZoneAlertModel::Alert alert;
    alert.alertId = alertId;
    alert.message = message;
    alert.severity = severity;
    alert.level = level;
    const auto subject = _alertSubjects.constFind(alertId);
    if (subject != _alertSubjects.cend()) {
        alert.vehicleKey = subject->vehicleKey;
        alert.zoneKey = subject->event.zoneKey;
        alert.asksPermission = subject->asksPermission;
    }
    _setAlertActions(alert);
    if (_alertModel->show(alert)) {
        _logPilotEvent(alertId, AlertEvent::Notified, alert.question ? QString::fromLatin1(permissionQuestion) : QString());
    }
}

void GeoZoneMonitor::_logPilotEvent(int alertId, AlertEvent::Kind kind, const QString &detail)
{
    // Fence breaches aren't geozone alerts
    const auto subject = _alertSubjects.constFind(alertId);
    if (subject == _alertSubjects.cend()) {
        return;
    }
    AlertEvent event = subject->event;
    event.time = GeoZoneEnvironment::currentDateTimeUtc();
    event.kind = kind;
    event.detail = detail;
    event.timeToEntry = qQNaN();
    // The thresholds belong to the alert
    event.warningDistance = qQNaN();
    event.warningVerticalDistance = qQNaN();
    event.warningTime = qQNaN();
    event.groundSpeed = qQNaN();
    event.verticalSpeed = qQNaN();
    if (const GeoZoneVehicleMonitor *const monitor = (subject->vehicleKey > 0) ? _monitorByKey(subject->vehicleKey) : nullptr) {
        event.position = monitor->position();
        const QList<GeoZoneAlertTracker::Nearby> &nearby = monitor->nearby();
        const auto zone = std::find_if(nearby.cbegin(), nearby.cend(), [&event](const GeoZoneAlertTracker::Nearby &candidate) {
            return candidate.zone->key() == event.zoneKey;
        });
        event.inside = (zone != nearby.cend()) && zone->inside;
        event.distance = (zone != nearby.cend()) ? zone->distance : qQNaN();
    }
    event.message = alertMessage(event, event.zoneName.isEmpty() ? event.zoneIdentifier : event.zoneName, GeoZoneText::restriction(event.restriction));
    _alertHistory->logPilotEvent(event);
}

void GeoZoneMonitor::_setAlertActions(GeoZoneAlertModel::Alert &alert) const
{
    const auto subject = _alertSubjects.constFind(alert.alertId);
    const GeoZoneVehicleMonitor *const monitor = ((subject != _alertSubjects.cend()) && (subject->vehicleKey > 0)) ? _monitorByKey(subject->vehicleKey) : nullptr;
    if (!monitor) {
        alert.question = false;
        alert.activeVehicle = false;
        alert.inside = false;
        alert.climbHeight = qQNaN();
        return;
    }
    const QList<GeoZoneAlertTracker::Nearby> &nearby = monitor->nearby();
    const auto zone = std::find_if(nearby.cbegin(), nearby.cend(), [&alert](const GeoZoneAlertTracker::Nearby &candidate) {
        return candidate.zone->key() == alert.zoneKey;
    });
    const bool near = (zone != nearby.cend());
    const bool known = near && !monitor->positionUnavailable();
    // Once the vehicle left the zone and its approach range, the permission no longer matters
    alert.question = subject->asksPermission && near && !_arbiter.zoneApproved(subject->vehicleKey, alert.zoneKey) && !_permits->contains(subject->permitKey);
    alert.activeVehicle = (monitor == _activeMonitor);
    alert.inside = known && zone->inside;
    alert.climbHeight = known ? _climbHeight(monitor, zone->zone) : qQNaN();
}

void GeoZoneMonitor::_updateAlertActions()
{
    _alertModel->updateActions([this](GeoZoneAlertModel::Alert &alert) { _setAlertActions(alert); });
}

void GeoZoneMonitor::acknowledgeAlert(int alertId)
{
    _alertHistory->acknowledgeAlert(alertId);
    _arbiter.acknowledge(alertId);
}

void GeoZoneMonitor::muteAlerts(int seconds)
{
    // Clamped so that the timer's milliseconds fit an int
    const int msecs = (seconds > 0) ? ((std::min)(seconds, maxMuteSecs) * 1000) : GeoZoneAlertArbiter::defaultMuteMSecs;
    for (const GeoZoneAlertModel::Alert &alert : _alertModel->alerts()) {
        _logPilotEvent(alert.alertId, AlertEvent::Muted, QStringLiteral("Silence speech for %1 s").arg(msecs / 1000));
    }
    _arbiter.mute(msecs);
    // Notified after the arbiter's clock passed the end
    _muteTimer.start(msecs + 100);
    _updateMuted();
}

void GeoZoneMonitor::unmuteAlerts()
{
    _arbiter.unmute();
    _muteTimer.stop();
    _updateMuted();
}

bool GeoZoneMonitor::asksPermission(GeoZone::Restriction restriction)
{
    return GeoZonePolicy::isRestricted(restriction);
}

GeoZoneAlertLevel::Level GeoZoneMonitor::zoneLevel(GeoZone::Restriction restriction, bool inside, bool imminent, bool approved)
{
    if (approved || (restriction == GeoZone::Conditional) || (restriction == GeoZone::NoRestriction)) {
        return GeoZoneAlertLevel::Advisory;
    }
    if (restriction == GeoZone::USpace) {
        return GeoZoneAlertLevel::Caution;
    }
    return (inside || imminent) ? GeoZoneAlertLevel::Warning : GeoZoneAlertLevel::Caution;
}

GeoZoneAlertLevel::Level GeoZoneMonitor::alertLevel(const AlertEvent &event, bool imminent)
{
    switch (event.kind) {
    case AlertEvent::Approach:
    case AlertEvent::Entry:
    case AlertEvent::Activated:
        return zoneLevel(event.restriction, event.inside, imminent);
    case AlertEvent::Traffic:
        return GeoZoneAlertLevel::Advisory;
    case AlertEvent::PositionLost:
    case AlertEvent::NonConformance:
        return GeoZoneAlertLevel::Caution;
    default:
        return event.level;
    }
}

void GeoZoneMonitor::_updateActiveLevel()
{
    int level = -1;
    int predictionLevel = -1;
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor;
    if (monitor && (_manager->zoneCount() > 0)) {
        const auto imminent = [this, monitor](double timeToEntry) {
            const double imminentTime = settings().limits ? settings().limits->imminentTime : monitor->thresholds().result().limits().imminentTime;
            return (timeToEntry >= 0) && (timeToEntry <= imminentTime);
        };
        const QSet<QString> approved = _arbiter.approvedZones(monitor->key());
        for (const GeoZoneAlertTracker::Nearby &nearby : monitor->nearby()) {
            const int zoneLevel = GeoZoneMonitor::zoneLevel(nearby.zone->restriction(), nearby.inside, imminent(nearby.timeToEntry), approved.contains(nearby.zone->key()));
            level = (level < 0) ? zoneLevel : (std::min)(level, zoneLevel);
        }
        if (const std::optional<GeoZoneAlertTracker::Entry> entry = monitor->entry()) {
            predictionLevel = zoneLevel(entry->zone->restriction(), false, imminent(entry->time), approved.contains(entry->zone->key()));
        }
        if (monitor->fenceBreached()) {
            level = GeoZoneAlertLevel::Warning;
        }
    }
    if ((level != _activeLevel) || (predictionLevel != _predictionLevel)) {
        _activeLevel = level;
        _predictionLevel = predictionLevel;
        emit activeLevelChanged();
    }
}

void GeoZoneMonitor::approveAlert(int alertId)
{
    _logPilotEvent(alertId, AlertEvent::Approved, QStringLiteral("I do"));
    const auto subject = _alertSubjects.constFind(alertId);
    if ((subject == _alertSubjects.cend()) || (subject->vehicleKey <= 0) || subject->event.zoneKey.isEmpty() || !_monitorByKey(subject->vehicleKey)) {
        _dismiss(alertId);
        return;
    }
    const int vehicleKey = subject->vehicleKey;
    // The zones of the alert, with those of the alerts it replaced, like the same zone from another source
    QStringList zoneKeys = { subject->event.zoneKey };
    for (const GeoZoneAlertModel::Alert &alert : _alertModel->alerts()) {
        if (alert.alertId == alertId) {
            zoneKeys = alert.zoneKeys;
        }
    }
    // All the shown alerts about the zones are answered
    const auto aboutZones = [vehicleKey, &zoneKeys](const GeoZoneAlertModel::Alert &alert) {
        return (alert.vehicleKey == vehicleKey) && std::any_of(alert.zoneKeys.cbegin(), alert.zoneKeys.cend(), [&zoneKeys](const QString &zoneKey) {
            return zoneKeys.contains(zoneKey);
        });
    };
    // ... and so are the alerts of the vehicle about them, also those replaced
    acknowledgeAlert(alertId);
    for (auto it = _alertSubjects.cbegin(); it != _alertSubjects.cend(); ++it) {
        if ((it->vehicleKey == vehicleKey) && zoneKeys.contains(it->event.zoneKey)) {
            acknowledgeAlert(it.key());
        }
    }
    const int approvalCount = _arbiter.approvalCount();
    const GeoZoneVehicleMonitor *const monitor = _monitorByKey(vehicleKey);
    for (const QString &zoneKey : std::as_const(zoneKeys)) {
        // Zones requiring an authorisation or a notification get a permit for the flight rather than until the vehicle leaves them
        const QList<GeoZoneAlertTracker::Nearby> &nearby = monitor->nearby();
        const auto zone = std::find_if(nearby.cbegin(), nearby.cend(), [&zoneKey](const GeoZoneAlertTracker::Nearby &candidate) { return candidate.zone->key() == zoneKey; });
        if ((zone != nearby.cend()) && GeoZonePermits::applies(zone->zone)) {
            (void) _permits->acknowledge(zone->zone, GeoZonePermits::AlertPopup, monitor->vehicleId());
        } else {
            _arbiter.approveZone(vehicleKey, zoneKey);
        }
    }
    _approvalsChanged(approvalCount);
    _alertModel->removeIf([alertId, &aboutZones](const GeoZoneAlertModel::Alert &alert) { return (alert.alertId == alertId) || aboutZones(alert); });
}

void GeoZoneMonitor::dismissAlert(int alertId)
{
    _logPilotEvent(alertId, AlertEvent::Dismissed, QStringLiteral("Dismiss"));
    _dismiss(alertId);
}

void GeoZoneMonitor::_dismiss(int alertId)
{
    acknowledgeAlert(alertId);
    // With the alerts it replaced, like the same alert about the zone from another source
    const auto subject = _alertSubjects.constFind(alertId);
    const QList<GeoZoneAlertModel::Alert> &shown = _alertModel->alerts();
    const auto row = std::find_if(shown.cbegin(), shown.cend(), [alertId](const GeoZoneAlertModel::Alert &alert) { return alert.alertId == alertId; });
    if ((subject != _alertSubjects.cend()) && (row != shown.cend())) {
        for (auto it = _alertSubjects.cbegin(); it != _alertSubjects.cend(); ++it) {
            if ((it->vehicleKey == subject->vehicleKey) && row->zoneKeys.contains(it->event.zoneKey) &&
                (it->event.kind == subject->event.kind) && (it->event.traffic == subject->event.traffic)) {
                acknowledgeAlert(it.key());
            }
        }
    }
    _alertModel->removeIf([alertId](const GeoZoneAlertModel::Alert &alert) { return alert.alertId == alertId; });
}

void GeoZoneMonitor::closeAlerts()
{
    for (const GeoZoneAlertModel::Alert &alert : _alertModel->alerts()) {
        _logPilotEvent(alert.alertId, AlertEvent::Dismissed, QStringLiteral("Close"));
    }
    _alertModel->clear();
}

void GeoZoneMonitor::_logPermit(const GeoZonePermits::Permit &permit, AlertEvent::Kind kind, const QString &detail)
{
    AlertEvent event;
    event.time = GeoZoneEnvironment::currentDateTimeUtc();
    event.vehicleId = permit.vehicleId;
    const auto monitor = std::find_if(_monitors.cbegin(), _monitors.cend(), [&permit](const GeoZoneVehicleMonitor *candidate) {
        return (candidate->vehicleId() == permit.vehicleId) && (permit.vehicleId != noVehicle);
    });
    if (monitor != _monitors.cend()) {
        event.position = (*monitor)->position();
    }
    event.kind = kind;
    event.distance = qQNaN();
    event.zoneIdentifier = permit.zoneIdentifier;
    event.zoneName = permit.zoneName;
    event.restriction = permit.restriction;
    event.sourceText = permit.sourceText;
    event.zoneKey = permit.zoneKey;
    event.detail = detail;
    event.message = alertMessage(event, permit.zoneName, GeoZoneText::restriction(permit.restriction));
    qCDebug(GeoZoneMonitorLog) << event.message << detail;
    _alertHistory->logPilotEvent(event);
}

void GeoZoneMonitor::_permitsChanged(const QString &withdrawnKey)
{
    for (GeoZoneVehicleMonitor *const monitor : std::as_const(_monitors)) {
        // Alerted again where the vehicle is in or near the zone
        if (!withdrawnKey.isEmpty()) {
            QSet<QString> zoneKeys;
            for (const GeoZoneAlertTracker::Nearby &nearby : monitor->nearby()) {
                if (GeoZonePermits::key(nearby.zone) == withdrawnKey) {
                    zoneKeys.insert(nearby.zone->key());
                }
            }
            monitor->forgetStates(zoneKeys);
        }
        _updateGroundStatus(monitor);
    }
    _updateAlertActions();
}

QStringList GeoZoneMonitor::approvedZoneKeys() const
{
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor;
    return monitor ? QStringList(_arbiter.approvedZones(monitor->key()).values()) : QStringList();
}

void GeoZoneMonitor::_approvalsChanged(int previousCount)
{
    if (_arbiter.approvalCount() != previousCount) {
        for (GeoZoneVehicleMonitor *const monitor : std::as_const(_monitors)) {
            _updateGroundStatus(monitor);
        }
        emit approvalsChanged();
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
        const double upper = GeoZoneVertical::limitAMSL(volumes[i], true, position);
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
    return vehicles && (_manager->zoneCount() > 0);
}

void GeoZoneMonitor::_forgetZones(GeoZoneSource *source)
{
    // Zones of the source are about to be deleted. Their alert states are kept so reloading the source doesn't repeat alerts.
    _policy.reset();
    for (GeoZoneVehicleMonitor *const monitor : std::as_const(_monitors)) {
        monitor->forgetZones(source->key());
    }
    QList<ZoneTraffic> traffic = _traffic;
    if (traffic.removeIf([source](const ZoneTraffic &zoneTraffic) { return zoneTraffic.nearby.zone->sourceKey() == source->key(); }) > 0) {
        _setTraffic(traffic);
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

QString GeoZoneMonitor::alertMessage(const AlertEvent &event, const QString &zoneName, const QString &restrictionText)
{
    // Alerts about a zone the pilot acknowledged a permit for, see GeoZonePermits
    const bool permitted = (event.detail == QStringLiteral("permit"));
    switch (event.kind) {
    case AlertEvent::Entry:
        return permitted ? tr("Entered geozone %1 (%2) \u2014 permit acknowledged").arg(zoneName, restrictionText) : tr("Entered geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::OnGround: {
        if (permitted) {
            return tr("On the ground inside geozone %1 (%2) \u2014 permit acknowledged").arg(zoneName, restrictionText);
        }
        const bool arming = (event.detail == QStringLiteral("arming"));
        switch (event.restriction) {
        case GeoZone::Prohibited:
            return arming ? tr("Arming inside prohibited geozone %1 — take-off not allowed here").arg(zoneName) :
                            tr("On the ground inside prohibited geozone %1 — take-off not allowed here").arg(zoneName);
        case GeoZone::ReqAuthorisation:
            return arming ? tr("Arming inside geozone %1 (%2) — take-off only with an authorisation").arg(zoneName, restrictionText) :
                            tr("On the ground inside geozone %1 (%2) — take-off only with an authorisation").arg(zoneName, restrictionText);
        case GeoZone::Conditional:
        case GeoZone::Unknown:
            return arming ? tr("Arming inside geozone %1 (%2) — check its conditions before take-off").arg(zoneName, restrictionText) :
                            tr("On the ground inside geozone %1 (%2) — check its conditions before take-off").arg(zoneName, restrictionText);
        default:
            break;
        }
        return arming ? tr("Arming inside geozone %1 (%2)").arg(zoneName, restrictionText) : tr("On the ground inside geozone %1 (%2)").arg(zoneName, restrictionText);
    }
    case AlertEvent::PositionLost:
        return event.inside ? tr("Position lost inside geozone %1 (%2)").arg(zoneName, restrictionText) :
                              tr("Position lost near geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Traffic:
        return tr("Traffic %1 entered geozone %2 (%3)").arg(event.traffic, zoneName, restrictionText);
    case AlertEvent::NonConformance:
        return zoneName.isEmpty() ? tr("Outside planned volume") : tr("Outside planned volume: %1").arg(zoneName);
    case AlertEvent::Awareness:
        return GeoZoneAwareness::alertMessage(GeoZoneAwareness::stateFromCode(event.zoneIdentifier), event.zoneName);
    case AlertEvent::FenceWarning:
        return tr("Geofence ahead: %1").arg(zoneName);
    case AlertEvent::FenceBreach:
        return zoneName;
    case AlertEvent::Activated:
        return tr("Geozone %1 (%2) is now active \u2014 you are inside it. Leave or land as soon as possible.").arg(zoneName, restrictionText);
    case AlertEvent::Notified:
        return tr("Pilot notified about geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Approved:
        return tr("Pilot confirmed the permission to fly in geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Dismissed:
        return tr("Pilot dismissed the alert about geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Muted:
        return tr("Pilot muted the alerts about geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Permit:
        return tr("Pilot acknowledged the permit to enter geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::PermitEnded:
        return tr("Permit to enter geozone %1 (%2) ended").arg(zoneName, restrictionText);
    case AlertEvent::Approach:
        break;
    }
    if (permitted) {
        return tr("Approaching geozone %1 (%2) \u2014 permit acknowledged").arg(zoneName, restrictionText);
    }
    if (!std::isnan(event.timeToEntry)) {
        return tr("Approaching geozone %1 (%2), entry in about %3 s").arg(zoneName, restrictionText).arg(std::lround(event.timeToEntry));
    }
    return tr("Approaching geozone %1 (%2)").arg(zoneName, restrictionText);
}

GeoZoneMonitor::AlertEvent GeoZoneMonitor::raiseVehicleAlert(AlertEvent::Kind kind, int vehicleId, const GeoZone::Position &position, double distance,
                                                              const QString &identifier, const QString &name, GeoZoneAlertLevel::Level level)
{
    AlertEvent event;
    event.id = _nextAlertId++;
    event.time = GeoZoneEnvironment::currentDateTimeUtc();
    event.vehicleId = vehicleId;
    event.position = position;
    event.kind = kind;
    event.distance = distance;
    event.zoneIdentifier = identifier;
    event.zoneName = name;
    event.level = level;
    event.message = _labelled(vehicleId, alertMessage(event, name, QString()));
    qCDebug(GeoZoneMonitorLog) << event.message;
    const int severity = GeoZoneAlertLevel::severity(level, GeoZone::restrictionSeverity(GeoZone::ReqAuthorisation));
    emit alertEvent(event);
    emit alert(event.message, severity, event.id);

    const auto monitor = std::find_if(_monitors.cbegin(), _monitors.cend(), [vehicleId](const GeoZoneVehicleMonitor *candidate) { return candidate->vehicleId() == vehicleId; });
    GeoZoneAlertArbiter::Alert announced;
    announced.id = event.id;
    announced.vehicleKey = (monitor != _monitors.cend()) ? (*monitor)->key() : 0;
    announced.zoneKey = identifier;
    announced.severity = severity;
    announced.level = level;
    announced.inside = true;
    announced.message = event.message;
    _arbiter.submit(announced);
    _announce();
    return event;
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
    event.message = _labelled(vehicleId, alertMessage(event, zone->displayName(), GeoZoneText::restriction(zone->restriction())));
    qCDebug(GeoZoneMonitorLog) << event.message << "distance" << distance << "time to entry" << timeToEntry;
    return event;
}
