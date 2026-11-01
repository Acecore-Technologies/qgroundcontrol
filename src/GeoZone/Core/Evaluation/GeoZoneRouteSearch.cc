/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRouteSearch.h"
#include "GeoZoneRouteDetour.h"
#include "GeoZoneRouteObstacles.h"
#include "GeoZoneRouteVertical.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kShortDetourFactor = 1.3;      ///< Detours searched first are at most this many times as long as the leg, or
constexpr double kShortDetourAllowance = 1000;  ///< this many meters longer, whichever is more

/// @return Names of the zones, the most restrictive first
QStringList _names(QList<const GeoZone*> zones)
{
    GeoZonePathEvaluator::sortBySeverity(zones);
    QStringList names;
    for (const GeoZone *zone : std::as_const(zones)) {
        if (!names.contains(zone->displayName())) {
            names.append(zone->displayName());
        }
    }
    return names;
}

/// Sets the detour, or why there is none, from the outcome of the search
void _setOutcome(GeoZoneRouteSearch::Route &route, const GeoZoneRouteDetour &search, GeoZoneRouteDetour::Outcome outcome, const QList<const GeoZone*> &avoided,
                 double maxLength)
{
    using Failure = GeoZoneRouteSearch::Failure;
    using Outcome = GeoZoneRouteDetour::Outcome;
    const QList<const GeoZone*> &blocking = search.blockingZones();
    route.zones = _names(avoided);
    switch (outcome) {
    case Outcome::Found:
        route.waypoints = search.waypoints();
        route.length = search.length();
        route.cost = (std::max)(route.length - search.directLength(), 0.0);
        return;
    case Outcome::StartInside:
        route.failure = Failure::StartInside;
        break;
    case Outcome::EndInside:
        route.failure = Failure::EndInside;
        break;
    case Outcome::Enclosed:
        route.failure = Failure::Enclosed;
        break;
    case Outcome::TooLong:
        route.failure = Failure::TooLong;
        route.length = search.length();
        route.limit = maxLength;
        return;
    case Outcome::SearchLimit:
        route.failure = Failure::SearchLimit;
        return;
    case Outcome::EntersZone:
        route.failure = Failure::EntersZone;
        break;
    case Outcome::Cancelled:
        route.failure = Failure::Cancelled;
        return;
    }
    if (!blocking.isEmpty()) {
        route.zones = _names(blocking);
    }
}

} // namespace

GeoZoneRouteSearch::GeoZoneRouteSearch(const GeoZoneCatalog &catalog, const GeoZonePolicy &policy, const QDateTime &time)
    : _catalog(catalog)
    , _policy(policy)
    , _time(time)
    , _cache(std::make_shared<Cache>())
{
}

void GeoZoneRouteSearch::setTerrainLookup(const GeoZonePathEvaluator::TerrainLookup &lookup)
{
    _terrainLookup = lookup;
}

double GeoZoneRouteSearch::maxDetourLength(double directLength, const Settings &settings)
{
    return (std::max)(directLength * settings.maxDetourFactor, directLength + settings.minDetourAllowance);
}

GeoZonePathEvaluator GeoZoneRouteSearch::_evaluator() const
{
    GeoZonePathEvaluator evaluator(_catalog, _policy, _time);
    if (_terrainLookup) {
        evaluator.setTerrainLookup(_terrainLookup);
    }
    return evaluator;
}

QList<GeoZone::Position> GeoZoneRouteSearch::_samples(const Leg &leg) const
{
    return GeoZoneRouteVertical::samples(leg, _settings, _terrainLookup);
}

QList<const GeoZone*> GeoZoneRouteSearch::_enteredZones(const GeoZonePathEvaluator &evaluator, const QList<GeoZone::Position> &path, const Leg &leg) const
{
    GeoZonePathEvaluator::Path evaluatorPath = GeoZonePathEvaluator::Path::fromPositions(path);
    evaluatorPath.setTimes(leg.start, leg.end);
    QList<const GeoZone*> zones = evaluator.evaluate({ evaluatorPath }).activeEnteredZones();
    GeoZonePathEvaluator::sortBySeverity(zones);
    return zones;
}

GeoZoneRouteSearch::Result GeoZoneRouteSearch::plan(const Leg &leg) const
{
    Result result;
    if (_cancel.isCancelled()) {
        result.cancelled = true;
        return result;
    }
    const QList<GeoZone::Position> samples = _samples(leg);
    const QList<const GeoZone*> crossed = _enteredZones(_evaluator(), { samples.constFirst(), samples.constLast() }, leg);
    if (crossed.isEmpty()) {
        return result;
    }
    result.crossedZones = _names(crossed);
    for (const Kind kind : { Kind::Detour, Kind::Climb, Kind::Descent }) {
        const Route route = (kind == Kind::Detour) ? _detour(leg, samples, crossed) : _vertical(kind, leg, samples, crossed);
        if ((route.failure == Failure::Cancelled) || _cancel.isCancelled()) {
            result.cancelled = true;
            result.routes.clear();
            return result;
        }
        result.routes.append(route);
    }
    std::stable_sort(result.routes.begin(), result.routes.end(), [](const Route &a, const Route &b) {
        if (a.isValid() != b.isValid()) {
            return a.isValid();
        }
        return a.isValid() && (a.cost < b.cost);
    });
    return result;
}

GeoZoneRouteSearch::Route GeoZoneRouteSearch::detour(const Leg &leg) const
{
    const QList<GeoZone::Position> samples = _samples(leg);
    return _detour(leg, samples, _enteredZones(_evaluator(), { samples.constFirst(), samples.constLast() }, leg));
}

