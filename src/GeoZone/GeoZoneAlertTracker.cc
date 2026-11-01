/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAlertTracker.h"
#include "GeoZoneCatalog.h"
#include "GeoZonePolicy.h"
#include "GeoZoneTerrain.h"

#include <QtCore/QSet>
#include <QtCore/QtMath>
#include <QtPositioning/QGeoCircle>

#include <algorithm>
#include <limits>

GeoZoneAlertTracker::Update GeoZoneAlertTracker::update(const GeoZoneCatalog &catalog, const GeoZonePolicy &policy, const GeoZone::Position &position, const Velocity &velocity, const Limits &limits)
{
    Update update;
    if (!position.coordinate.isValid()) {
        return update;
    }

    QGeoRectangle area = QGeoCircle(position.coordinate, limits.distance + hysteresisDistance).boundingGeoRectangle();
    // Zones the vehicle would enter at its velocity within the approach time are near too
    const bool predict = velocity.valid && (limits.time > 0) &&
                         ((velocity.groundSpeed() >= minPredictionSpeed) || (std::abs(velocity.up) >= minPredictionSpeed));
    const double horizon = limits.time + hysteresisTime;
    QList<GeoZone::Position> ahead;
    if (predict) {
        ahead = predictedPath(position, velocity, horizon);
        area = area.united(QGeoRectangle(QList<QGeoCoordinate>{ position.coordinate, ahead.constLast().coordinate }));
    }

    QSet<QString> nearbyZones;
    for (GeoZone *zone : catalog.zonesInArea(area, policy)) {
        const QString &key = zone->key();
        const State previous = _states.value(key, State::Away);
        const double nearLimit = limits.distance + ((previous != State::Away) ? hysteresisDistance : 0);
        // The approach margin includes the inside margin, so a zone which isn't near isn't entered either
        const double nearDistance = zone->distanceTo(position, limits.verticalDistance + ((previous != State::Away) ? hysteresisDistance : 0), nearLimit);
        const bool nearByDistance = (nearDistance <= nearLimit);
        double insideDistance = qInf();
        if (nearByDistance) {
            insideDistance = zone->distanceTo(position, (previous == State::Inside) ? hysteresisDistance : 0, hysteresisDistance);
        }
        const bool inside = (insideDistance <= 0) || ((previous == State::Inside) && (insideDistance <= hysteresisDistance));

        double entryTime = qQNaN();
        if (!inside && predict) {
            entryTime = timeToEntry(zone, ahead, horizon);
        }
        const double timeLimit = limits.time + ((previous != State::Away) ? hysteresisTime : 0);
        const bool nearByTime = !std::isnan(entryTime) && (entryTime <= timeLimit);
        if (!inside && !nearByDistance && !nearByTime) {
            continue;
        }

        const State state = inside ? State::Inside : State::Near;
        // Zones only near in time can be far, or above or below the vehicle
        const double distance = nearByDistance ? (std::min)(insideDistance, nearDistance) : zone->distanceTo(position, std::numeric_limits<double>::max());

        nearbyZones.insert(key);
        _states[key] = state;
        update.nearby.append(Nearby{ zone, inside, static_cast<int>(std::lround(distance)), std::isnan(entryTime) ? -1 : static_cast<int>(std::lround(entryTime)) });
        if (state > previous) {
            update.alerts.append(Alert{ zone, inside, inside ? 0 : distance, entryTime });
        }
    }

    // Zones out of range or no longer active can alert again
    for (auto it = _states.begin(); it != _states.end();) {
        it = nearbyZones.contains(it.key()) ? std::next(it) : _states.erase(it);
    }

    std::stable_sort(update.alerts.begin(), update.alerts.end(), [](const Alert &a, const Alert &b) {
        const int severityA = GeoZone::restrictionSeverity(a.zone->restriction());
        const int severityB = GeoZone::restrictionSeverity(b.zone->restriction());
        return (severityA != severityB) ? (severityA < severityB) : (a.inside && !b.inside);
    });

    // The most restrictive first, so being inside large U-space airspace doesn't hide a prohibited zone ahead
    std::stable_sort(update.nearby.begin(), update.nearby.end(), [](const Nearby &a, const Nearby &b) {
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

    return update;
}

QList<GeoZone::Position> GeoZoneAlertTracker::predictedPath(const GeoZone::Position &position, const Velocity &velocity, double seconds)
{
    GeoZone::Position predicted;
    const double course = qRadiansToDegrees(std::atan2(velocity.east, velocity.north));
    predicted.coordinate = position.coordinate.atDistanceAndAzimuth(velocity.groundSpeed() * seconds, course);
    if (!std::isnan(position.altitudeAMSL)) {
        predicted.altitudeAMSL = position.altitudeAMSL + (velocity.up * seconds);
    }
    return GeoZoneTerrain::withTerrain({ position, predicted });
}

double GeoZoneAlertTracker::timeToEntry(const GeoZone *zone, const QList<GeoZone::Position> &path, double horizon)
{
    const auto entry = zone->pathEntry(path);
    if (!entry) {
        return qQNaN();
    }
    // The vehicle moves along the path at a constant velocity, so the time is proportional to the distance
    double total = 0;
    double toEntry = 0;
    for (qsizetype i = 1; i < path.count(); i++) {
        const double length = path[i - 1].coordinate.distanceTo(path[i].coordinate);
        if ((i - 1) < entry->segment) {
            toEntry += length;
        } else if ((i - 1) == entry->segment) {
            toEntry += length * entry->fraction;
        }
        total += length;
    }
    // Climbing or descending in place
    if (total < 0.01) {
        return entry->fraction * horizon;
    }
    return (toEntry / total) * horizon;
}

GeoZoneAlertTracker::Velocity GeoZoneVelocityEstimator::add(qint64 msecs, const GeoZone::Position &position)
{
    _samples.removeIf([msecs](const Sample &sample) { return (msecs - sample.msecs) > windowMSecs; });
    _samples.append(Sample{ msecs, position });

    GeoZoneAlertTracker::Velocity velocity;
    const Sample &oldest = _samples.constFirst();
    const double seconds = static_cast<double>(msecs - oldest.msecs) / 1000.0;
    if ((seconds < 0.5) || !oldest.position.coordinate.isValid()) {
        return velocity;
    }
    const double distance = oldest.position.coordinate.distanceTo(position.coordinate);
    const double azimuth = qDegreesToRadians(oldest.position.coordinate.azimuthTo(position.coordinate));
    velocity.north = distance * std::cos(azimuth) / seconds;
    velocity.east = distance * std::sin(azimuth) / seconds;
    if (!std::isnan(oldest.position.altitudeAMSL) && !std::isnan(position.altitudeAMSL)) {
        velocity.up = (position.altitudeAMSL - oldest.position.altitudeAMSL) / seconds;
    }
    velocity.valid = true;
    return velocity;
}

QList<GeoZoneVerticalBand> GeoZoneVerticalBand::at(const GeoZoneCatalog &catalog, const GeoZonePolicy &policy, const GeoZone::Position &position)
{
    QList<GeoZoneVerticalBand> bands;
    for (GeoZone *zone : catalog.zonesAt(position.coordinate)) {
        if (!policy.accepts(zone)) {
            continue;
        }
        for (const GeoZone::Volume &volume : zone->data().volumes) {
            if (GeoZone::volumeContains(volume, position.coordinate)) {
                bands.append(GeoZoneVerticalBand{ zone, GeoZone::limitAMSL(volume, false, position), GeoZone::limitAMSL(volume, true, position) });
            }
        }
    }
    return bands;
}
