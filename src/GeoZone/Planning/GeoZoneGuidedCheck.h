/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZoneReturnModel.h"

class GeoZoneMissionPath;
class GeoZoneMonitor;
class MissionController;
class Vehicle;

/// Checks of the guided commands of the Fly View, each returning a warning naming the zones accepted by
/// GeoZonePolicy::alerts and active now which the vehicle would enter, or an empty string. Zones authorised now and
/// zones the vehicle is in are left out, unless it would leave and enter them again. See GeoZonePathEvaluator.
/// Limits above ground use the terrain along the path from GeoZoneTerrain where it is loaded, and the terrain below
/// the vehicle elsewhere, see GeoZoneMonitor::vehiclePosition. Returns follow the vehicle's return parameters, see
/// GeoZoneReturnModel, and missions the flight path of GeoZoneMissionPath.
class GeoZoneGuidedCheck : public QObject
{
    Q_OBJECT

public:
    explicit GeoZoneGuidedCheck(GeoZoneMonitor *monitor);

    /// @param vehicle Vehicle object, the checks return an empty string without a position
    /// @param altitudeAMSL Target altitude in meters above mean sea level, NaN to keep the vehicle's altitude
    Q_INVOKABLE QString gotoWarning(QObject *vehicle, const QGeoCoordinate &coordinate, double altitudeAMSL = qQNaN()) const;
    /// Flying to and around the circle
    Q_INVOKABLE QString orbitWarning(QObject *vehicle, const QGeoCoordinate &center, double radius, double altitudeAMSL) const;
    /// Climbing or descending at the vehicle's position, also for takeoffs
    /// @param relativeAltitude Target altitude in meters above the vehicle's home
    Q_INVOKABLE QString altitudeWarning(QObject *vehicle, double relativeAltitude) const;
    /// Returning to launch from the vehicle's position with its return parameters, to the destinations on the vehicle,
    /// see returnDestinations, and along its mission when the return follows it
    /// @param missionController The Fly View's, with the mission on the vehicle, null without
    Q_INVOKABLE QString returnWarning(QObject *vehicle, QObject *missionController) const;
    /// Landing at the vehicle's position: the descent to the ground below it, or to the home altitude where the terrain
    /// is unknown
    Q_INVOKABLE QString landWarning(QObject *vehicle) const;
    /// Flying the mission from the item on: from the vehicle's position to the item, then along the mission
    /// @param missionController The Fly View's, with the mission on the vehicle
    /// @param sequenceNumber Of the item, like the current item set or the item a mission resumes at
    /// @param fromGround The vehicle climbs to the altitude of the item first, like a resumed mission taking off
    Q_INVOKABLE QString missionWarning(QObject *vehicle, QObject *missionController, int sequenceNumber, bool fromGround = false) const;

    static constexpr int maxListedZones = 4;

    /// @return Warning naming the zones the path enters after its start, empty without
    QString pathWarning(const QList<GeoZone::Position> &path) const;
    QString pathWarning(const GeoZonePathEvaluator::Path &path) const;
    /// @return Warning naming the zones the paths enter, empty without. The vehicle starts on the first path: zones it
    ///         is in only count where the first path leaves and enters them again.
    /// @param subject What flies the paths, like "The return to launch", the vehicle when empty
    QString pathsWarning(const QList<GeoZonePathEvaluator::Path> &paths, const QString &subject = QString()) const;

    /// @return Where the vehicle returns to: its home position, the rally points on it and the landing sequences and
    ///         the first waypoint of the mission
    static GeoZoneReturnModel::Destinations returnDestinations(Vehicle *vehicle, MissionController *missionController);
    /// @return The paths of the return, the vehicle starting on the first. Returns along the mission follow its flight
    ///         path from or back from the item with the sequence number the vehicle flies to, and returns to the mission
    ///         landing the landing sequence. A vehicle landing where it is descends to groundAMSL.
    static QList<GeoZonePathEvaluator::Path> returnPaths(const GeoZoneReturnModel::Path &path, const GeoZoneReturnModel::Start &start,
                                                         const GeoZoneReturnModel::Destinations &destinations, const GeoZoneMissionPath &mission,
                                                         int sequenceNumber, double groundAMSL);
    /// @return The paths of the mission from the item with the sequence number on, the flight path from the item first,
    ///         without vertices when the item has none, and no paths when the mission has no item there. Items without a
    ///         coordinate are flown from the item with a coordinate before them.
    static QList<GeoZonePathEvaluator::Path> missionPaths(const GeoZoneMissionPath &mission, int sequenceNumber);
    /// @return Descent from the position to the ground at groundAMSL, which is NaN when unknown
    static GeoZonePathEvaluator::Path landingPath(const GeoZone::Position &from, double groundAMSL);

private:
    /// @return Index of the item of the mission with the sequence number, or of the item with a coordinate before it,
    ///         -1 without
    static int _itemIndex(const GeoZoneMissionPath &mission, int sequenceNumber);
    /// @return Positions of the mission's flight path flown between the distances along it
    static QList<GeoZone::Position> _flightPathPositions(const GeoZoneMissionPath &mission, double from, double to);

    GeoZoneMonitor *_monitor = nullptr;
};
