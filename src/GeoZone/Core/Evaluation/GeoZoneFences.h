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
#include <QtCore/QString>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZonePathEvaluator.h"

class GeoZone;

/// Exclusion fences of zones, as GeoZoneFenceImporter adds them to a GeoFence, whether fences cover zones, and which
/// zones fit into the fence storage of a vehicle
namespace GeoZoneFences {

constexpr double boundaryTolerance = 1;     ///< Meters zones may extend past fences covering them, for rounding

/// Exclusion fences of a zone
struct ZoneFences {
    QList<QList<QGeoCoordinate>> polygons;
    QList<QGeoCircle> circles;

    bool isEmpty() const { return polygons.isEmpty() && circles.isEmpty(); }
    friend bool operator==(const ZoneFences &, const ZoneFences &) = default;
};

constexpr int minCircleVertices = 6;    ///< Of polygons replacing circles, see circlePolygon
constexpr int maxCircleVertices = 32;

/// @return Vertices of a polygon replacing the circle for vehicles without fence circles: enough for it to extend at
///         most twice the tolerance, or 2% of the radius when more, beyond the circle, within minCircleVertices and
///         maxCircleVertices
int circleVertexCount(double radius, double simplifyTolerance);

/// @return Clockwise polygon whose edges touch the circle from outside, so that it encloses the whole circle
QList<QGeoCoordinate> circlePolygon(const QGeoCircle &circle, int vertexCount);

/// @return The exclusion fences importing the zone adds: the outer boundaries of its polygon volumes, simplified
///         outward when the tolerance is positive, see GeoZone::exclusionPolygons, and its circles, as polygons
///         enclosing them with circlesAsPolygons, see circlePolygon
ZoneFences zoneFences(const GeoZone *zone, double simplifyTolerance = 0, bool circlesAsPolygons = false);

/// @return true if each of the fences importing the zone without simplification adds lies within one of the fences,
///         up to boundaryTolerance, so that the fences exclude the whole zone. Fences importing it with any
///         simplification add enclose those. False for zones importing adds no fence for.
bool fencesCover(const ZoneFences &fences, const GeoZone *zone);

/// @return Horizontal meters between the fences and the closest of the lines, like flight paths, 0 when a line enters
///         them, infinity without fences or lines. Lines of a single coordinate are points.
double distanceTo(const ZoneFences &fences, const QList<QList<QGeoCoordinate>> &lines);

/// Fence storage taken on a vehicle, as the MAVLink mission protocol and ArduPilot store fences
struct Usage {
    int items = 0;              ///< Mission items: one per polygon vertex, circle and return point
    int bytes = 0;              ///< ArduPilot storage without its header and end marker, see Capacity::arduPilotFixedBytes
    int largestPolygon = 0;     ///< Vertices of the largest polygon

    void addPolygon(qsizetype vertexCount);
    void addCircle();
    void addReturnPoint();
    void add(const ZoneFences &fences);
    Usage &operator+=(const Usage &other);
    friend Usage operator+(Usage usage, const Usage &other) { return usage += other; }
    friend bool operator==(const Usage &, const Usage &) = default;
};

/// What the fence storage of a vehicle holds. ArduPilot limits the items to its fence storage size divided by 8 and
/// stores each polygon as a type and vertex count byte plus 8 bytes per vertex, each circle in 13 bytes and the return
/// point in 9 bytes, after a 4 byte header and before an end marker (AC_PolyFence_loader). PX4 stores 64 items in its
/// dataman (DM_KEY_FENCE_POINTS_MAX).
struct Capacity {
    int maxItems = 0;               ///< Fence items, 0 without limit
    int storageBytes = 0;           ///< ArduPilot fence storage, 0 when only the items are limited
    int maxPolygonVertices = 0;     ///< Per polygon, 0 without limit
    bool circles = true;            ///< Stores fence circles, otherwise they are replaced by polygons
    bool exclusions = true;         ///< Stores exclusion fences
    bool estimated = false;         ///< Assumed, since the vehicle doesn't report its storage

    /// @return true if the vehicle can store a fence with the usage
    bool fits(const Usage &usage) const;

