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
#include "GeoZoneGuidedCheck.h"
#include "GeoZoneInfo.h"
#include "GeoZoneManager.h"
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
    (void) connect(this, &GeoZoneMonitor::fenceBreach, this, [this](const QString &message) { _show(_nextAlertId++, message, -1); });

    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMonitor::_forgetZones);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMonitor::_zonesChanged);

    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    for (Fact *const fact : { settings->alertsEnabled(), settings->audioAlerts(), settings->approachDistance(), settings->approachTime(),
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
    result.alertsEnabled = settings->alertsEnabled()->rawValue().toBool();
    result.audioAlerts = settings->audioAlerts()->rawValue().toBool();
    result.limits.distance = settings->approachDistance()->rawValue().toDouble();
    result.limits.verticalDistance = approachVerticalDistance;
    // Stored before the minimum was raised
    result.limits.time = (std::max)(settings->approachTime()->rawValue().toDouble(), GeoZoneAlertTracker::minWarningTime);
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
    if (settings().alertsEnabled != previous.alertsEnabled) {
        _resetMonitors();
        emit monitoringChanged();
    }
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
        _updateAlertActions();
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
        const GeoZoneAlertTracker::Update update = tracker.update(*context.catalog, context.policy, position, velocity, context.limits);
        for (const GeoZoneAlertTracker::Alert &zoneAlert : update.alerts) {
            if (zoneAlert.inside && settings().alertsEnabled) {
                entries.append(_alertEvent(zoneAlert.zone, AlertEvent::Traffic, true, 0, qQNaN(), position, noVehicle, plane->label()));
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
        const int severity = trafficSeverity + GeoZone::restrictionSeverity(event.restriction);
        emit alert(event.message, severity, event.id);
        _show(event.id, event.message, severity);
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
    // Breaches end when the vehicle leaves the zone, approvals when it leaves its approach range too. Without a
    // current position the zones it is in aren't known, the nearby zones are those of its last position.
    if (!monitor->positionUnavailable()) {
        const int approvalCount = _arbiter.approvalCount();
        _arbiter.setZones(monitor->key(), monitor->insideZoneKeys(), monitor->nearbyZoneKeys());
        _approvalsChanged(approvalCount);
    }
}

void GeoZoneMonitor::_submit(GeoZoneVehicleMonitor *monitor, const AlertEvent &event, const GeoZone *zone)
{
    const int severity = GeoZone::restrictionSeverity(event.restriction);
    const bool asks = (event.kind == AlertEvent::Approach || event.kind == AlertEvent::Entry) && asksPermission(event.restriction);
    _alertSubjects.insert(event.id, AlertSubject{ monitor->key(), event, asks });
    (void) _alertSubjects.remove(event.id - maxActionAlerts);
    emit alertEvent(event);
    // A zone approved until the vehicle leaves it is logged but not shown, the pilot answered already
    if (_arbiter.zoneApproved(monitor->key(), event.zoneKey)) {
        _alertHistory->acknowledgeAlert(event.id);
    } else {
        emit alert(event.message, severity, event.id);
        _show(event.id, event.message, severity);
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
        announced.repeatMessage = _labelled(monitor->vehicleId(), tr("Still inside geozone %1 (%2)").arg(zone->displayName(), GeoZoneText::restriction(zone->restriction())));
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
        _show(repeated.id, repeated.repeatMessage, repeated.severity);
    }
    _updateMuted();
    _updateAlertActions();
}

void GeoZoneMonitor::_show(int alertId, const QString &message, int severity)
{
    GeoZoneAlertModel::Alert alert;
    alert.alertId = alertId;
    alert.message = message;
    alert.severity = severity;
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
    alert.question = subject->asksPermission && near && !_arbiter.zoneApproved(subject->vehicleKey, alert.zoneKey);
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
        _logPilotEvent(alert.alertId, AlertEvent::Muted, QStringLiteral("Mute alerts for %1 s").arg(msecs / 1000));
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
    for (const QString &zoneKey : std::as_const(zoneKeys)) {
        _arbiter.approveZone(vehicleKey, zoneKey);
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

QStringList GeoZoneMonitor::approvedZoneKeys() const
{
    const GeoZoneVehicleMonitor *const monitor = _activeMonitor;
    return monitor ? QStringList(_arbiter.approvedZones(monitor->key()).values()) : QStringList();
}

void GeoZoneMonitor::_approvalsChanged(int previousCount)
{
    if (_arbiter.approvalCount() != previousCount) {
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
    return vehicles && (_manager->zoneCount() > 0) && settings().alertsEnabled;
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
    switch (event.kind) {
    case AlertEvent::Entry:
        return tr("Entered geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::PositionLost:
        return event.inside ? tr("Position lost inside geozone %1 (%2)").arg(zoneName, restrictionText) :
                              tr("Position lost near geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Traffic:
        return tr("Traffic %1 entered geozone %2 (%3)").arg(event.traffic, zoneName, restrictionText);
    case AlertEvent::NonConformance:
        return zoneName.isEmpty() ? tr("Outside planned volume") : tr("Outside planned volume: %1").arg(zoneName);
    case AlertEvent::Notified:
        return tr("Pilot notified about geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Approved:
        return tr("Pilot confirmed the permission to fly in geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Dismissed:
        return tr("Pilot dismissed the alert about geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Muted:
        return tr("Pilot muted the alerts about geozone %1 (%2)").arg(zoneName, restrictionText);
    case AlertEvent::Approach:
        break;
    }
    if (!std::isnan(event.timeToEntry)) {
        return tr("Approaching geozone %1 (%2), entry in about %3 s").arg(zoneName, restrictionText).arg(std::lround(event.timeToEntry));
    }
    return tr("Approaching geozone %1 (%2)").arg(zoneName, restrictionText);
}

GeoZoneMonitor::AlertEvent GeoZoneMonitor::raiseVehicleAlert(AlertEvent::Kind kind, int vehicleId, const GeoZone::Position &position, double distance,
                                                              const QString &identifier, const QString &name)
{
    AlertEvent event;
    if (!settings().alertsEnabled) {
        return event;
    }
    event.id = _nextAlertId++;
    event.time = GeoZoneEnvironment::currentDateTimeUtc();
    event.vehicleId = vehicleId;
    event.position = position;
    event.kind = kind;
    event.distance = distance;
    event.zoneIdentifier = identifier;
    event.zoneName = name;
    event.message = _labelled(vehicleId, alertMessage(event, name, QString()));
    qCDebug(GeoZoneMonitorLog) << event.message;
    const int severity = GeoZone::restrictionSeverity(GeoZone::ReqAuthorisation);
    emit alertEvent(event);
    emit alert(event.message, severity, event.id);

    const auto monitor = std::find_if(_monitors.cbegin(), _monitors.cend(), [vehicleId](const GeoZoneVehicleMonitor *candidate) { return candidate->vehicleId() == vehicleId; });
    GeoZoneAlertArbiter::Alert announced;
    announced.id = event.id;
    announced.vehicleKey = (monitor != _monitors.cend()) ? (*monitor)->key() : 0;
    announced.zoneKey = identifier;
    announced.severity = severity;
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
