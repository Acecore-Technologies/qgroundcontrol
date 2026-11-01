/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRouteVertical.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneRouteVector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kTolerance = 0.01;             ///< Meters altitudes may differ by, for rounding
constexpr int kMaxVerticalRounds = 8;           ///< Climbs and descents raising the targets for the zones they enter
constexpr int kMaxSamples = 2000;
constexpr double kUnlimitedCeiling = 99999;     ///< Published as upper limit when a zone has no ceiling, see GeoZone

using Failure = GeoZoneRouteSearch::Failure;

} // namespace

QList<GeoZone::Position> GeoZoneRouteVertical::samples(const GeoZoneRouteSearch::Leg &leg, const GeoZoneRouteSearch::Settings &settings,
                                                       const GeoZonePathEvaluator::TerrainLookup &terrainLookup)
{
    const GeoZoneGeometry::LocalProjection projection(leg.from.coordinate);
    const QPointF end = projection.toLocal(leg.to.coordinate);
    const int count = std::clamp(static_cast<int>(std::ceil(GeoZoneVector::length(end) / (std::max)(settings.sampleSpacing, 1.0))), 1, kMaxSamples);
    QList<GeoZone::Position> samples;
    samples.reserve(count + 1);
    for (int i = 0; i <= count; i++) {
        const double fraction = static_cast<double>(i) / count;
        GeoZone::Position position = (i == 0) ? leg.from : ((i == count) ? leg.to : GeoZone::Position{ projection.toGeo(end * fraction) });
        position.altitudeAMSL = leg.from.altitudeAMSL + ((leg.to.altitudeAMSL - leg.from.altitudeAMSL) * fraction);
        if (qIsNaN(position.terrainAMSL) && terrainLookup) {
            position.terrainAMSL = terrainLookup(position.coordinate);
        }
        samples.append(position);
    }
    return samples;
}

GeoZoneRouteVertical::GeoZoneRouteVertical(const GeoZonePathEvaluator &evaluator, const GeoZoneRouteSearch::Settings &settings, const Entered &entered,
                                           const GeoZoneCancelToken &cancel)
    : _evaluator(evaluator)
    , _settings(settings)
    , _entered(entered)
    , _cancel(cancel)
{
}

