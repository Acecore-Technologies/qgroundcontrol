/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtPositioning/QGeoRectangle>

class GeoZone;

/// Grid of zone bounding boxes to find the zones near an area without testing every zone
class GeoZoneIndex
{
public:
    static constexpr double cellSizeDegrees = 0.1;
    static constexpr int maxCellsPerZone = 64;      ///< Larger zones are kept in a list which every query checks
    static constexpr int maxCellsPerQuery = 4096;   ///< Larger queries check every zone

    void build(const QList<GeoZone*> &zones);
    void clear();

    /// @return Indices into the zones passed to build, in ascending order, of the zones whose bounding box
    ///         intersects the area
    QList<int> query(const QGeoRectangle &area) const;

    /// Same as query, for bounds which QGeoRectangle can't represent like a single point
    QList<int> query(double south, double west, double north, double east) const;

private:
    struct Box {
        double north = 0;
        double south = 0;
        double west = 0;
        double east = 0;

        bool intersects(const Box &other) const;
    };

    static Box _box(const QGeoRectangle &rectangle);
    static int _cell(double degrees);
    static quint64 _key(int latitudeCell, int longitudeCell);

    QList<Box> _boxes;
    QHash<quint64, QList<int>> _cells;
    QList<int> _largeZones;
    mutable QList<quint32> _marks;
    mutable quint32 _stamp = 0;
};
