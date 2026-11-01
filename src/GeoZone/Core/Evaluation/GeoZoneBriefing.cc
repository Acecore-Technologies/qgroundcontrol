/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneBriefing.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneBox.h"
#include "GeoZoneCatalog.h"
#include "GeoZoneEnvironment.h"
#include "GeoZonePolicy.h"

#include <algorithm>
#include <cmath>

namespace {

using Evaluator = GeoZonePathEvaluator;

/// @return Altitude of the vertex above mean sea level, NaN when unknown
double vertexAltitudeAMSL(const Evaluator::Path &path, const Evaluator::Vertex &vertex)
{
    switch (path.altitudeReference) {
    case Evaluator::AltitudeReference::MeanSeaLevel:
        return vertex.altitude;
    case Evaluator::AltitudeReference::Relative:
        return vertex.altitude + path.referenceAltitudeAMSL;
    case Evaluator::AltitudeReference::Terrain:
        return vertex.altitude + vertex.terrainAMSL;
    }
    return qQNaN();
}

void extend(double &minimum, double &maximum, double value)
{
    if (qIsNaN(value)) {
        return;
    }
    minimum = qIsNaN(minimum) ? value : (std::min)(minimum, value);
    maximum = qIsNaN(maximum) ? value : (std::max)(maximum, value);
}

QList<GeoZone::Restriction> nearbyRestrictions()
{
    return { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::Unknown, GeoZone::USpace };
}

} // namespace

QList<const GeoZoneBriefing::Zone*> GeoZoneBriefing::zonesWith(Involvement involvement) const
{
    QList<const Zone*> result;
    for (const Zone &zone : zones) {
        if (zone.involvement == involvement) {
            result.append(&zone);
        }
    }
    return result;
}

QString GeoZoneBriefing::involvementCode(Involvement involvement)
{
    switch (involvement) {
    case Involvement::Entered:
        return QStringLiteral("entered");
    case Involvement::Contingency:
        return QStringLiteral("contingency");
    case Involvement::Nearby:
        return QStringLiteral("nearby");
    }
    return QString();
}

