/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneTerrain.h"
#include "TerrainQuery.h"

#include <algorithm>
#include <cmath>

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
    QList<double> altitudes;
    bool error = false;
    if (!TerrainAtCoordinateQuery::getAltitudesForCoordinates({ coordinate }, altitudes, error) || error || altitudes.isEmpty()) {
        return qQNaN();
    }
    return altitudes.constFirst();
}

void setLookup(const Lookup &lookup)
{
    testLookup() = lookup;
}

QList<GeoZone::Position> withTerrain(const QList<GeoZone::Position> &path, double spacing)
{
    double totalLength = 0;
    for (qsizetype i = 1; i < path.count(); i++) {
        if (path[i - 1].coordinate.isValid() && path[i].coordinate.isValid()) {
            totalLength += path[i - 1].coordinate.distanceTo(path[i].coordinate);
        }
    }
    spacing = (std::max)(spacing, totalLength / maxPositions);

    QList<GeoZone::Position> result;
    result.reserve(path.count());
    const auto appendWithTerrain = [&result](GeoZone::Position position) {
        if (std::isnan(position.terrainAMSL)) {
            position.terrainAMSL = elevationAt(position.coordinate);
            if (std::isnan(position.terrainAMSL) && !result.isEmpty()) {
                position.terrainAMSL = result.constLast().terrainAMSL;
            }
        }
        result.append(position);
    };

    for (qsizetype i = 0; i < path.count(); i++) {
        if (i > 0) {
            const GeoZone::Position &from = path[i - 1];
            const GeoZone::Position &to = path[i];
            if (from.coordinate.isValid() && to.coordinate.isValid()) {
                const double length = from.coordinate.distanceTo(to.coordinate);
                const double azimuth = from.coordinate.azimuthTo(to.coordinate);
                for (double along = spacing; along < (length - (spacing / 2)); along += spacing) {
                    GeoZone::Position position;
                    position.coordinate = from.coordinate.atDistanceAndAzimuth(along, azimuth);
                    position.terrainAMSL = elevationAt(position.coordinate);
                    if (std::isnan(position.terrainAMSL)) {
                        continue;
                    }
                    position.altitudeAMSL = from.altitudeAMSL + ((to.altitudeAMSL - from.altitudeAMSL) * (along / length));
                    result.append(position);
                }
            }
        }
        appendWithTerrain(path[i]);
    }
    return result;
}

} // namespace GeoZoneTerrain
