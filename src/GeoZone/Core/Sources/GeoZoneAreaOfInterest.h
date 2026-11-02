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
/// missions and the position of the active vehicle, and the view of the map shown. Their feeds are downloaded for it
/// with a margin around, and again when it leaves the area downloaded while it is followed.
///
/// The view of the map shown last is cut to maxViewMeters around its center. It only joins the missions and the vehicle
/// when they all fit in maxViewMeters and in the area the service answers, so that looking at the map elsewhere never
/// replaces the zones around the mission and the vehicle.
class GeoZoneAreaOfInterest
{
public:
    static constexpr double margin = 5000;  ///< Meters downloaded around the area of interest
    static constexpr double maxViewMeters = 200000;     ///< Widest and highest map view taken, larger views are cut around their center

    /// @param owner Identifies the mission
    /// @param area Invalid when the mission has no flight path
    /// @return true when the area of the mission was set or changed, rather than removed or kept
    bool setMissionArea(const void *owner, const QGeoRectangle &area);
    /// @param position Invalid without vehicle. Positions out of range or at 0, 0 count as unknown.
    /// @return true when the position is known
    bool setVehiclePosition(const QGeoCoordinate &position);
    /// @param area Visible region of the map shown last, invalid to forget it
    /// @return true when the view taken changed
    bool setViewArea(const QGeoRectangle &area);
    QGeoRectangle viewArea() const { return _viewArea; }
    /// When false, the map view is left out of the area of interest
    bool followView() const { return _followView; }
    /// @return true when the area of interest changed
    bool setFollowView(bool follow);
    /// @return Area of interest of the services answering areas up to maxAreaKm2 (0 without limit): the bounds of the
    ///         planned missions and the vehicle, with the map view when they fit together, invalid without any of them
    QGeoRectangle interest(double maxAreaKm2 = 0) const;
    /// @return interest() of services without area limit
    QGeoRectangle area() const { return interest(); }

    /// When false, the feeds keep the area they were downloaded for
    bool follow() const { return _follow; }
    void setFollow(bool follow) { _follow = follow; }

    /// @return Area to download a feed for: the area it was downloaded for while that covers the area of interest or
    ///         isn't followed, otherwise the area of interest with the margin, which shrinks for services answering
    ///         areas up to maxAreaKm2 (0 without limit)
    QGeoRectangle requestArea(const QGeoRectangle &downloaded, double maxAreaKm2) const;

    /// @return The area with a margin around it
    static QGeoRectangle expanded(const QGeoRectangle &area, double meters);
    /// @return The area cut around its center to at most the width and height in meters
    static QGeoRectangle cropped(const QGeoRectangle &area, double maxMeters);
    /// @return Larger of the width and height of the area in meters
    static double spanMeters(const QGeoRectangle &area);

private:
    /// @return Bounds of the planned missions and the vehicle, invalid without either
    QGeoRectangle _missionsAndVehicle() const;

    QHash<const void*, QGeoRectangle> _missionAreas;
    QGeoRectangle _viewArea;    ///< Cut to maxViewMeters
    QGeoCoordinate _vehiclePosition;
    bool _follow = true;
    bool _followView = true;
};
