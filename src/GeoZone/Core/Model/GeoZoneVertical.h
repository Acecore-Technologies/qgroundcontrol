/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <optional>
#include <utility>

#include "GeoZone.h"

/// Vertical datums of geozone limits: the conversion of the published limits to heights which compare with the
/// altitudes of positions, and the comparisons the checks, alerts, profile and route search use.
///
/// Policy for what isn't known: a limit bounds a volume only when it can be compared. A limit in an unknown
/// reference, above ground where the terrain elevation is unknown, or compared with an unknown altitude doesn't bound
/// the volume, which then extends over all altitudes on that side of it, so that unknowns count as inside. Missing
/// floors, floors at the ground and the published unlimited ceiling don't bound it either. Flight levels are converted
/// to heights above mean sea level with the QNH of GeoZoneEnvironment, then floors are lowered and ceilings raised by
/// flightLevelMargin.
namespace GeoZoneVertical {

/// Published as upper limit when a zone has no ceiling
inline constexpr double unlimitedSentinel = 99999.0;
inline constexpr double metersPerFoot = 0.3048;

/// Fraction of the height of a flight level above mean sea level by which floors are lowered and ceilings raised
/// when they are compared. The conversion assumes the temperatures of the standard atmosphere, in colder air flight
/// levels are lower by about 4 % of their height per 10 degrees Celsius.
inline constexpr double flightLevelMargin = 0.04;

/// Why the comparison of altitudes with the limits of a volume is approximate
enum Approximation {
    TerrainUnknown = 0x1,   ///< A limit above ground where the terrain elevation is unknown, so it doesn't bound the volume
};
Q_DECLARE_FLAGS(Approximations, Approximation)

/// A limit of a volume with the heights of two positions in its datum
struct Bound {
    double limit = 0;   ///< Meters, flight levels converted and widened, see comparableLimit
    double from = 0;    ///< Heights of the positions in the datum of the limit
    double to = 0;
};

/// @return true if the volume has a floor above the ground. Zones from the ground have none, vehicles on the ground
///         or terrain data errors can be slightly below it.
bool hasFloor(const GeoZone::Volume &volume);

/// @return true if the volume has a ceiling below the published unlimited one
bool hasCeiling(const GeoZone::Volume &volume);

/// @return The limit of the volume in meters in its datum: as published, except for flight levels which are
///         converted to heights above mean sea level with the current QNH, then widened by flightLevelMargin
double comparableLimit(const GeoZone::Volume &volume, bool upper);

/// @return Heights of the positions in the datum of a reference, nothing when they can't be compared: an unknown
///         reference, altitude, or terrain elevation for limits above ground
std::optional<std::pair<double, double>> referenceHeights(GeoZone::VerticalReference reference, const GeoZone::Position &from,
                                                          const GeoZone::Position &to);

/// The policy: the lower or upper limit of the volume with the heights of the positions to compare it with, nothing
/// when it doesn't bound the volume, because the volume has no such limit or it can't be compared
std::optional<Bound> bound(const GeoZone::Volume &volume, bool upper, const GeoZone::Position &from, const GeoZone::Position &to);

/// @return true if the vertical limits of the volume overlap the altitudes between the positions, extended by
///         the margin
bool overlap(const GeoZone::Volume &volume, const GeoZone::Position &from, const GeoZone::Position &to, double margin = 0);

/// @return Fractions of the straight path between the positions, with altitudes and terrain interpolated linearly,
///         which are within the vertical limits of the volume extended by the margin, nothing when none is
std::optional<std::pair<double, double>> interval(const GeoZone::Volume &volume, const GeoZone::Position &from, const GeoZone::Position &to,
                                                  double margin = 0);

/// @return Where the altitude of the position is relative to the vertical limits of the volume
GeoZone::VerticalPosition position(const GeoZone::Volume &volume, const GeoZone::Position &position);

/// @return Why the comparison of the altitudes between the positions with the limits of the volume is approximate,
///         see the policy
Approximations approximations(const GeoZone::Volume &volume, const GeoZone::Position &from, const GeoZone::Position &to);

/// @return Lower or upper limit of the volume in meters above mean sea level at the position, using its terrain
///         elevation for limits above ground. NaN when the volume has no such limit or it can't be converted, like
///         limits above unknown terrain or in unknown references. Flight levels are widened like when compared.
double limitAMSL(const GeoZone::Volume &volume, bool upper, const GeoZone::Position &position);

/// @return Height in meters above mean sea level of a pressure altitude, like a flight level, where the sea
///         level pressure is qnhHPa. As read by an altimeter set to the QNH, which is the true altitude in the
///         standard atmosphere: the pressure altitude less the pressure altitude of the QNH,
///         44330.8 m * (1 - (QNH / 1013.25 hPa)^0.190263), about 8.3 m per hPa near the sea level.
/// @param pressureAltitude Meters in the ICAO standard atmosphere, 30.48 m per flight level
double pressureAltitudeToAMSL(double pressureAltitude, double qnhHPa);

} // namespace GeoZoneVertical

Q_DECLARE_OPERATORS_FOR_FLAGS(GeoZoneVertical::Approximations)
