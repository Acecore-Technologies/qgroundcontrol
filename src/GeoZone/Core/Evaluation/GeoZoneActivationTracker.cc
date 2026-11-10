/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneActivationTracker.h"
#include "GeoZoneBox.h"
#include "GeoZoneCatalog.h"
#include "GeoZonePolicy.h"

#include <QtCore/QSet>
#include <QtPositioning/QGeoCircle>

#include <algorithm>

QList<GeoZoneActivationTracker::Alert> GeoZoneActivationTracker::update(const GeoZoneCatalog &catalog, const GeoZonePolicy &policy, const GeoZone::Position &position,
                                                                        const GeoZoneAlertTracker::Limits &limits, const QDateTime &now, int cautionSecs)
{
    QList<Alert> alerts;
    if (!position.coordinate.isValid() || !now.isValid() || (cautionSecs <= 0)) {
        return alerts;
    }

    QSet<QString> nearbyZones;
    const GeoZoneBox area(QGeoCircle(position.coordinate, limits.distance).boundingGeoRectangle());
    for (const GeoZone *zone : catalog.zonesInArea(area, policy)) {
        if (zone->active() || policy.authorisedAt(zone, position)) {
            continue;
        }
        const double distance = zone->distanceTo(position, limits.verticalDistance, limits.distance);
        if (distance > limits.distance) {
            continue;
        }
        const QString &key = zone->key();
        nearbyZones.insert(key);

        // The activation is looked for again when the zone changed, the caution time changed or the last search is old
        Pending &pending = _pending[key];
        const bool stale = (pending.zone != zone) || (pending.cautionSecs != cautionSecs) || !pending.checked.isValid() ||
                           (pending.activation.isValid() ? (pending.activation <= now) : (pending.checked.secsTo(now) >= recheckSecs));
        if (stale) {
            pending = Pending{ zone, now, nextActivation(zone, now, cautionSecs), cautionSecs };
        }
        if (!pending.activation.isValid() || (now.secsTo(pending.activation) > cautionSecs) || (_alerted.value(key) == pending.activation)) {
            continue;
        }
        _alerted.insert(key, pending.activation);
        const bool inside = zone->contains(position);
        alerts.append(Alert{ zone, inside, inside ? 0 : distance, pending.activation });
    }

    // Zones left behind are looked for again when the vehicle comes back, their activations alerted stay alerted
    // until they pass
    for (auto it = _pending.begin(); it != _pending.end();) {
        it = nearbyZones.contains(it.key()) ? std::next(it) : _pending.erase(it);
    }
    for (auto it = _alerted.begin(); it != _alerted.end();) {
        it = (it.value() < now) ? _alerted.erase(it) : std::next(it);
    }

    std::stable_sort(alerts.begin(), alerts.end(), [](const Alert &a, const Alert &b) { return a.activation < b.activation; });
    return alerts;
}

void GeoZoneActivationTracker::reset()
{
    _pending.clear();
    _alerted.clear();
}

QDateTime GeoZoneActivationTracker::nextActivation(const GeoZone *zone, const QDateTime &now, int secs)
{
    if (!zone || zone->isActiveAt(now) || !zone->isActiveDuring(now, now.addSecs(secs))) {
        return QDateTime();
    }
    // Active at some time within the seconds: the first second it is, active during it from now on
    qint64 inactive = 0;
    qint64 active = secs;
    while ((active - inactive) > 1) {
        const qint64 middle = (inactive + active) / 2;
        if (zone->isActiveDuring(now, now.addSecs(middle))) {
            active = middle;
        } else {
            inactive = middle;
        }
    }
    return now.addSecs(active).toUTC();
}
