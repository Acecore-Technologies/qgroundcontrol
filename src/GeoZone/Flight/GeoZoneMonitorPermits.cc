/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// The pilot's permission to fly in zones, of GeoZoneMonitor: the approvals given by answering an alert and the permits
// for the flight, see GeoZonePermits

#include "GeoZoneMonitor.h"
#include "GeoZoneAlertHistory.h"
#include "GeoZoneEnvironment.h"
#include "GeoZonePermits.h"
#include "GeoZonePolicy.h"
#include "GeoZoneText.h"

#include <algorithm>
#include <utility>

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

void GeoZoneMonitor::_endFlightPermissions(GeoZoneVehicleMonitor *monitor)
{
    _permits->endFlight(monitor->vehicleId(), GeoZonePermits::EndReason::FlightEnded);
    // The pilot confirmed the permission for that flight: a vehicle which landed in the zone is warned on the
    // ground and when it arms again, like before its first flight
    const int approvalCount = _arbiter.approvalCount();
    _arbiter.endApprovals(monitor->key());
    _approvalsChanged(approvalCount);
    _updateAlertActions();
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

void GeoZoneMonitor::_permitsChanged(const GeoZonePermits::Permit &permit, bool withdrawn)
{
    for (GeoZoneVehicleMonitor *const monitor : std::as_const(_monitors)) {
        // Its vehicle is alerted again where it is in or near the zone
        if (withdrawn && ((permit.vehicleId < 0) || (monitor->vehicleId() == permit.vehicleId))) {
            QSet<QString> zoneKeys;
            for (const GeoZoneAlertTracker::Nearby &nearby : monitor->nearby()) {
                if (GeoZonePermits::key(nearby.zone) == permit.key) {
                    zoneKeys.insert(nearby.zone->key());
                }
            }
            monitor->forgetStates(zoneKeys, true);
        }
        _updateGroundStatus(monitor);
    }
    _updateAlertActions();
    // A permit makes its zone an advisory, and its end a caution or warning again, before the next evaluation
    _updateActiveLevel();
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
