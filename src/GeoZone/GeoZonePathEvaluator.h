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
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtPositioning/QGeoCoordinate>

#include <functional>

#include "GeoZone.h"
#include "GeoZoneCatalog.h"
#include "GeoZonePolicy.h"

/// Evaluates flight paths against the zones of a catalog snapshot: where each path enters and leaves the zones, when,
/// and how its altitudes compare with their vertical limits. The mission check, altitude profile, guided command
/// checks and route planner all use it, so that they agree.
/// The evaluator is a plain value without settings or singletons. Evaluations don't change it and give the same result
/// for the same input, so a copy can run on any thread, unless the policy excludes authorised zones or the terrain
/// lookup needs the GUI thread.
class GeoZonePathEvaluator
{
    Q_DECLARE_TR_FUNCTIONS(GeoZonePathEvaluator)

public:
    /// @return Ground elevation in meters AMSL at the coordinate, NaN when unknown
    using TerrainLookup = std::function<double(const QGeoCoordinate &coordinate)>;

    static constexpr double defaultTerrainSpacing = 100;    ///< Meters between the positions added along paths with a terrain lookup
    static constexpr int defaultMaxTerrainPositions = 100;  ///< Positions of a path after adding the terrain, the spacing grows for long paths

    /// Datum of the altitudes of a path
    enum class AltitudeReference {
        MeanSeaLevel,
        Relative,       ///< Above Path::referenceAltitudeAMSL, like the home position
        Terrain,        ///< Above the ground, which must be known at the vertices
    };

    struct Vertex {
        QGeoCoordinate coordinate;
        double altitude = qQNaN();      ///< Meters in the datum of the path, NaN when unknown
        double terrainAMSL = qQNaN();   ///< Ground elevation, NaN to use the terrain lookup
        QDateTime time;                 ///< Estimated time the vehicle is at the vertex, invalid when unknown
    };

    /// Positions flown in order, along straight lines in latitude and longitude, with the altitudes, terrain and times
    /// interpolated linearly between them
    struct Path {
        QList<Vertex> vertices;
        AltitudeReference altitudeReference = AltitudeReference::MeanSeaLevel;
        double referenceAltitudeAMSL = qQNaN();     ///< Of relative altitudes
        QString label;                              ///< What the vehicle does along the path, empty for the main flight path

        /// @return Path through the positions, with altitudes above mean sea level
        static Path fromPositions(const QList<GeoZone::Position> &positions, const QString &label = QString());

        /// Sets the times of the vertices from start to end in proportion to the distance along the path, all to start
        /// without end, and all invalid without start
        void setTimes(const QDateTime &start, const QDateTime &end);
    };

    /// Point on a path, its vertex indexes refer to the input path even where terrain positions are added
    struct PathPoint {
        int path = 0;                   ///< Index of the path
        qsizetype segment = 0;          ///< From vertex segment towards vertex segment + 1, 0 for single vertex paths
        double fraction = 0;            ///< Along the segment, 0 at its start
        double distance = 0;            ///< Meters along the path from its start
        GeoZone::Position position;     ///< Interpolated, with the altitude above mean sea level
        QDateTime time;                 ///< Interpolated estimated time, invalid when unknown
    };

    /// Part of a horizontal stretch where the path keeps its altitude relative to the vertical limits of the volume
    struct VerticalPart {
        PathPoint from;
        PathPoint to;
        GeoZone::VerticalPosition position = GeoZone::VerticalPosition::Inside;
    };

    /// Stretch of a path inside the horizontal projection of a volume, whatever its altitude
    struct Stretch {
        int volume = 0;                 ///< Index into the volumes of the zone
        PathPoint entry;
        PathPoint exit;
        QList<VerticalPart> parts;      ///< From the entry to the exit
        bool active = true;             ///< See Crossing::active
    };

    /// Stretch of a path inside the zone, horizontally and between the vertical limits of any of its volumes
    struct Crossing {
        PathPoint entry;
        PathPoint exit;
        /// The zone applies during the crossing: the policy accepts zones whatever their schedule, or the zone is active
        /// at some time between the estimated times of the entry and the exit, or, without times, as the policy's
        /// schedule accepts it at the evaluation time
        bool active = true;
    };

