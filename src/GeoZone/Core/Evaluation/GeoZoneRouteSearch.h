/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QList>
#include <QtCore/QStringList>
#include <QtPositioning/QGeoCoordinate>

#include <limits>
#include <memory>

#include "GeoZone.h"
#include "GeoZoneCancelToken.h"
#include "GeoZoneCatalog.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZonePolicy.h"

/// Finds how a straight leg between two positions can avoid the zones it enters: a horizontal detour around the zones
/// grown by a margin, or climbing over their ceilings or descending below their floors. Each way is checked with
/// GeoZonePathEvaluator and scored by its cost, the distance it adds plus weighted climbs and descents.
///
/// Detours are the shortest paths through a visibility graph of nodes around the convex corners of the zones grown by
/// the margin: nodes on the arcs of the margin around the corners, none at concave corners, and none within the margin
/// of other zones, which keeps the concave parts of zones, their holes and the gaps between them. They are found by A*
/// with the straight-line distance as heuristic, which tests the visibility of an edge only when it would reach a node
/// first. Only the zones a candidate path runs into are added to the graph, until the path keeps the margin from all
/// zones within reach of the length limit, which the catalog's index finds. Visibility tests are cached between these
/// rounds, a cell grid finds the zone edges near a segment, and zone outlines are only projected when a test comes near.
///
/// A search reads a catalog snapshot and keeps nothing between calls but the simplified zone outlines, so that it can
/// run on a worker thread, when the terrain lookup and the policy can. Use a search, or its copies, from one thread at
/// a time.
class GeoZoneRouteSearch
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneRouteSearch)

