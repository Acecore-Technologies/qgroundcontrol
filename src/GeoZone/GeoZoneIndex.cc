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
    if ((south > other.north) || (other.south > north)) {
        return false;
    }
    // Either box may cross the antimeridian
    for (const double shift : { -360.0, 0.0, 360.0 }) {
        if ((west <= (other.east + shift)) && ((other.west + shift) <= east)) {
            return true;
        }
    }
    return false;
}

GeoZoneIndex::Box GeoZoneIndex::_box(double south, double west, double north, double east)
{
    return Box{ north, south, west, (west > east) ? (east + 360) : east };
}

GeoZoneIndex::Box GeoZoneIndex::_box(const QGeoRectangle &rectangle)
{
    return _box(rectangle.bottomRight().latitude(), rectangle.topLeft().longitude(), rectangle.topLeft().latitude(), rectangle.bottomRight().longitude());
}

std::pair<double, double> GeoZoneIndex::longitudeSpan(double longitude1, double longitude2)
{
    const double west = (std::min)(longitude1, longitude2);
    const double east = (std::max)(longitude1, longitude2);
    return ((east - west) > 180) ? std::make_pair(east, west) : std::make_pair(west, east);
}

int GeoZoneIndex::_cell(double degrees)
{
    return static_cast<int>(std::floor(degrees / cellSizeDegrees));
}

int GeoZoneIndex::_wrappedLongitudeCell(int cell)
{
    const int cellCount = static_cast<int>(std::lround(360 / cellSizeDegrees));
    const int first = _cell(-180);
    return first + ((((cell - first) % cellCount) + cellCount) % cellCount);
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
                _cells[_key(latitudeCell, _wrappedLongitudeCell(longitudeCell))].append(i);
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
    return query(area.bottomRight().latitude(), area.topLeft().longitude(), area.topLeft().latitude(), area.bottomRight().longitude());
}

QList<int> GeoZoneIndex::query(double south, double west, double north, double east) const
{
    QList<int> result;
    if (_boxes.isEmpty()) {
        return result;
    }

    const Box queryBox = _box(south, west, north, east);
    const int southCell = _cell(queryBox.south);
    const int northCell = _cell(queryBox.north);
    const int westCell = _cell(queryBox.west);
    const int eastCell = _cell(queryBox.east);
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
            const auto it = _cells.constFind(_key(latitudeCell, _wrappedLongitudeCell(longitudeCell)));
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