    static constexpr int px4MaxItems = 64;                  ///< PX4 dataman fence items (DM_KEY_FENCE_POINTS_MAX)
    static constexpr int arduPilotMaxPolygonVertices = 255; ///< ArduPilot stores the vertex count of a polygon in a byte
    static constexpr int arduPilotFixedBytes = 5;           ///< ArduPilot fence storage format header and end marker
    static constexpr int arduPilotCopterBytes = 560;        ///< Copter and helicopter fence storage of boards with 16 KB storage, 70 items
    static constexpr int arduPilotOtherBytes = 672;         ///< Plane, rover and sub fence storage of boards with 16 KB storage, 84 items
    static constexpr int arduPilotMaxBytes = 65535;         ///< ArduPilot addresses fence storage with 16 bits

    static Capacity px4();
    /// @param copter Copter and helicopter builds have less fence storage than the others
    /// @param sdCardKB Fence storage on the SD card (BRD_SD_FENCE), used instead of the board's when positive. Otherwise
    ///                 the storage is estimated for boards with 16 KB of storage, those with less store fewer items.
    static Capacity arduPilot(bool copter, int sdCardKB = 0);
    /// Fits both PX4 and ArduPilot copters with 16 KB storage, without fence circles, for other or unknown autopilots
    static Capacity conservative();
    /// Vehicles which don't support fences through the mission protocol
    static Capacity unsupported();
};

/// How close a planned flight comes to a zone, most relevant first
enum class Proximity {
    Entered,        ///< A path or an area enters the zone, between its vertical limits
    Overflown,      ///< A path crosses it horizontally, but only above or below its vertical limits
    Near,           ///< Within the margin horizontally
    Far,
};

/// Zone which may be added to the fence
struct Candidate {
    const GeoZone *zone = nullptr;
    Proximity proximity = Proximity::Far;
    double distance = qInf();       ///< Horizontal meters from the planned flight, infinity without a flight
    bool excluded = false;          ///< Left out by the user. Overflown zones are left out unless included.
};

constexpr double defaultProximityMargin = 1000;    ///< Meters around the planned flight zones are near

/// @return Candidates for the zones, with how close the evaluated paths and the lines, like the flight path and the
///         outlines of its areas, come to them
QList<Candidate> candidates(const QList<const GeoZone*> &zones, const GeoZonePathEvaluator::Result &evaluation, const QList<QList<QGeoCoordinate>> &lines,
                            double margin = defaultProximityMargin);

/// Whether and how a candidate is in a selection
enum class Status {
    Selected,
    Excluded,       ///< By the user, or overflown
    NoRoom,         ///< The zones before it fill the fence
    TooLarge,       ///< Its fences don't fit even alone, even simplified
    Unsupported,    ///< The vehicle doesn't store exclusion fences
    NoShape,        ///< The zone has no area, like a line
    Duplicate,      ///< Its fences are those of a zone before it
};

struct Choice {
    Candidate candidate;
    Status status = Status::Selected;
    ZoneFences fences;              ///< Of selected zones
    double simplifyTolerance = 0;   ///< Of the fences
    Usage usage;                    ///< Of the fences, or at the largest tolerance for zones left out
    const GeoZone *duplicateOf = nullptr;
};

struct Selection {
    QList<Choice> choices;          ///< Selected first, then those left out, each in priority order
    Usage usage;                    ///< Of the selected fences and the reserved ones
    Usage reserved;

    QList<const GeoZone*> selectedZones() const;
};

/// @return The candidates in priority order: prohibited zones, other restricted ones (GeoZonePolicy::isRestricted),
///         then the others; by proximity within those, by distance for far zones, then the most restrictive ones and
///         those with fewer fence items at the largest simplification
QList<Candidate> sortedByPriority(const QList<Candidate> &candidates, const QList<double> &simplifyTolerances = { 0 }, bool circlesAsPolygons = false);

/// Selects the zones whose fences fit into the capacity besides the reserved fences, in priority order, see
/// sortedByPriority. Zones are added while they fit at the largest simplification tolerance, then the selected zones
/// take the smallest tolerance which fits, in priority order. Simplified fences only grow, see zoneFences.
/// @param simplifyTolerances Ascending meters, 0 for unsimplified fences
Selection select(const QList<Candidate> &candidates, const Capacity &capacity, const Usage &reserved = Usage(), const QList<double> &simplifyTolerances = { 0 });

/// @return Why the zone of the choice is left out, empty for selected zones
QString statusText(const Choice &choice);

} // namespace GeoZoneFences
