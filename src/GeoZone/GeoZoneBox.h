/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QPointF>
#include <QtCore/QtNumeric>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include <utility>

/// Latitude and longitude bounds which may cross the antimeridian, unlike QGeoRectangle a box can be a single point
/// or a meridian segment. Longitudes are continuous: west is from -180 up to 180 degrees and east is not smaller than
/// west, past 180 degrees for boxes crossing the antimeridian. Boxes of 360 degrees or more span all longitudes.
class GeoZoneBox
{
public:
    /// Invalid box, which contains and intersects nothing
    GeoZoneBox() = default;
    /// @param west Larger than east for boxes crossing the antimeridian
    GeoZoneBox(double south, double west, double north, double east);
    /// Invalid for an invalid rectangle
    explicit GeoZoneBox(const QGeoRectangle &rectangle);

    /// @return Smallest box containing the path, whose consecutive coordinates are joined the shorter way around,
    ///         like the edges of a polygon. Invalid coordinates are skipped.
    static GeoZoneBox fromPath(const QList<QGeoCoordinate> &path);
    /// Same as fromPath, for x longitude and y latitude in degrees, whose longitudes may already be continuous
    static GeoZoneBox fromLonLatPath(const QList<QPointF> &path);

    /// @return West and east longitudes of the shorter way between the longitudes, west larger than east when it
    ///         crosses the antimeridian
    static std::pair<double, double> longitudeSpan(double longitude1, double longitude2);

    bool isValid() const { return !qIsNaN(_south); }
    double south() const { return _south; }
    double north() const { return _north; }
    double west() const { return _west; }
    /// Continuous, larger than 180 degrees when the box crosses the antimeridian
    double east() const { return _east; }
    /// Degrees of longitude
    double width() const { return _east - _west; }
    double height() const { return _north - _south; }
    bool crossesAntimeridian() const { return _east > 180; }
    QGeoCoordinate center() const;

    bool contains(const QGeoCoordinate &coordinate) const;
    bool intersects(const GeoZoneBox &other) const;
    /// @return Smallest box containing both boxes, going the shorter way around
    GeoZoneBox united(const GeoZoneBox &other) const;

    /// @return Degrees of longitude from the longitude to the box the shorter way around, 0 inside
    double longitudeDistance(double longitude) const;
    /// @return Approximate horizontal distance in meters from the coordinate to the box, 0 inside, infinity when
    ///         either is invalid. Measured like in an equirectangular projection around the coordinate.
    double distanceTo(const QGeoCoordinate &coordinate) const;

    /// @return The box as a QGeoRectangle, which crosses the antimeridian when its west longitude is larger than its
    ///         east one, for QML and maps. Invalid for an invalid box.
    QGeoRectangle toRectangle() const;

    friend bool operator==(const GeoZoneBox &a, const GeoZoneBox &b)
    {
        return (!a.isValid() && !b.isValid()) ||
               ((a._south == b._south) && (a._north == b._north) && (a._west == b._west) && (a._east == b._east));
    }

private:
    double _south = qQNaN();
    double _north = qQNaN();
    double _west = qQNaN();
    double _east = qQNaN();
};
