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

/// What the fence storage of a vehicle holds. These are firmware limits which the vehicle enforces when a fence is
/// uploaded, and which it doesn't report.
/// PX4 stores DM_KEY_FENCE_POINTS_MAX items in its dataman, 64, of which one kept the statistics before v1.15, and 16 on
/// flight controllers with constrained memory, which can't be told from the others. It rejects more with
/// MAV_MISSION_NO_SPACE.
/// ArduPilot limits the items to its fence storage less a 4 byte header, divided by 8 (MAV_MISSION_NO_SPACE beyond),
/// and stores each polygon as a type and vertex count byte plus 8 bytes per vertex, each circle in 13 bytes and the
/// return point in 9 bytes, after the header and before an end marker (AC_PolyFence_loader), rejecting fences which
/// need more bytes with MAV_MISSION_ERROR ("Fence exceeds storage size"). Its storage size depends on the storage of
/// the flight controller and the vehicle type (StorageManager), see ArduPilotStorage, so it is estimated unless the
/// fence is stored on the SD card (BRD_SD_FENCE).
struct Capacity {
    int maxItems = 0;               ///< Fence items, 0 without limit
    int storageBytes = 0;           ///< ArduPilot fence storage, 0 when only the items are limited
    int maxPolygonVertices = 0;     ///< Per polygon, 0 without limit
    bool circles = true;            ///< Stores fence circles, otherwise they are replaced by polygons
    bool exclusions = true;         ///< Stores exclusion fences
    bool estimated = false;         ///< Assumed, since the vehicle doesn't report its storage: it may store fewer
    bool reduced = false;           ///< Lowered after the vehicle rejected a fence for lack of room, see reducedBelow

    /// @return true if the vehicle can store a fence with the usage
    bool fits(const Usage &usage) const;

    /// @return The next capacity to try after the vehicle rejected a fence with the usage for lack of room: that of the
    ///         next smaller storage known for the autopilot which doesn't hold the fence, or one item less than the
    ///         fence without one. Estimated.
    Capacity reducedBelow(const Usage &rejected) const;

    static constexpr int px4MaxItems = 64;                  ///< PX4 dataman fence items (DM_KEY_FENCE_POINTS_MAX), from v1.15
    static constexpr int px4LegacyMaxItems = 63;            ///< Before v1.15, whose first item kept the statistics
    static constexpr int px4ConstrainedMaxItems = 16;       ///< Flight controllers with constrained memory
    static constexpr int px4FullItemsMajor = 1;             ///< First version storing px4MaxItems
    static constexpr int px4FullItemsMinor = 15;
    static constexpr int arduPilotMaxPolygonVertices = 255; ///< ArduPilot stores the vertex count of a polygon in a byte
    static constexpr int arduPilotHeaderBytes = 4;          ///< ArduPilot fence storage format header
    static constexpr int arduPilotFixedBytes = arduPilotHeaderBytes + 1;    ///< With the end marker
    static constexpr int arduPilotItemBytes = 8;            ///< Storage ArduPilot counts per item for its item limit
    static constexpr int arduPilotMaxBytes = 65535;         ///< ArduPilot addresses fence storage with 16 bits

    /// Fence storage of ArduPilot's storage layouts, by the storage of the flight controller
    enum class ArduPilotStorage {
        Small,      ///< 4 KB: 5 items for copters, 19 for the others
        Common,     ///< 8 KB, or 15 KB without CAN: 37 and 51 items. The smallest of the flight controllers in common use.
        Large,      ///< 16 KB and more, or 15 KB with CAN: 69 and 83 items
    };
    /// @param copter Copter and helicopter builds have less fence storage than the others
    static int arduPilotBytes(bool copter, ArduPilotStorage storage);
    static constexpr int arduPilotCopterBytes = 304;        ///< Assumed for copters and helicopters: ArduPilotStorage::Common
    static constexpr int arduPilotOtherBytes = 416;         ///< Assumed for planes, rovers and subs

