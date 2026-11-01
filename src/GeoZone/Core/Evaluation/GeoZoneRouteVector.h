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

// Stand-in for the shared Core/Model/GeoZoneVector.h, with the same functions: include that header instead and remove
// this one once both are on the same branch.

/// Planar vector helpers of the route search, on points of a local projection in meters
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

} // namespace GeoZoneVector