QString GeoZoneBriefing::restrictionCode(GeoZone::Restriction restriction)
{
    switch (restriction) {
    case GeoZone::Prohibited:
        return QStringLiteral("prohibited");
    case GeoZone::ReqAuthorisation:
        return QStringLiteral("req_authorisation");
    case GeoZone::Conditional:
        return QStringLiteral("conditional");
    case GeoZone::NoRestriction:
        return QStringLiteral("no_restriction");
    case GeoZone::USpace:
        return QStringLiteral("u_space");
    case GeoZone::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

QList<GeoZoneBriefing::Interval> GeoZoneBriefing::activePeriods(const GeoZone *zone, const QDateTime &start, const QDateTime &end)
{
    if (!zone || !start.isValid() || !end.isValid() || (end < start)) {
        return {};
    }
    if (zone->data().applicability.isEmpty()) {
        return { Interval{ start.toUTC(), end.toUTC() } };
    }

    const qint64 span = start.secsTo(end);
    const qint64 step = (std::max)(qint64{ 60 }, (span + maxScheduleSamples - 1) / maxScheduleSamples);
    // The second at which the zone switches between two samples
    const auto transition = [zone](QDateTime before, QDateTime after) {
        const bool activeBefore = zone->isActiveAt(before);
        while (before.secsTo(after) > 1) {
            const QDateTime middle = before.addSecs(before.secsTo(after) / 2);
            if (zone->isActiveAt(middle) == activeBefore) {
                before = middle;
            } else {
                after = middle;
            }
        }
        return after;
    };

    QList<Interval> periods;
    QDateTime previous = start.toUTC();
    bool previousActive = zone->isActiveAt(previous);
    QDateTime periodStart = previousActive ? previous : QDateTime();
    for (qint64 offset = step; ; offset += step) {
        const QDateTime time = (offset < span) ? start.toUTC().addSecs(offset) : end.toUTC();
        const bool active = zone->isActiveAt(time);
        if (active != previousActive) {
            const QDateTime switched = transition(previous, time);
            if (active) {
                periodStart = switched;
            } else {
                periods.append(Interval{ periodStart, switched });
            }
        }
        previous = time;
        previousActive = active;
        if (offset >= span) {
            break;
        }
    }
    if (previousActive) {
        periods.append(Interval{ periodStart, end.toUTC() });
    }
    return periods;
}

GeoZoneBriefing GeoZoneBriefing::build(const GeoZoneCatalog &catalog, const Input &input)
{
    GeoZoneBriefing briefing;
    briefing.mission = input.mission;
    briefing.sources = input.sources;
    const QDateTime now = input.now.isValid() ? input.now.toUTC() : GeoZoneEnvironment::currentDateTimeUtc();
    briefing.generatedAt = now;
    briefing.scheduleTolerance = (std::max)(input.scheduleTolerance, 0);
    briefing.nearbyDistance = (std::max)(input.nearbyDistance, 0.0);

    const QDateTime start = input.start.isValid() ? input.start.toUTC() : QDateTime();
    const QDateTime end = (start.isValid() && input.end.isValid() && (input.end >= start)) ? input.end.toUTC() : start;
    const QDateTime windowStart = start.isValid() ? start.addSecs(-briefing.scheduleTolerance) : QDateTime();
    const QDateTime windowEnd = start.isValid() ? end.addSecs(briefing.scheduleTolerance) : QDateTime();

    GeoZonePathEvaluator evaluator(catalog, GeoZonePolicy::mission(start, end), start);
    evaluator.setScheduleTolerance(briefing.scheduleTolerance);

    const auto makeZone = [&](const GeoZone *zone, Involvement involvement) {
        Zone result;
        result.involvement = involvement;
        result.key = zone->key();
        result.identifier = zone->identifier();
        result.country = zone->country();
        result.name = zone->name();
        result.restriction = zone->restriction();
        result.restrictionCode = zone->data().restrictionCode;
        result.reasons = zone->reasonText();
        result.verticalLimits = zone->verticalLimitsText();
        result.schedule = zone->data().applicability.isEmpty() ? QString() : zone->applicabilityText();
        result.activePeriods = activePeriods(zone, windowStart, windowEnd);
        result.activeDuringFlight = start.isValid() ? !result.activePeriods.isEmpty() : zone->active();
        result.authorities = zone->data().authorities;
        result.message = zone->message();
        result.conditions = zone->restrictionConditions();
        result.otherReasonInfo = zone->data().otherReasonInfo;
        result.uSpaceClass = zone->uSpaceClass();
        result.regulationExemption = zone->regulationExemptionText();
        result.sourceKey = zone->sourceKey();
        result.contentHash = zone->data().contentHash;
        if (input.authorisations) {
            if (const GeoZoneAuthorisation *const authorisation = input.authorisations->find(zone)) {
                Authorisation briefed;
                briefed.permit = authorisation->permit;
                briefed.validFrom = authorisation->validFrom;
                briefed.validTo = authorisation->validTo;
                briefed.status = GeoZoneAuthorisations::statusText(*authorisation, now);
                briefed.coversFlight = start.isValid() ? authorisation->covers(start, end) :
                                                         (!authorisation->expired(now) && (!authorisation->validFrom.isValid() || (authorisation->validFrom <= now)));
                result.authorisation = briefed;
            }
        }
        return result;
    };

    // Meters along the mission at a point of a flight path
    const auto missionDistance = [&input](const Evaluator::PathPoint &point) {
        const QList<double> distances = input.pathDistances.value(point.path);
        if ((point.path < 0) || (point.path >= input.paths.count()) || (distances.count() != input.paths[point.path].vertices.count()) || distances.isEmpty()) {
            return point.distance;
        }
        const qsizetype from = std::clamp<qsizetype>(point.segment, 0, distances.count() - 1);
        const qsizetype to = (std::min)(from + 1, distances.count() - 1);
        return distances[from] + ((distances[to] - distances[from]) * point.fraction);
    };
    const auto itemBefore = [&input](double distance) {
        int sequenceNumber = -1;
        for (const Item &item : input.items) {
            if (qIsNaN(item.distance) || (item.distance > (distance + 0.5))) {
                continue;
            }
            sequenceNumber = item.sequenceNumber;
        }
        return sequenceNumber;
    };

    const Evaluator::Result result = evaluator.evaluate(input.paths, input.areas);
    QHash<const GeoZone*, qsizetype> indexes;
    for (const Evaluator::ZoneResult &zoneResult : result.zones) {
        if (!zoneResult.entered()) {
            continue;
        }
        Zone zone = makeZone(zoneResult.zone, Involvement::Entered);
        zone.activeDuringFlight = zoneResult.active();
        zone.inArea = !zoneResult.areas.isEmpty();
        for (const Evaluator::Crossing &crossing : zoneResult.crossings) {
            Crossing briefed;
            const Evaluator::Path &path = input.paths[crossing.entry.path];
            briefed.path = path.label;
            briefed.entryDistance = missionDistance(crossing.entry);
            briefed.exitDistance = missionDistance(crossing.exit);
            briefed.entryItem = itemBefore(briefed.entryDistance);
            briefed.exitItem = itemBefore(briefed.exitDistance);
            briefed.entry = crossing.entry.position.coordinate;
            briefed.exit = crossing.exit.position.coordinate;
            briefed.entryTime = crossing.entry.time;
            briefed.exitTime = crossing.exit.time;
            briefed.active = crossing.active;
            extend(briefed.pathAltitudeMin, briefed.pathAltitudeMax, crossing.entry.position.altitudeAMSL);
            extend(briefed.pathAltitudeMin, briefed.pathAltitudeMax, crossing.exit.position.altitudeAMSL);
            if (crossing.exit.path == crossing.entry.path) {
                for (qsizetype i = crossing.entry.segment + 1; (i <= crossing.exit.segment) && (i < path.vertices.count()); i++) {
                    extend(briefed.pathAltitudeMin, briefed.pathAltitudeMax, vertexAltitudeAMSL(path, path.vertices[i]));
                }
            }

            // Limits of the volumes the path is inside at the entry
            QList<int> volumes;
            for (const Evaluator::Stretch &stretch : zoneResult.stretches) {
                if ((stretch.entry.path == crossing.entry.path) && (stretch.entry.distance <= (crossing.entry.distance + 0.01)) &&
                    (stretch.exit.distance >= (crossing.entry.distance - 0.01)) && !volumes.contains(stretch.volume)) {
                    volumes.append(stretch.volume);
                }
            }
            const QList<GeoZone::Volume> &zoneVolumes = zoneResult.zone->data().volumes;
            bool floorKnown = !volumes.isEmpty();
            bool ceilingKnown = !volumes.isEmpty();
            for (const int volume : std::as_const(volumes)) {
                if ((volume < 0) || (volume >= zoneVolumes.count())) {
                    continue;
                }
                const double floor = GeoZone::limitAMSL(zoneVolumes[volume], false, crossing.entry.position);
                const double ceiling = GeoZone::limitAMSL(zoneVolumes[volume], true, crossing.entry.position);
                floorKnown = floorKnown && !qIsNaN(floor);
                ceilingKnown = ceilingKnown && !qIsNaN(ceiling);
                briefed.zoneFloor = qIsNaN(briefed.zoneFloor) ? floor : (std::min)(briefed.zoneFloor, floor);
                briefed.zoneCeiling = qIsNaN(briefed.zoneCeiling) ? ceiling : (std::max)(briefed.zoneCeiling, ceiling);
            }
            if (!floorKnown) {
                briefed.zoneFloor = qQNaN();
            }
            if (!ceilingKnown) {
                briefed.zoneCeiling = qQNaN();
            }
            zone.crossings.append(briefed);
        }
        indexes.insert(zoneResult.zone, briefing.zones.count());
        briefing.zones.append(zone);
    }

    // Zones the returns from along the mission enter while they apply
    if (!input.contingencyPaths.isEmpty()) {
        const Evaluator::Result contingency = evaluator.evaluate(input.contingencyPaths);
        QHash<const GeoZone*, QStringList> labels;
        if (input.groupContingencies) {
            labels = input.groupContingencies(contingency);
        } else {
            for (const Evaluator::ZoneResult &zoneResult : contingency.zones) {
                for (const Evaluator::Crossing &crossing : zoneResult.crossings) {
                    const QString label = input.contingencyPaths.value(crossing.entry.path).label;
                    if (crossing.active && !labels[zoneResult.zone].contains(label)) {
                        labels[zoneResult.zone].append(label);
                    }
                }
            }
        }
        QList<const GeoZone*> contingencyZones;
        for (const Evaluator::ZoneResult &zoneResult : contingency.zones) {
            const QStringList zoneLabels = labels.value(zoneResult.zone);
            if (zoneLabels.isEmpty()) {
                continue;
            }
            if (indexes.contains(zoneResult.zone)) {
                briefing.zones[indexes.value(zoneResult.zone)].contingencies = zoneLabels;
            } else {
                contingencyZones.append(zoneResult.zone);
            }
        }
        GeoZonePathEvaluator::sortBySeverity(contingencyZones);
        for (const GeoZone *contingencyZone : std::as_const(contingencyZones)) {
            Zone zone = makeZone(contingencyZone, Involvement::Contingency);
            zone.contingencies = labels.value(contingencyZone);
            zone.activeDuringFlight = true;
            indexes.insert(contingencyZone, briefing.zones.count());
            briefing.zones.append(zone);
        }
    }

    // Restricted zones passed above or below, then those horizontally near the flight paths
    const GeoZonePolicy nearbyPolicy(nearbyRestrictions());
    QList<Zone> nearby;
    for (const Evaluator::ZoneResult &zoneResult : result.zones) {
        if (zoneResult.entered() || indexes.contains(zoneResult.zone) || !nearbyPolicy.acceptsRestriction(zoneResult.zone->restriction())) {
            continue;
        }
        Zone zone = makeZone(zoneResult.zone, Involvement::Nearby);
        zone.distance = 0;
        for (const Evaluator::Stretch &stretch : zoneResult.stretches) {
            for (const Evaluator::VerticalPart &part : stretch.parts) {
                if ((part.position != GeoZone::VerticalPosition::Inside) && (zone.passed == GeoZone::VerticalPosition::Inside)) {
                    zone.passed = part.position;
                }
            }
        }
        indexes.insert(zoneResult.zone, -1);
        nearby.append(zone);
    }

    if (briefing.nearbyDistance > 0) {
        // Positions along the paths, close enough that no zone nearer than the nearby distance falls between them
        double length = 0;
        for (const Evaluator::Path &path : input.paths) {
            for (qsizetype i = 1; i < path.vertices.count(); i++) {
                length += path.vertices[i - 1].coordinate.distanceTo(path.vertices[i].coordinate);
            }
        }
        const double spacing = (std::max)({ briefing.nearbyDistance / 2, 10.0, length / maxNearbySamples });
        QList<QGeoCoordinate> samples;
        for (const Evaluator::Path &path : input.paths) {
            for (qsizetype i = 0; i < path.vertices.count(); i++) {
                const QGeoCoordinate &coordinate = path.vertices[i].coordinate;
                if (!coordinate.isValid()) {
                    continue;
                }
                samples.append(coordinate);
                if ((i + 1) < path.vertices.count()) {
                    const QGeoCoordinate &next = path.vertices[i + 1].coordinate;
                    const double distance = coordinate.distanceTo(next);
                    const double azimuth = coordinate.azimuthTo(next);
                    for (double along = spacing; along < distance; along += spacing) {
                        samples.append(coordinate.atDistanceAndAzimuth(along, azimuth));
                    }
                }
            }
        }
        GeoZoneBox area = GeoZoneBox::fromPath(samples);
        if (area.isValid()) {
            const double latitudeMargin = briefing.nearbyDistance / 111000.0;
            const double latitude = (std::max)(std::abs(area.south()), std::abs(area.north()));
            const double longitudeMargin = latitudeMargin / (std::max)(std::cos(qDegreesToRadians((std::min)(latitude, 89.0))), 0.01);
            area = GeoZoneBox((std::max)(area.south() - latitudeMargin, -90.0), area.west() - longitudeMargin, (std::min)(area.north() + latitudeMargin, 90.0),
                              area.east() + longitudeMargin);
            for (const GeoZone *candidate : catalog.zonesInArea(area, nearbyPolicy)) {
                if (indexes.contains(candidate)) {
                    continue;
                }
                double closest = qInf();
                for (const QGeoCoordinate &sample : std::as_const(samples)) {
                    closest = (std::min)(closest, candidate->distanceTo(GeoZone::Position{ sample }, 0, briefing.nearbyDistance));
                    if (closest <= 0) {
                        break;
                    }
                }
                if (closest <= briefing.nearbyDistance) {
                    Zone zone = makeZone(candidate, Involvement::Nearby);
                    zone.distance = closest;
                    nearby.append(zone);
                }
            }
        }
    }
    std::stable_sort(nearby.begin(), nearby.end(), [](const Zone &a, const Zone &b) {
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        return GeoZone::restrictionSeverity(a.restriction) < GeoZone::restrictionSeverity(b.restriction);
    });
    briefing.zones.append(nearby);
    return briefing;
}