GeoZoneRouteSearch::Route GeoZoneRouteVertical::route(GeoZoneRouteSearch::Kind kind, const QList<GeoZone::Position> &samples,
                                                      const QList<const GeoZone*> &crossed, QList<const GeoZone*> &zones) const
{
    GeoZoneRouteSearch::Route route;
    route.kind = kind;
    zones = crossed;
    const bool climb = (kind == GeoZoneRouteSearch::Kind::Climb);
    const double fromAltitude = samples.constFirst().altitudeAMSL;
    const double toAltitude = samples.constLast().altitudeAMSL;
    if (qIsNaN(fromAltitude) || qIsNaN(toAltitude)) {
        route.failure = Failure::AltitudeUnknown;
        return route;
    }

    const GeoZonePathEvaluator::Path path = GeoZonePathEvaluator::Path::fromPositions({ samples.constFirst(), samples.constLast() });
    const qsizetype last = samples.count() - 1;
    QList<const GeoZone*> avoided;
    QList<const GeoZone*> blocking = crossed;
    double target = climb ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    for (int round = 0; round < kMaxVerticalRounds; round++) {
        if (_cancel.isCancelled()) {
            route.failure = Failure::Cancelled;
            return route;
        }

        // Above the highest ceiling or below the lowest floor where the leg passes the zones horizontally. Limits above
        // ground follow the terrain, which is linear between the samples.
        for (const GeoZone *zone : std::as_const(blocking)) {
            avoided.append(zone);
            for (const GeoZonePathEvaluator::Stretch &stretch : _evaluator.evaluateZone(zone, { path }).stretches) {
                const GeoZone::Volume &volume = zone->data().volumes[stretch.volume];
                // The samples between the entry and the exit, along the same line
                QList<GeoZone::Position> positions = { stretch.entry.position, stretch.exit.position };
                const auto firstSample = static_cast<qsizetype>(std::ceil(stretch.entry.fraction * last));
                const auto lastSample = static_cast<qsizetype>(std::floor(stretch.exit.fraction * last));
                for (qsizetype i = (std::max)(firstSample, qsizetype(0)); i <= (std::min)(lastSample, last); i++) {
                    positions.append(samples[i]);
                }
                for (const GeoZone::Position &position : std::as_const(positions)) {
                    const double limit = GeoZone::limitAMSL(volume, climb, position);
                    if (!qIsNaN(limit)) {
                        target = climb ? (std::max)(target, limit + _settings.verticalMargin) : (std::min)(target, limit - _settings.verticalMargin);
                        continue;
                    }
                    zones = { zone };
                    const double published = climb ? volume.upperLimit : volume.lowerLimit;
                    const GeoZone::VerticalReference reference = climb ? volume.upperReference : volume.lowerReference;
                    if (!climb && (qIsNaN(published) || ((reference == GeoZone::VerticalReference::Ground) && (published <= 0)))) {
                        route.failure = Failure::ReachesGround;
                    } else if ((reference == GeoZone::VerticalReference::Ground) && !qIsNaN(published) && (published < kUnlimitedCeiling) &&
                               qIsNaN(position.terrainAMSL)) {
                        route.failure = Failure::TerrainUnknown;
                    } else {
                        route.failure = climb ? Failure::NoCeiling : Failure::NoFloor;
                    }
                    return route;
                }
            }
        }
        zones = avoided;
        if (std::isinf(target)) {
            route.failure = Failure::EntersZone;
            return route;
        }

        // The start and the end move to the target when they are below it for climbs, above it for descents
        const double newFrom = climb ? (std::max)(fromAltitude, target) : (std::min)(fromAltitude, target);
        const double newTo = climb ? (std::max)(toAltitude, target) : (std::min)(toAltitude, target);
        QList<GeoZone::Position> moved = samples;
        double extreme = climb ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        for (qsizetype i = 0; i <= last; i++) {
            const double fraction = static_cast<double>(i) / last;
            moved[i].altitudeAMSL = newFrom + ((newTo - newFrom) * fraction);
            if (std::abs(moved[i].altitudeAMSL - samples[i].altitudeAMSL) < kTolerance) {
                continue;
            }
            if (qIsNaN(samples[i].terrainAMSL)) {
                route.failure = Failure::TerrainUnknown;
                return route;
            }
            const double height = moved[i].altitudeAMSL - samples[i].terrainAMSL;
            extreme = climb ? (std::max)(extreme, height) : (std::min)(extreme, height);
        }
        route.altitude = target;
        if (climb && !qIsNaN(_settings.maxAltitudeAGL) && (extreme > (_settings.maxAltitudeAGL + kTolerance))) {
            route.failure = Failure::AboveMaxAltitude;
            route.limit = extreme;
            return route;
        }
        if (!climb && (extreme < (_settings.minGroundClearance - kTolerance))) {
            route.failure = Failure::BelowMinClearance;
            route.limit = extreme;
            return route;
        }

        const QList<const GeoZone*> entered = _entered({ moved.constFirst(), moved.constLast() });
        if (entered.isEmpty()) {
            route.fromAltitude = newFrom;
            route.toAltitude = newTo;
            // The vehicle climbs to the new altitudes and descends back after the leg, or the other way round
            const double change = (std::max)(std::abs(newFrom - fromAltitude), std::abs(newTo - toAltitude));
            route.cost = (_settings.climbWeight + _settings.descentWeight) * change;
            return route;
        }
        blocking.clear();
        for (const GeoZone *zone : entered) {
            if (!avoided.contains(zone)) {
                blocking.append(zone);
            }
        }
        if (blocking.isEmpty()) {
            zones = entered;
            route.failure = Failure::EntersZone;
            return route;
        }
    }
    zones = blocking;
    route.failure = Failure::EntersZone;
    return route;
}
