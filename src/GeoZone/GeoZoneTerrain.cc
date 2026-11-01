/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneTerrain.h"
#include "GeoZoneEnvironment.h"
#include "GeoZonePathEvaluator.h"

namespace GeoZoneTerrain {

namespace {

Lookup &testLookup()
{
    static Lookup lookup;
    return lookup;
}

} // namespace

double elevationAt(const QGeoCoordinate &coordinate)
{
    if (!coordinate.isValid()) {
        return qQNaN();
    }
    if (testLookup()) {
        return testLookup()(coordinate);
    }
    return GeoZoneEnvironment::terrainElevation(coordinate);
}

void setLookup(const Lookup &lookup)
{
    testLookup() = lookup;
}

QList<GeoZone::Position> withTerrain(const QList<GeoZone::Position> &path, double spacing)
{
    return GeoZonePathEvaluator::withTerrain(path, elevationAt, spacing, maxPositions);
}

} // namespace GeoZoneTerrain