    /// @param majorVersion Of the firmware, with minorVersion, negative when unknown: 63 items are then assumed, estimated
    static Capacity px4(int majorVersion = -1, int minorVersion = -1);
    /// @param copter Copter and helicopter builds have less fence storage than the others
    /// @param sdCardKB Fence storage on the SD card (BRD_SD_FENCE), used instead of the board's when positive. Otherwise
    ///                 the storage is estimated
    /// @param storage of the flight controller, which those with less don't have
    static Capacity arduPilot(bool copter, int sdCardKB = 0, ArduPilotStorage storage = ArduPilotStorage::Common);
    /// Fits PX4 of any version and ArduPilot copters with ArduPilotStorage::Common, without fence circles, for other or
    /// unknown autopilots. Estimated.
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
    /// Active only part of the planned flight. Vehicle fences don't follow schedules, so such zones aren't fenced:
    /// a zone becoming active while the vehicle is inside must only be warned about, not make the vehicle act.
    bool partTime = false;
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
    PartTime,       ///< Active only part of the planned flight, see Candidate::partTime
};

struct Choice {
    Candidate candidate;
    Status status = Status::Selected;
    ZoneFences fences;              ///< Of selected zones
    double simplifyTolerance = 0;   ///< Of the fences
    Usage usage;                    ///< Of the fences, or at the largest tolerance for zones left out
    const GeoZone *duplicateOf = nullptr;   ///< The zone before it in priority with the same fences
    bool duplicateSelected = false;         ///< duplicateOf is selected, so that its fences cover the zone

    /// @return false if the zone can't be included whatever the user chooses: it adds no fence of its own, since it
    ///         has no area, has the same area as a zone before it, which it follows, or the vehicle stores no
    ///         exclusion fences, or it is active only part of the planned flight
    bool includable() const { return (status != Status::NoShape) && (status != Status::Duplicate) && (status != Status::Unsupported) && (status != Status::PartTime); }
    /// @return true if the fence excludes the zone: it is selected, or a duplicate of a selected zone
    bool fenced() const { return (status == Status::Selected) || ((status == Status::Duplicate) && duplicateSelected); }
};

struct Selection {
    QList<Choice> choices;          ///< Selected first, then those left out, each in priority order
    Usage usage;                    ///< Of the selected fences and the reserved ones
    Usage reserved;

    QList<const GeoZone*> selectedZones() const;
    /// @return The choices whose zones the fence doesn't exclude, see Choice::fenced, in priority order
    QList<Choice> notFenced() const;
};

/// @return The candidates in priority order: zones active during the whole planned flight before those active only part
///         of it, then prohibited zones, other restricted ones (GeoZonePolicy::isRestricted), then the others; by
///         proximity within those, by distance for far zones, then the most restrictive ones and those with fewer
///         fence items at the largest simplification
QList<Candidate> sortedByPriority(const QList<Candidate> &candidates, const QList<double> &simplifyTolerances = { 0 }, bool circlesAsPolygons = false);

/// Candidates in priority order with the fences of their zones measured: what selections need besides the capacity,
/// the reserved fences and which candidates are excluded, so that selecting again is quick, like after the user
/// includes or leaves out a zone
struct Prepared {
    struct Entry {
        Candidate candidate;        ///< Its excluded flag may be changed between selections
        Usage usage;                ///< Of the zone's fences at the largest simplification tolerance
        size_t shapeHash = 0;       ///< Of the zone's unsimplified fences, to find duplicates
        bool hasShape = false;      ///< Importing the zone adds fences
    };

    QList<Entry> entries;                       ///< In priority order, see sortedByPriority
    QList<double> simplifyTolerances = { 0 };   ///< Ascending meters, 0 for unsimplified fences
    bool circlesAsPolygons = false;
};

/// @param simplifyTolerances Ascending meters, 0 for unsimplified fences
/// @param circlesAsPolygons For capacities without circles, see Capacity::circles
Prepared prepare(const QList<Candidate> &candidates, const QList<double> &simplifyTolerances = { 0 }, bool circlesAsPolygons = false);

/// Selects the zones whose fences fit into the capacity besides the reserved fences, in priority order, see
/// sortedByPriority. Zones are added while they fit at the largest simplification tolerance, then the selected zones
/// take the smallest tolerance which fits, in priority order. Simplified fences only grow, see zoneFences.
/// Zones with the same fences as a zone before them are duplicates, whether that zone is selected or not, so that
/// leaving out a zone leaves out its area.
/// @param simplifyTolerances Ascending meters, 0 for unsimplified fences
Selection select(const QList<Candidate> &candidates, const Capacity &capacity, const Usage &reserved = Usage(), const QList<double> &simplifyTolerances = { 0 });
/// Same as select with the prepared candidates, prepared for the circles of the capacity
Selection select(const Prepared &prepared, const Capacity &capacity, const Usage &reserved = Usage());

/// @return Why the zone of the choice is left out, empty for selected zones
QString statusText(const Choice &choice);

} // namespace GeoZoneFences
