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
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

/// The area of interest of the services taking one, see GeoZoneService::areaOfInterest: the bounds of the planned
/// missions and the position of the active vehicle. Their feeds are downloaded for it with a margin around, and again
/// when it leaves the area downloaded while it is followed.
class GeoZoneAreaOfInterest
{
public:
    static constexpr double margin = 5000;  ///< Meters downloaded around the area of interest

    /// @param owner Identifies the mission
    /// @param area Invalid when the mission has no flight path
    /// @return true when the area of the mission was set or changed, rather than removed or kept
    bool setMissionArea(const void *owner, const QGeoRectangle &area);
    /// @param position Invalid without vehicle. Positions out of range or at 0, 0 count as unknown.
    /// @return true when the position is known
    bool setVehiclePosition(const QGeoCoordinate &position);
    /// @return Bounds of the planned missions and the vehicle, invalid without either
    QGeoRectangle area() const;

    /// When false, the feeds keep the area they were downloaded for
    bool follow() const { return _follow; }
    void setFollow(bool follow) { _follow = follow; }

    /// @return Area to download a feed for: the area it was downloaded for while that covers the area of interest or
    ///         isn't followed, otherwise the area of interest with the margin, which shrinks for services answering
    ///         areas up to maxAreaKm2 (0 without limit)
    QGeoRectangle requestArea(const QGeoRectangle &downloaded, double maxAreaKm2) const;

    /// @return The area with a margin around it
    static QGeoRectangle expanded(const QGeoRectangle &area, double meters);

private:
    QHash<const void*, QGeoRectangle> _missionAreas;
    QGeoCoordinate _vehiclePosition;
    bool _follow = true;
};
