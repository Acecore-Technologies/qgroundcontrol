/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneVertical.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneGeometry.h"

#include <cmath>

#include <algorithm>

namespace GeoZoneVertical {

namespace {

// ICAO standard atmosphere: sea level temperature over the temperature lapse rate in meters, and the exponent of the
// pressure ratio R * L / (g * M)
constexpr double kStandardTemperatureOverLapseRate = 288.15 / 0.0065;
constexpr double kPressureExponent = 0.190263;

} // namespace

bool hasFloor(const GeoZone::Volume &volume)
{
    return !qIsNaN(volume.lowerLimit) && !((volume.lowerReference == GeoZone::VerticalReference::Ground) && (volume.lowerLimit <= 0));
}

bool hasCeiling(const GeoZone::Volume &volume)
{
    return !qIsNaN(volume.upperLimit) && (volume.upperLimit < unlimitedSentinel);
}

bool aboveGround(const GeoZone::Volume &volume)
{
    return (hasFloor(volume) && (volume.lowerReference == GeoZone::VerticalReference::Ground)) ||
           (hasCeiling(volume) && (volume.upperReference == GeoZone::VerticalReference::Ground));
}

double comparableLimit(const GeoZone::Volume &volume, bool upper)
{
    const double meters = (upper ? volume.upperLimit : volume.lowerLimit) * (volume.feet ? metersPerFoot : 1.0);
    if ((upper ? volume.upperReference : volume.lowerReference) != GeoZone::VerticalReference::FlightLevel) {
        return meters;
    }
    const double amsl = pressureAltitudeToAMSL(meters, GeoZoneEnvironment::qnhHPa());
    const double margin = flightLevelMargin * std::abs(amsl);
    return upper ? (amsl + margin) : (amsl - margin);
}

std::optional<std::pair<double, double>> referenceHeights(GeoZone::VerticalReference reference, const GeoZone::Position &from, const GeoZone::Position &to)
{
    double height1 = from.altitudeAMSL;
    double height2 = to.altitudeAMSL;
    switch (reference) {
    case GeoZone::VerticalReference::Ground:
        height1 -= from.terrainAMSL;
        height2 -= to.terrainAMSL;
        break;
    case GeoZone::VerticalReference::Ellipsoid:
        // Heights above the ellipsoid differ from mean sea level by the geoid height of up to about 100 m
        height1 += GeoZoneGeometry::geoidHeight(from.coordinate);
        height2 += GeoZoneGeometry::geoidHeight(to.coordinate);
        break;
    case GeoZone::VerticalReference::MeanSeaLevel:
    case GeoZone::VerticalReference::FlightLevel:
        // Flight levels are compared once converted to heights above mean sea level, see comparableLimit
        break;
    case GeoZone::VerticalReference::Unknown:
        return std::nullopt;
    }
    if (qIsNaN(height1) || qIsNaN(height2)) {
        return std::nullopt;
    }
    return std::make_pair(height1, height2);
}

std::optional<Bound> bound(const GeoZone::Volume &volume, bool upper, const GeoZone::Position &from, const GeoZone::Position &to)
{
    if (!(upper ? hasCeiling(volume) : hasFloor(volume))) {
        return std::nullopt;
    }
    const auto heights = referenceHeights(upper ? volume.upperReference : volume.lowerReference, from, to);
    if (!heights) {
        return std::nullopt;
    }
    return Bound{ comparableLimit(volume, upper), heights->first, heights->second };
}

bool overlap(const GeoZone::Volume &volume, const GeoZone::Position &from, const GeoZone::Position &to, double margin)
{
    if (const auto floor = bound(volume, false, from, to); floor && ((std::max)(floor->from, floor->to) < (floor->limit - margin))) {
        return false;
    }
    if (const auto ceiling = bound(volume, true, from, to); ceiling && ((std::min)(ceiling->from, ceiling->to) > (ceiling->limit + margin))) {
        return false;
    }
    return true;
}

std::optional<std::pair<double, double>> interval(const GeoZone::Volume &volume, const GeoZone::Position &from, const GeoZone::Position &to, double margin)
{
    double first = 0;
    double last = 1;
    // Keeps the fractions at which the height in the datum of the limit is on the inner side of it
    const auto constrain = [&first, &last, &volume, &from, &to, margin](bool upper) {
        const auto limit = bound(volume, upper, from, to);
        if (!limit) {
            return;
        }
        const double value = upper ? (limit->limit + margin) : (limit->limit - margin);
        const double slope = limit->to - limit->from;
        if (std::abs(slope) < 1e-9) {
            if (upper ? (limit->from > value) : (limit->from < value)) {
                first = 1;
                last = 0;
            }
            return;
        }
        const double crossing = (value - limit->from) / slope;
        if (upper == (slope > 0)) {
            last = (std::min)(last, crossing);
        } else {
            first = (std::max)(first, crossing);
        }
    };
    constrain(false);
    constrain(true);
    if (first > last) {
        return std::nullopt;
    }
    return std::make_pair(first, last);
}

GeoZone::VerticalPosition position(const GeoZone::Volume &volume, const GeoZone::Position &position)
{
    if (const auto floor = bound(volume, false, position, position); floor && (floor->from < floor->limit)) {
        return GeoZone::VerticalPosition::Below;
    }
    if (const auto ceiling = bound(volume, true, position, position); ceiling && (ceiling->from > ceiling->limit)) {
        return GeoZone::VerticalPosition::Above;
    }
    return GeoZone::VerticalPosition::Inside;
}

Approximations approximations(const GeoZone::Volume &volume, const GeoZone::Position &from, const GeoZone::Position &to)
{
    Approximations result;
    for (const bool upper : { false, true }) {
        if (!(upper ? hasCeiling(volume) : hasFloor(volume))) {
            continue;
        }
        switch (upper ? volume.upperReference : volume.lowerReference) {
        case GeoZone::VerticalReference::Unknown:
            result |= ReferenceUnknown;
            continue;
        case GeoZone::VerticalReference::Ground:
            if (qIsNaN(from.terrainAMSL) || qIsNaN(to.terrainAMSL)) {
                result |= TerrainUnknown;
            }
            break;
        case GeoZone::VerticalReference::FlightLevel:
            result |= FlightLevel;
            break;
        case GeoZone::VerticalReference::MeanSeaLevel:
        case GeoZone::VerticalReference::Ellipsoid:
            break;
        }
        if (qIsNaN(from.altitudeAMSL) || qIsNaN(to.altitudeAMSL)) {
            result |= AltitudeUnknown;
        }
    }
    return result;
}

double limitAMSL(const GeoZone::Volume &volume, bool upper, const GeoZone::Position &position)
{
    if (!(upper ? hasCeiling(volume) : hasFloor(volume))) {
        return qQNaN();
    }

    const double meters = comparableLimit(volume, upper);
    switch (upper ? volume.upperReference : volume.lowerReference) {
    case GeoZone::VerticalReference::Ground:
        return meters + position.terrainAMSL;
    case GeoZone::VerticalReference::MeanSeaLevel:
    case GeoZone::VerticalReference::FlightLevel:
        return meters;
    case GeoZone::VerticalReference::Ellipsoid:
        return meters - GeoZoneGeometry::geoidHeight(position.coordinate);
    case GeoZone::VerticalReference::Unknown:
        break;
    }
    return qQNaN();
}

double pressureAltitudeToAMSL(double pressureAltitude, double qnhHPa)
{
    const double qnhPressureAltitude = kStandardTemperatureOverLapseRate * (1.0 - std::pow(qnhHPa / GeoZoneEnvironment::kStandardPressureHPa, kPressureExponent));
    return pressureAltitude - qnhPressureAltitude;
}

} // namespace GeoZoneVertical
