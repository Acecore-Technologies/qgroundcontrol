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
}

void GeoZoneIndex::build(const QList<GeoZone*> &zones)
{
    clear();
    _boxes.reserve(zones.count());
    for (int i = 0; i < zones.count(); i++) {
        // Invalid boxes never match a query
        const GeoZoneBox &box = zones[i]->box();
        _boxes.append(box);
        if (!box.isValid()) {
            continue;
        }

        const int south = _cell(box.south());
        const int north = _cell(box.north());
        const int west = _cell(box.west());
        const int east = _cell(box.east());
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
}

QList<int> GeoZoneIndex::query(const QGeoRectangle &area) const
{
    return query(GeoZoneBox(area));
}

QList<int> GeoZoneIndex::query(const GeoZoneBox &queryBox) const
{
    QList<int> result;
    if (_boxes.isEmpty() || !queryBox.isValid()) {
        return result;
    }

    const int southCell = _cell(queryBox.south());
    const int northCell = _cell(queryBox.north());
    const int westCell = _cell(queryBox.west());
    const int eastCell = _cell(queryBox.east());
    if ((static_cast<qint64>(northCell - southCell + 1) * (eastCell - westCell + 1)) > maxCellsPerQuery) {
        for (int i = 0; i < _boxes.count(); i++) {
            if (_boxes[i].intersects(queryBox)) {
                result.append(i);
            }
        }
        return result;
    }

    // Zones spanning several cells are found once per cell
    QList<int> candidates = _largeZones;
    for (int latitudeCell = southCell; latitudeCell <= northCell; latitudeCell++) {
        for (int longitudeCell = westCell; longitudeCell <= eastCell; longitudeCell++) {
            const auto it = _cells.constFind(_key(latitudeCell, _wrappedLongitudeCell(longitudeCell)));
            if (it != _cells.constEnd()) {
                candidates.append(it.value());
            }
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    for (const int index : std::as_const(candidates)) {
        if (_boxes[index].intersects(queryBox)) {
            result.append(index);
        }
    }
    return result;
}