GeoZoneRouteSearch::Route GeoZoneRouteSearch::_detour(const Leg &leg, const QList<GeoZone::Position> &samples, const QList<const GeoZone*> &crossed) const
{
    Route route;
    route.kind = Kind::Detour;
    const GeoZone::Position &from = samples.constFirst();
    const GeoZone::Position &to = samples.constLast();

    // The volumes within reach whose limits overlap the altitudes of the leg, over the range of the terrain along it.
    // The evaluation of the paths finds those the terrain elsewhere brings closer.
    double lowTerrain = std::numeric_limits<double>::infinity();
    double highTerrain = -std::numeric_limits<double>::infinity();
    for (const GeoZone::Position &sample : samples) {
        if (!qIsNaN(sample.terrainAMSL)) {
            lowTerrain = (std::min)(lowTerrain, sample.terrainAMSL);
            highTerrain = (std::max)(highTerrain, sample.terrainAMSL);
        }
    }
    if (lowTerrain > highTerrain) {
        lowTerrain = highTerrain = qQNaN();
    }
    const GeoZone::Position low{ from.coordinate, (std::min)(from.altitudeAMSL, to.altitudeAMSL), highTerrain };
    const GeoZone::Position high{ to.coordinate, (std::max)(from.altitudeAMSL, to.altitudeAMSL), lowTerrain };
    GeoZonePolicy policy = _policy;
    if ((policy.schedule() == GeoZonePolicy::Schedule::Planned) && leg.start.isValid()) {
        const int tolerance = GeoZonePathEvaluator::defaultScheduleToleranceSecs;
        policy = policy.activeDuring(leg.start.addSecs(-tolerance), (leg.end.isValid() ? leg.end : leg.start).addSecs(tolerance));
    }

    // Detours change altitude evenly from the start to the end of the leg, like the leg
    const GeoZonePathEvaluator evaluator = _evaluator();
    const GeoZoneRouteDetour::Verify verify = [this, &evaluator, &leg, &from, &to](const QList<QGeoCoordinate> &path) {
        QList<double> distances = { 0 };
        for (qsizetype i = 1; i < path.count(); i++) {
            distances.append(distances.constLast() + path[i - 1].distanceTo(path[i]));
        }
        QList<GeoZone::Position> positions;
        for (qsizetype i = 0; i < path.count(); i++) {
            const double fraction = (distances.constLast() > 0) ? (distances[i] / distances.constLast()) : 0;
            positions.append(GeoZone::Position{ path[i], from.altitudeAMSL + ((to.altitudeAMSL - from.altitudeAMSL) * fraction) });
        }
        positions.first().terrainAMSL = from.terrainAMSL;
        positions.last().terrainAMSL = to.terrainAMSL;
        return _enteredZones(evaluator, positions, leg);
    };

    // Most detours are short, so the zones within the reach of a short one are searched first, which are far fewer
    const double directLength = from.coordinate.distanceTo(to.coordinate);
    const double maxLength = maxDetourLength(directLength, _settings);
    const double shortLength = (std::min)(maxLength, (std::max)(directLength * kShortDetourFactor, directLength + kShortDetourAllowance));
    for (const double length : { shortLength, maxLength }) {
        GeoZoneRouteDetour search(from, to, _settings.margin, length, _settings.maxSearchWork, _cancel, *_cache, route.statistics);
        search.setPreliminary(length < maxLength);
        for (const GeoZone *zone : _catalog.zonesInArea(search.reachBox(), policy)) {
            const QList<GeoZone::Volume> &volumes = zone->data().volumes;
            for (int volume = 0; volume < volumes.count(); volume++) {
                if (GeoZone::verticalOverlap(volumes[volume], low, high)) {
                    search.addObstacle(zone, volume);
                }
            }
        }
        const GeoZoneRouteDetour::Outcome outcome = search.run(verify);
        if ((outcome != GeoZoneRouteDetour::Outcome::TooLong) || (length >= maxLength)) {
            _setOutcome(route, search, outcome, crossed, maxLength);
            break;
        }
    }
    return route;
}

GeoZoneRouteSearch::Route GeoZoneRouteSearch::detourAround(const QGeoCoordinate &from, const QGeoCoordinate &to, const QList<const GeoZone*> &zones,
                                                           const Settings &settings, const GeoZoneCancelToken &cancel)
{
    Route route;
    route.kind = Kind::Detour;
    Cache cache;
    GeoZoneRouteDetour search(GeoZone::Position{ from }, GeoZone::Position{ to }, settings.margin, std::numeric_limits<double>::infinity(),
                              settings.maxSearchWork, cancel, cache, route.statistics);
    for (const GeoZone *zone : zones) {
        for (int volume = 0; volume < zone->data().volumes.count(); volume++) {
            search.addObstacle(zone, volume);
        }
    }
    _setOutcome(route, search, search.run(GeoZoneRouteDetour::Verify()), zones, std::numeric_limits<double>::infinity());
    return route;
}

GeoZoneRouteSearch::Route GeoZoneRouteSearch::_vertical(Kind kind, const Leg &leg, const QList<GeoZone::Position> &samples, const QList<const GeoZone*> &crossed) const
{
    const GeoZonePathEvaluator evaluator = _evaluator();
    const GeoZoneRouteVertical vertical(evaluator, _settings, [this, &evaluator, &leg](const QList<GeoZone::Position> &path) {
        return _enteredZones(evaluator, path, leg);
    }, _cancel);
    QList<const GeoZone*> zones;
    Route route = vertical.route(kind, samples, crossed, zones);
    route.zones = _names(zones);
    return route;
}
