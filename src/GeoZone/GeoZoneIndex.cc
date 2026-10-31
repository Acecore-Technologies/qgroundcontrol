/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneIndex.h"
#include "GeoZone.h"

#include <algorithm>
#include <cmath>

bool GeoZoneIndex::Box::intersects(const Box &other) const
{
    return (west <= other.east) && (other.west <= east) && (south <= other.north) && (other.south <= north);
}

GeoZoneIndex::Box GeoZoneIndex::_box(const QGeoRectangle &rectangle)
{
    Box box;
    box.north = rectangle.topLeft().latitude();
    box.south = rectangle.bottomRight().latitude();
    box.west = rectangle.topLeft().longitude();
    box.east = rectangle.bottomRight().longitude();
    // Boxes crossing the antimeridian are rare for geozones, cover all longitudes for them
    if (box.west > box.east) {
        box.west = -180;
        box.east = 180;
    }
    return box;
}

int GeoZoneIndex::_cell(double degrees)
{
    return static_cast<int>(std::floor(degrees / cellSizeDegrees));
}

quint64 GeoZoneIndex::_key(int latitudeCell, int longitudeCell)
{
    return (static_cast<quint64>(static_cast<quint32>(latitudeCell)) << 32) | static_cast<quint32>(longitudeCell);
}

void GeoZoneIndex::clear()
{
    _boxes.clear();
    _cells.clear();
    _largeZones.clear();
    _marks.clear();
}

void GeoZoneIndex::build(const QList<GeoZone*> &zones)
{
    clear();
    _boxes.reserve(zones.count());
    for (int i = 0; i < zones.count(); i++) {
        const QGeoRectangle rectangle = zones[i]->boundingBox();
        if (!rectangle.isValid()) {
            // Never matches a query
            _boxes.append(Box{ -1000, -1000, -1000, -1000 });
            continue;
        }

        const Box box = _box(rectangle);
        _boxes.append(box);
        const int south = _cell(box.south);
        const int north = _cell(box.north);
        const int west = _cell(box.west);
        const int east = _cell(box.east);
        if ((static_cast<qint64>(north - south + 1) * (east - west + 1)) > maxCellsPerZone) {
            _largeZones.append(i);
            continue;
        }
        for (int latitudeCell = south; latitudeCell <= north; latitudeCell++) {
            for (int longitudeCell = west; longitudeCell <= east; longitudeCell++) {
                _cells[_key(latitudeCell, longitudeCell)].append(i);
            }
        }
    }
    _marks.fill(0, _boxes.count());
    _stamp = 0;
}

QList<int> GeoZoneIndex::query(const QGeoRectangle &area) const
{
    if (!area.isValid()) {
        return QList<int>();
    }
    const Box box = _box(area);
    return query(box.south, box.west, box.north, box.east);
}

QList<int> GeoZoneIndex::query(double south, double west, double north, double east) const
{
    QList<int> result;
    if (_boxes.isEmpty()) {
        return result;
    }

    const Box queryBox{ north, south, west, east };
    const int southCell = _cell(south);
    const int northCell = _cell(north);
    const int westCell = _cell(west);
    const int eastCell = _cell(east);
    if ((static_cast<qint64>(northCell - southCell + 1) * (eastCell - westCell + 1)) > maxCellsPerQuery) {
        for (int i = 0; i < _boxes.count(); i++) {
            if (_boxes[i].intersects(queryBox)) {
                result.append(i);
            }
        }
        return result;
    }

    if (++_stamp == 0) {
        _marks.fill(0);
        _stamp = 1;
    }
    const auto consider = [this, &queryBox, &result](int index) {
        if (_marks[index] != _stamp) {
            _marks[index] = _stamp;
            if (_boxes[index].intersects(queryBox)) {
                result.append(index);
            }
        }
    };

    for (const int index : _largeZones) {
        consider(index);
    }
    for (int latitudeCell = southCell; latitudeCell <= northCell; latitudeCell++) {
        for (int longitudeCell = westCell; longitudeCell <= eastCell; longitudeCell++) {
            const auto it = _cells.constFind(_key(latitudeCell, longitudeCell));
            if (it != _cells.constEnd()) {
                for (const int index : it.value()) {
                    consider(index);
                }
            }
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}