public:
    /// What routes keep from the zones, the terrain and the leg, and the weights of their costs
    struct Settings {
        double margin = 100;                ///< Meters detours keep from the zones horizontally
        double verticalMargin = 10;         ///< Meters climbs keep above ceilings and descents below floors
        double minGroundClearance = 10;     ///< Meters above the terrain that descents keep
        double maxAltitudeAGL = 120;        ///< Meters above the terrain that climbs may reach, NaN without limit
        double maxDetourFactor = 3;         ///< Detours may be this many times as long as the leg, or
        double minDetourAllowance = 3000;   ///< this many meters longer, whichever is more
        double climbWeight = 5;             ///< Cost of climbing a meter, in meters of horizontal flight
        double descentWeight = 3;           ///< Cost of descending a meter, in meters of horizontal flight
        double sampleSpacing = 25;          ///< Meters between the positions where climbs and descents are checked
    };

    /// Way of avoiding the zones
    enum class Kind {
        Detour,     ///< Around the zones at the altitudes of the leg
        Climb,      ///< Over the ceilings, raising the start or the end of the leg
        Descent,    ///< Under the floors, lowering the start or the end of the leg
    };

    /// Why a route isn't possible
    enum class Failure {
        None,
        Cancelled,
        StartInside,        ///< The leg starts inside the zones
        EndInside,          ///< The leg ends inside the zones
        Enclosed,           ///< The zones surround the start or the end
        TooLong,            ///< Detours around the zones are longer than the limit, see Route::limit
        NotFound,           ///< The zones are too many or too complex for a detour
        AltitudeUnknown,    ///< The altitudes of the leg aren't known
        NoCeiling,          ///< The zones have no ceiling, or none in meters, like flight levels
        ReachesGround,      ///< The zones extend to the ground
        NoFloor,            ///< The floors of the zones aren't in meters, like flight levels
        AboveMaxAltitude,   ///< Climbing over the zones exceeds the maximum height above ground, see Route::limit
        BelowMinClearance,  ///< Descending below the zones comes too close to the terrain, see Route::limit
        TerrainUnknown,     ///< The terrain along the leg isn't known
        EntersZone,         ///< The route enters other zones it can't avoid
    };

    /// Leg to route, in a straight line in latitude and longitude like GeoZonePathEvaluator::Path
    struct Leg {
        GeoZone::Position from;     ///< Altitude above mean sea level, NaN when unknown
        GeoZone::Position to;
        QDateTime start;            ///< Estimated times the vehicle flies the leg, for zone schedules, invalid when unknown
        QDateTime end;
    };

    /// Counters of a detour search
    struct Statistics {
        int rounds = 0;             ///< Searches of the graph, each adding the zones the previous path ran into
        int obstacles = 0;          ///< Zone volumes within reach of the length limit
        int includedObstacles = 0;  ///< Of them, those added to the graph
        int nodes = 0;              ///< Of the graph, with the start and the end
        int visibilityTests = 0;    ///< Segments tested against the zones
        int cachedVisibility = 0;   ///< Visibility answered by the cache, entirely or for some zones
    };

    /// Way around, over or under the zones, or why it isn't possible
    struct Route {
        Kind kind = Kind::Detour;
        Failure failure = Failure::None;
        QStringList zones;                  ///< Names of the zones avoided, or of those preventing the route
        QList<QGeoCoordinate> waypoints;    ///< Detours: between the start and the end of the leg
        double length = 0;                  ///< Detours: meters from the start to the end, a lower bound when TooLong
        double fromAltitude = qQNaN();      ///< Climbs and descents: new altitude AMSL of the start of the leg
        double toAltitude = qQNaN();        ///< Climbs and descents: new altitude AMSL of the end of the leg
        double altitude = qQNaN();          ///< Climbs and descents: meters AMSL just above the ceilings or below the floors
        /// TooLong: maximum length. AboveMaxAltitude: height above ground the climb needs. BelowMinClearance: smallest
        /// height above ground of the descent.
        double limit = qQNaN();
        double cost = std::numeric_limits<double>::infinity();  ///< See Settings
        Statistics statistics;              ///< Detours only

        bool isValid() const { return failure == Failure::None; }
    };

    struct Result {
        QStringList crossedZones;   ///< Names of the zones the leg enters while they apply, most restrictive first
        /// Detour, climb and descent, those possible first from the cheapest, then the others in that order. Empty when
        /// the leg enters no zone or the search was cancelled.
        QList<Route> routes;
        bool cancelled = false;

        /// @return The cheapest possible route, null without
        const Route *best() const { return (!routes.isEmpty() && routes.constFirst().isValid()) ? &routes.constFirst() : nullptr; }
    };

    /// @param policy Zones to avoid. To leave out authorised zones on another thread, its authorisations must not change.
    /// @param time   Evaluation time, see GeoZonePathEvaluator
    GeoZoneRouteSearch(const GeoZoneCatalog &catalog, const GeoZonePolicy &policy, const QDateTime &time = QDateTime());

    void setSettings(const Settings &settings) { _settings = settings; }
    const Settings &settings() const { return _settings; }
    /// Ground elevations for limits above ground and the clearance of descents, see GeoZonePathEvaluator::setTerrainLookup
    void setTerrainLookup(const GeoZonePathEvaluator::TerrainLookup &lookup);
    /// Stops searches in progress, which then return cancelled results
    void setCancelToken(const GeoZoneCancelToken &token) { _cancel = token; }

    /// @return The routes avoiding the zones the leg enters while they apply
    Result plan(const Leg &leg) const;

    /// @return The shortest detour of the leg keeping the margin from the zones accepted by the policy whose vertical
    ///         limits overlap the altitudes of the leg, checked with GeoZonePathEvaluator. Without waypoints when the
    ///         leg already keeps the margin.
    Route detour(const Leg &leg) const;

    /// @return The shortest detour from one coordinate to another keeping the margin from all volumes of the zones,
    ///         whatever their vertical limits, without the length limit. Without waypoints when the straight line
    ///         keeps the margin.
    static Route detourAround(const QGeoCoordinate &from, const QGeoCoordinate &to, const QList<const GeoZone*> &zones, const Settings &settings,
                              const GeoZoneCancelToken &cancel = GeoZoneCancelToken());

    /// @return Meters a detour may be long at most, see Settings::maxDetourFactor
    static double maxDetourLength(double directLength, const Settings &settings);

    /// Zone outlines the searches of a GeoZoneRouteSearch and its copies share
    struct Cache;

private:
    GeoZonePathEvaluator _evaluator() const;
    QList<GeoZone::Position> _samples(const Leg &leg) const;
    /// @return Zones the path enters while they apply
    QList<const GeoZone*> _enteredZones(const GeoZonePathEvaluator &evaluator, const QList<GeoZone::Position> &path, const Leg &leg) const;
    Route _detour(const Leg &leg, const QList<GeoZone::Position> &samples, const QList<const GeoZone*> &crossed) const;
    Route _vertical(Kind kind, const Leg &leg, const QList<GeoZone::Position> &samples, const QList<const GeoZone*> &crossed) const;

    GeoZoneCatalog _catalog;
    GeoZonePolicy _policy;
    QDateTime _time;
    Settings _settings;
    GeoZonePathEvaluator::TerrainLookup _terrainLookup;
    GeoZoneCancelToken _cancel;
    std::shared_ptr<Cache> _cache;
};
