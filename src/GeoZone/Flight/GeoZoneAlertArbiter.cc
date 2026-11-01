/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAlertArbiter.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QStringList>

#include <algorithm>
#include <memory>

namespace {

GeoZoneAlertArbiter::Clock elapsedClock()
{
    const auto timer = std::make_shared<QElapsedTimer>();
    timer->start();
    return [timer]() { return timer->elapsed(); };
}

} // namespace

GeoZoneAlertArbiter::GeoZoneAlertArbiter(const Clock &clock)
    : _clock(clock ? clock : elapsedClock())
{
}

void GeoZoneAlertArbiter::setClock(const Clock &clock)
{
    _clock = clock ? clock : elapsedClock();
}

void GeoZoneAlertArbiter::submit(const Alert &alert)
{
    if (zoneMuted(alert.vehicleKey, alert.zoneKey) ||
        std::any_of(_pending.cbegin(), _pending.cend(), [&alert](const Alert &pending) { return pending.id == alert.id; })) {
        return;
    }
    _pending.append(alert);
    if (alert.breach) {
        (void) _breaches.removeIf([&alert](const Breach &breach) {
            return (breach.alert.vehicleKey == alert.vehicleKey) && (breach.alert.zoneKey == alert.zoneKey);
        });
        _breaches.append(Breach{ alert, -1 });
    }
}

void GeoZoneAlertArbiter::setInsideZones(int vehicleKey, const QSet<QString> &zoneKeys)
{
    (void) _breaches.removeIf([vehicleKey, &zoneKeys](const Breach &breach) {
        return (breach.alert.vehicleKey == vehicleKey) && !zoneKeys.contains(breach.alert.zoneKey);
    });
    const auto mutes = _zoneMutes.find(vehicleKey);
    if (mutes != _zoneMutes.end()) {
        mutes->intersect(zoneKeys);
        if (mutes->isEmpty()) {
            _zoneMutes.erase(mutes);
        }
    }
}

void GeoZoneAlertArbiter::muteZone(int vehicleKey, const QString &zoneKey)
{
    _zoneMutes[vehicleKey].insert(zoneKey);
    const auto matches = [vehicleKey, &zoneKey](const Alert &alert) { return (alert.vehicleKey == vehicleKey) && (alert.zoneKey == zoneKey); };
    (void) _pending.removeIf(matches);
    (void) _breaches.removeIf([&matches](const Breach &breach) { return matches(breach.alert); });
}

int GeoZoneAlertArbiter::zoneMuteCount() const
{
    int count = 0;
    for (const QSet<QString> &zoneKeys : _zoneMutes) {
        count += static_cast<int>(zoneKeys.count());
    }
    return count;
}

void GeoZoneAlertArbiter::setSuspended(int vehicleKey, bool suspended)
{
    if (suspended) {
        _suspended.insert(vehicleKey);
    } else {
        (void) _suspended.remove(vehicleKey);
    }
}

void GeoZoneAlertArbiter::removeVehicle(int vehicleKey)
{
    (void) _suspended.remove(vehicleKey);
    (void) _zoneMutes.remove(vehicleKey);
    (void) _pending.removeIf([vehicleKey](const Alert &alert) { return alert.vehicleKey == vehicleKey; });
    (void) _breaches.removeIf([vehicleKey](const Breach &breach) { return breach.alert.vehicleKey == vehicleKey; });
}

void GeoZoneAlertArbiter::clear()
{
    _pending.clear();
    _breaches.clear();
}

void GeoZoneAlertArbiter::acknowledge(int alertId)
{
    (void) _pending.removeIf([alertId](const Alert &alert) { return alert.id == alertId; });
    (void) _breaches.removeIf([alertId](const Breach &breach) { return breach.alert.id == alertId; });
}

void GeoZoneAlertArbiter::acknowledgeAll()
{
    clear();
}

void GeoZoneAlertArbiter::mute(qint64 msecs)
{
    _mutedUntilMSecs = _now() + msecs;
}

int GeoZoneAlertArbiter::_urgency(const Candidate &candidate)
{
    enum { Entry, Repeat, Approach, Count };
    const int kind = candidate.repeat ? Repeat : (candidate.alert.inside ? Entry : Approach);
    return (candidate.alert.severity * Count) + kind;
}

GeoZoneAlertArbiter::Announcement GeoZoneAlertArbiter::tick()
{
    Announcement announcement;
    const qint64 now = _now();
    const bool silenced = muted();
    if (silenced) {
        (void) _pending.removeIf([](const Alert &alert) { return !alert.breach; });
    }
    QList<Candidate> candidates;
    for (const Alert &alert : std::as_const(_pending)) {
        candidates.append(Candidate{ alert, false });
    }
    if (!silenced) {
        for (const Breach &breach : std::as_const(_breaches)) {
            if ((breach.announcedMSecs >= 0) && ((now - breach.announcedMSecs) >= repeatIntervalMSecs) && !_suspended.contains(breach.alert.vehicleKey)) {
                candidates.append(Candidate{ breach.alert, true });
            }
        }
    }
    if (candidates.isEmpty()) {
        return announcement;
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return _urgency(a) < _urgency(b); });
    const int urgency = _urgency(candidates.constFirst());
    const bool allowed = (_lastSpokenMSecs < 0) || ((now - _lastSpokenMSecs) >= minIntervalMSecs) || (urgency < _lastSpokenUrgency);
    if (!allowed) {
        return announcement;
    }

    QStringList parts;
    for (const Candidate &candidate : std::as_const(candidates)) {
        const Alert &alert = candidate.alert;
        if (parts.count() < maxSpokenAlerts) {
            parts.append((candidate.repeat && !alert.repeatMessage.isEmpty()) ? alert.repeatMessage : alert.message);
        }
        announcement.alerts.append(alert);
        if (candidate.repeat) {
            announcement.repeated.append(alert);
        }
        if (alert.breach) {
            for (Breach &breach : _breaches) {
                if (breach.alert.id == alert.id) {
                    breach.announcedMSecs = now;
                }
            }
        }
    }
    announcement.text = parts.join(QStringLiteral(". "));
    const qsizetype more = candidates.count() - parts.count();
    if (more > 0) {
        announcement.text += tr(", and %n more", nullptr, static_cast<int>(more));
    }

    _pending.clear();
    _lastSpokenMSecs = now;
    _lastSpokenUrgency = urgency;
    return announcement;
}
