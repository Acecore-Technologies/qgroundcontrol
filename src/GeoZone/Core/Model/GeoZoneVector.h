/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QPointF>

#include <cmath>

/// Planar vector helpers shared by the geozone geometry, on points of a local projection in meters
namespace GeoZoneVector {

inline double dot(const QPointF &a, const QPointF &b)
{
    return (a.x() * b.x()) + (a.y() * b.y());
}

/// @return z component of the cross product, positive when b turns counterclockwise from a
inline double cross(const QPointF &a, const QPointF &b)
{
    return (a.x() * b.y()) - (a.y() * b.x());
}

inline double length(const QPointF &vector)
{
    return std::hypot(vector.x(), vector.y());
}

/// @return The vector scaled to length 1, null for a null vector
inline QPointF unit(const QPointF &vector)
{
    const double vectorLength = length(vector);
    return (vectorLength > 0) ? (vector / vectorLength) : QPointF();
}

/// @return Sign of the turn from a->b to a->c, 0 when collinear within the tolerance
inline int orientation(const QPointF &a, const QPointF &b, const QPointF &c, double tolerance = 1e-12)
{
    const double value = cross(b - a, c - a);
    if (std::abs(value) < tolerance) {
        return 0;
    }
    return (value > 0) ? 1 : -1;
}

} // namespace GeoZoneVector
