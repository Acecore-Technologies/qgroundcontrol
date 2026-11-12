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
#include <QtCore/QtNumeric>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

/// The area of interest of the services taking one, see GeoZoneService::areaOfInterest: the bounds of the planned
/// missions, the surroundings of the active vehicle, and the view of the map shown. Their feeds are downloaded for it
/// with a margin around, and again when it leaves the area downloaded while it is followed.
///
/// The surroundings of the vehicle look ahead: its position and the position it reaches on its course in
/// lookAheadSecs, with the distance it flies in that time around them, at least minLookAheadMeters. So the zones are
/// downloaded again that far before the vehicle reaches the edge of the area downloaded, in time for the download and
/// for the alert of a zone beyond it, about 1 km at 50 m/s.
///
/// The view of the map shown last is cut to maxViewMeters around its center. It only joins the missions and the vehicle
/// when they all fit in maxViewMeters and in the area the service answers, so that looking at the map elsewhere never
/// replaces the zones around the mission and the vehicle.
///
/// For a service answering areas up to a size, the area requested never exceeds it: the margin and the look-ahead
/// shrink together to fit, then the view is left out, then the missions (cut), so that the zones around the vehicle
/// are always requested.
class GeoZoneAreaOfInterest
{
public:
    static constexpr double margin = 5000;  ///< Meters downloaded around the area of interest
    static constexpr double maxViewMeters = 200000;     ///< Widest and highest map view taken, larger views are cut around their center
    static constexpr double lookAheadSecs = 90;         ///< Flight time the surroundings of the vehicle look ahead
    static constexpr double minLookAheadMeters = 2000;  ///< Shortest look-ahead, that of a vehicle slower than 22 m/s
    static constexpr double maxSpeed = 100;             ///< Meters per second, faster reported speeds are taken as this
    /// Smallest share of the margin and the look-ahead kept for a service answering small areas before the view, then
    /// the missions, are left out for the vehicle
    static constexpr double minVehicleScale = 0.25;

    /// @param owner Identifies the mission
    /// @param area Invalid when the mission has no flight path
    /// @return true when the area of the mission was set or changed, rather than removed or kept
    bool setMissionArea(const void *owner, const QGeoRectangle &area);
    bool hasMissionArea(const void *owner) const { return _missionAreas.contains(owner); }
    /// @param position Invalid without vehicle. Positions out of range or at 0, 0 count as unknown.
    /// @param groundSpeed Meters per second, 0 when unknown
    /// @param course Direction of the flight over the ground in degrees from north, NaN when unknown
    /// @return true when the position is known
    bool setVehiclePosition(const QGeoCoordinate &position, double groundSpeed = 0, double course = qQNaN());
    /// @return Meters around the vehicle whose zones must be known: those it flies in lookAheadSecs, at least
    ///         minLookAheadMeters
    double lookAheadMeters() const;
    /// @param area Visible region of the map shown last, invalid to forget it
    /// @return true when the view taken changed
    bool setViewArea(const QGeoRectangle &area);
    QGeoRectangle viewArea() const { return _viewArea; }
    /// When false, the map view is left out of the area of interest
    bool followView() const { return _followView; }
    /// @return true when the area of interest changed
    bool setFollowView(bool follow);
    /// @return Area of interest of the services answering areas up to maxAreaKm2 (0 without limit): the bounds of the
    ///         planned missions and the surroundings of the vehicle, with the map view when they fit together, invalid
    ///         without any of them. For a service answering a smaller area, the part of it requested, see missionsCut.
    QGeoRectangle interest(double maxAreaKm2 = 0) const;
    /// @return true when a service answering areas up to maxAreaKm2 can't answer the missions with the vehicle: the
    ///         area of interest is then around the vehicle only, or around the center of the missions without vehicle
    bool missionsCut(double maxAreaKm2) const;
    /// @return Bounds of the planned missions and the position of the vehicle, invalid without either
    QGeoRectangle missionsAndVehicle() const;
    bool hasVehicle() const { return _vehiclePosition.isValid(); }
    /// @return interest() of services without area limit
    QGeoRectangle area() const { return interest(); }

    /// When false, the feeds keep the area they were downloaded for
    bool follow() const { return _follow; }
    void setFollow(bool follow) { _follow = follow; }

    /// @return Area to download a feed for: the area it was downloaded for while that covers the area of interest or
    ///         isn't followed, otherwise the area of interest with the margin, which shrinks for services answering
    ///         areas up to maxAreaKm2 (0 without limit) and never exceeds it
    QGeoRectangle requestArea(const QGeoRectangle &downloaded, double maxAreaKm2) const;

    /// @return The area with a margin around it
    static QGeoRectangle expanded(const QGeoRectangle &area, double meters);
    /// @return The area cut around its center to at most the width and height in meters
    static QGeoRectangle cropped(const QGeoRectangle &area, double maxMeters);
    /// @return Larger of the width and height of the area in meters
    static double spanMeters(const QGeoRectangle &area);

private:
    /// What is requested from a service
    struct Plan {
        QGeoRectangle interest;     ///< Area which must be downloaded
        QGeoRectangle request;      ///< The interest with the margin, which a download asks for
        bool missionsCut = false;
    };

    /// @return What is requested from a service answering areas up to maxAreaKm2, 0 without limit
    Plan _plan(double maxAreaKm2) const;
    /// @return Bounds of the planned missions, invalid without
    QGeoRectangle _missions() const;
    /// @return The position of the vehicle and the one it reaches on its course, with the look-ahead around them,
    ///         both by the scale, invalid without vehicle
    QGeoRectangle _vehicleArea(double scale) const;

    QHash<const void*, QGeoRectangle> _missionAreas;
    QGeoRectangle _viewArea;    ///< Cut to maxViewMeters
    QGeoCoordinate _vehiclePosition;
    double _vehicleSpeed = 0;   ///< Meters per second over the ground, 0 when unknown
    double _vehicleCourse = qQNaN();    ///< Degrees from north, NaN when unknown
    bool _follow = true;
    bool _followView = true;
};