    /// How the paths and areas interact with a zone
    struct ZoneResult {
        const GeoZone *zone = nullptr;
        QList<Crossing> crossings;      ///< By path and distance
        QList<Stretch> stretches;       ///< By path, distance and volume
        QList<int> areas;               ///< Indexes of the areas overlapping the zone horizontally and vertically
        bool areasActive = true;        ///< The zone applies during the flight of the areas, see Crossing::active
        /// Labels of the paths entering the zone while it applies, only when neither an unlabelled path nor an area does,
        /// like "the loiter at item 3"
        QStringList labels;
        int severity = 0;               ///< See GeoZone::restrictionSeverity

        /// @return true if a path or an area enters the zone, whether or not it applies then
        bool entered() const { return !crossings.isEmpty() || !areas.isEmpty(); }
        /// @return true if a path or an area enters the zone while it applies, or, when none enters it, if a path is
        ///         inside its horizontal projection while it applies
        bool active() const;
        /// @return First crossing while the zone applies, or the first one, null without crossings
        const Crossing *firstCrossing() const;
    };

    struct Result {
        /// Zones a path or an area interacts with, ordered by their first crossing, then the zones only entered by
        /// areas, then the zones only passed above or below
        QList<ZoneResult> zones;

        /// @return The result of the zone, null when the paths and areas don't interact with it
        const ZoneResult *find(const GeoZone *zone) const;
        /// @return The zones entered while they apply, in the order of zones
        QList<const GeoZone*> activeEnteredZones() const;
    };

    /// @param policy Restrictions, authorisations and schedule of the zones to evaluate. Authorisations are only
    ///               looked up on the GUI thread.
    /// @param time   Evaluation time, for zones active now and paths without times. When invalid, zones active now are
    ///               those marked active, see GeoZone::active.
    GeoZonePathEvaluator(const GeoZoneCatalog &catalog, const GeoZonePolicy &policy, const QDateTime &time = QDateTime());

    /// Sets the ground elevations for vertices without terrain, which take the previous vertex's elevation where the
    /// lookup has none, and positions added between the vertices every spacing meters where it has, so that limits
    /// above ground follow the terrain. The lookup is called from the evaluating thread.
    void setTerrainLookup(const TerrainLookup &lookup, double spacing = defaultTerrainSpacing, int maxPositions = defaultMaxTerrainPositions);

    /// @return How the paths and the areas, like mission patterns, interact with the zones accepted by the policy
    Result evaluate(const QList<Path> &paths, const QList<GeoZone::Area> &areas = {}) const;

    /// @return How the paths interact with the zone, whatever the policy's restrictions and authorisations. The zone
    ///         is null when they don't.
    ZoneResult evaluateZone(const GeoZone *zone, const QList<Path> &paths) const;

    /// @return The path with the terrain elevation at the positions without one, or the elevation of the previous
    ///         position where the lookup has none, and with positions added every spacing meters between them where it
    ///         has, with interpolated altitudes
    static QList<GeoZone::Position> withTerrain(const QList<GeoZone::Position> &path, const TerrainLookup &lookup, double spacing = defaultTerrainSpacing,
                                                int maxPositions = defaultMaxTerrainPositions);

    /// Sorts zones from the most restrictive, keeping the order of equally restrictive ones
    static void sortBySeverity(QList<const GeoZone*> &zones);

    /// @return The names of up to maxListed zones, with their restriction when withRestriction, and how many more
    static QString zoneNames(const QList<const GeoZone*> &zones, int maxListed, bool withRestriction);

private:
    struct Prepared;

    QList<Prepared> _prepare(const QList<Path> &paths) const;
    ZoneResult _evaluateZone(const GeoZone *zone, const QList<Prepared> &paths, const QList<std::pair<int, qsizetype>> &segments) const;
    bool _activeDuring(const GeoZone *zone, const QDateTime &start, const QDateTime &end) const;
    /// @return true if the policy accepts the restriction of the zone and doesn't exclude it as authorised
    bool _accepts(const GeoZone *zone) const;

    GeoZoneCatalog _catalog;
    GeoZonePolicy _policy;
    QDateTime _time;
    TerrainLookup _terrainLookup;
    double _terrainSpacing = defaultTerrainSpacing;
    int _maxTerrainPositions = defaultMaxTerrainPositions;
};
