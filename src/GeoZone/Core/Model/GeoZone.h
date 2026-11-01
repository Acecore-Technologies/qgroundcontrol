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
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QMutex>
#include <QtCore/QSharedPointer>
#include <QtCore/QRectF>
#include <QtCore/QStringList>
#include <QtCore/QTime>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include <cmath>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "GeoZoneBox.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneSchedule.h"

/// A UAS geographical zone loaded from a geozone source. Zones are plain objects owned by their source through
/// shared pointers, QML gets GeoZoneInfo values describing them. The class is a gadget for its enums.
class GeoZone : public QEnableSharedFromThis<GeoZone>
{
    Q_GADGET
    Q_DECLARE_TR_FUNCTIONS(GeoZone)
    Q_DISABLE_COPY_MOVE(GeoZone)

public:
    enum Restriction {
        Prohibited,
        ReqAuthorisation,
        Conditional,
        NoRestriction,
        USpace,
        Unknown,
    };
    Q_ENUM(Restriction)

    /// Datum of a vertical limit
    enum class VerticalReference {
        Unknown,        ///< Missing or unsupported, the limit can't be compared
        Ground,         ///< AGL or SFC
        MeanSeaLevel,   ///< AMSL or MSL
        Ellipsoid,      ///< WGS84
        FlightLevel,    ///< STD, pressure altitude converted with the QNH, see GeoZoneVertical::pressureAltitudeToAMSL
    };

    /// Altitude of a position relative to the vertical limits of a volume
    enum class VerticalPosition {
        Below,          ///< Under the floor
        Inside,         ///< Between the limits, or not comparable with them
        Above,          ///< Over the ceiling
    };

    /// Unclosed polygon boundary, x is longitude and y latitude in degrees. Much smaller than QGeoCoordinate vertices.
    /// The rings of a zone are continuous across the antimeridian, so their longitudes can exceed 180 degrees.
    using Ring = QList<QPointF>;

    static Ring toRing(const QList<QGeoCoordinate> &coordinates);
    static QList<QGeoCoordinate> toCoordinates(const Ring &ring);
    static QGeoCoordinate toCoordinate(const QPointF &vertex) { return QGeoCoordinate(vertex.y(), std::remainder(vertex.x(), 360.0)); }

    /// Horizontal projection plus vertical limits of one airspace volume of a zone
    struct Volume {
        bool isCircle = false;
        QList<Ring> rings;                  ///< Polygon only: [0] is the outer boundary, others are holes
        QGeoCoordinate center;              ///< Circle only
        double radius = 0;                  ///< Circle only, meters
        double lowerLimit = qQNaN();        ///< As published, in feet when \c feet is set. NaN when not specified.
        double upperLimit = qQNaN();
        VerticalReference lowerReference = VerticalReference::Unknown;
        VerticalReference upperReference = VerticalReference::Unknown;
        QString lowerReferenceCode;         ///< As published, for display
        QString upperReferenceCode;
        bool feet = false;

        /// Sets the references from the published codes
        void setReferences(const QString &lowerCode, const QString &upperCode);

        /// @return true if a limit is a flight level, whose height above mean sea level is approximated from the
        ///         QNH of GeoZoneEnvironment when it is compared or shown
        bool approximateLimits() const { return (lowerReference == VerticalReference::FlightLevel) || (upperReference == VerticalReference::FlightLevel); }

        /// Of the outer ring or the circle, invalid without them
        GeoZoneBox boundingBox() const;
    };

    /// Horizontal position with optional altitudes
    struct Position {
        QGeoCoordinate coordinate;
        double altitudeAMSL = qQNaN();  ///< NaN when unknown
        double terrainAMSL = qQNaN();   ///< Ground elevation for limits relative to the ground, NaN when unknown
    };

    using DailyPeriod = GeoZoneSchedule::DailyPeriod;
    using TimePeriod = GeoZoneSchedule::TimePeriod;

    struct Data {
        QString identifier;
        QString country;
        QString name;
        QString restrictionCode;    ///< Restriction/type value as published
        Restriction restriction = Unknown;
        QStringList reasons;
        QString otherReasonInfo;
        QString restrictionConditions;
        QString message;
        QStringList authorities;    ///< One human readable line per zone authority
        QList<TimePeriod> applicability;
        QList<Volume> volumes;
        QString uSpaceClass;            ///< ED-269/ED-318 U-space classification, as published
        QString regulationExemption;    ///< ED-269/ED-318 YES or NO, as published
        QStringList extendedProperties; ///< Publisher specific properties, one "name: value" line each
        QStringList issues;             ///< Problems found while reading the zone, translated, like an unknown unit
        QByteArray contentHash;         ///< Of the published feature, to recognize changed zones when a source reloads
    };  // Fields added here must be serialized, see operator<<

    explicit GeoZone(const Data &data);

    const Data &data() const { return _data; }

    QString identifier() const { return _data.identifier; }
    QString name() const { return _data.name; }
    QString displayName() const { return _data.name.isEmpty() ? _data.identifier : _data.name; }
    QString country() const { return _data.country; }
    Restriction restriction() const { return _data.restriction; }
    QString restrictionText() const { return restrictionToString(_data.restriction); }
    QString restrictionConditions() const { return _data.restrictionConditions; }
    QString reasonText() const;
    QString message() const { return _data.message; }
    QString authorityText() const { return _data.authorities.join(QStringLiteral("\n")); }
    QString uSpaceClass() const { return _data.uSpaceClass; }
    /// @return Whether the regulation allows exemptions, empty when not published
    QString regulationExemptionText() const;
    QString extendedPropertiesText() const { return _data.extendedProperties.join(QStringLiteral("\n")); }
    QString issuesText() const { return _data.issues.join(QStringLiteral("\n")); }
    QString applicabilityText() const;
    /// Limits of each volume on a line. Flight levels show their approximate height above mean sea level with the QNH.
    QString verticalLimitsText() const;
    /// @return true if the heights of some limits are approximate, see Volume::approximateLimits
    bool approximateLimits() const;
    QGeoCoordinate center() const { return _box.center(); }
    /// Bounds of all volumes, which may cross the antimeridian
    const GeoZoneBox &box() const { return _box; }
    /// Same as box, for QML and maps
    QGeoRectangle boundingBox() const { return _box.toRectangle(); }
    bool active() const { return _active.load(std::memory_order_relaxed); }
    QString sourceKey() const { return _sourceKey; }
    QString sourceText() const { return _sourceText; }
    /// Set by the source before the zone is published
    void setSourceKey(const QString &sourceKey);
    void setSourceText(const QString &sourceText);

    /// @return Identifies the zone across reloads of its source, from the source key, identifier, name and restriction
    const QString &key() const { return _key; }

    /// Re-evaluates the active state against the specified UTC time
    /// @return true if the state changed
    bool updateActive(const QDateTime &now);

    /// @return true if the zone applies at the specified time. Daylight events are computed at the zone center.
    bool isActiveAt(const QDateTime &dateTime) const;

    /// @return true if the zone applies at any time between start and end inclusive
    bool isActiveDuring(const QDateTime &start, const QDateTime &end) const;

    /// @return true if the schedule can't be fully evaluated, like daily periods from or to an unknown daylight event,
    ///         which are then taken from the start or to the end of the day, so the zone is assumed active longer
    bool scheduleAssumed() const { return _scheduleAssumed; }

    /// Outer boundaries of all polygon volumes, clockwise and without duplicates. Holes are dropped
    /// so that excluding these polygons is always conservative.
    /// @param simplifyTolerance Meters. When positive, polygons are reduced to fewer vertices which still enclose
    ///                          the zone, see GeoZoneGeometry::simplifyOutward.
    QList<QList<QGeoCoordinate>> exclusionPolygons(double simplifyTolerance = 0) const;

    /// Circle volumes without duplicates
    QList<QGeoCircle> exclusionCircles() const;

    /// @return true if the position is inside a volume. Unknown altitudes are inside all vertical limits.
    bool contains(const Position &position) const;
    bool contains(const QGeoCoordinate &coordinate) const { return contains(Position{ coordinate }); }

    /// @return Horizontal distance in meters from the position to the closest volume whose vertical limits are
    ///         within verticalMargin meters of the position's altitude. 0 when inside, infinity without such a volume.
    /// @param maxDistance Polygons farther than this may be skipped, the result is then larger than maxDistance
    double distanceTo(const Position &position, double verticalMargin = 0, double maxDistance = qInf()) const;
    double distanceTo(const QGeoCoordinate &coordinate) const { return distanceTo(Position{ coordinate }); }

    /// Where a path enters a zone
    struct PathEntry {
        qsizetype segment = 0;  ///< From path[segment] towards path[segment + 1]
        double fraction = 0;    ///< Along the segment, 0 at its start. 0 at the first position when it is inside.
        Position position;      ///< Interpolated entry position
    };

    /// @return First position of the path inside a volume, horizontally and between its vertical limits, nothing
    ///         when the path doesn't enter the zone. The path follows the great circles between the positions, see
    ///         horizontalIntervals, and altitudes and terrain elevations are interpolated linearly between them, so
    ///         paths following terrain need a position at each terrain sample.
    std::optional<PathEntry> pathEntry(const QList<Position> &path) const;

    /// @return true if the path enters a volume, see pathEntry
    bool intersectsPath(const QList<Position> &path) const { return pathEntry(path).has_value(); }
    bool intersectsPath(const QList<QGeoCoordinate> &path) const;

    /// Area flown over by a mission item, like a survey polygon, with the range of its altitudes
    struct Area {
        QList<QGeoCoordinate> polygon;  ///< Unclosed
        GeoZone::Position lowest;       ///< Lowest altitude, with the highest terrain for limits above ground
        GeoZone::Position highest;      ///< Highest altitude, with the lowest terrain
    };

    /// @return true if the area overlaps a volume horizontally and vertically
    bool intersectsArea(const Area &area) const;

    /// Comparisons with the vertical limits of a volume, see GeoZoneVertical for the datums and the policy for unknown
    /// altitudes, limits, references and terrain, which don't bound the volume
    static bool verticalOverlap(const Volume &volume, const Position &from, const Position &to, double margin = 0);
    static std::optional<std::pair<double, double>> verticalInterval(const Volume &volume, const Position &from, const Position &to, double margin = 0);
    static VerticalPosition verticalPosition(const Volume &volume, const Position &position);

    /// @return true if the coordinate is inside the horizontal projection of the volume at the index
    bool volumeContains(qsizetype volumeIndex, const QGeoCoordinate &coordinate) const;

    /// @return Fractions of the great circle between the coordinates, which vehicles fly along, inside the horizontal
    ///         projection of the volume at the index, ordered and with touching intervals merged. The boundary is
    ///         inside, so a segment touching it has an empty interval there. Long segments are checked in pieces
    ///         straight in latitude and longitude within GeoZoneGeometry::kPathGreatCircleToleranceMeters of the great
    ///         circle, see GeoZoneGeometry::greatCircleFractions. The segment takes the shorter way, which may cross the
    ///         antimeridian.
    QList<GeoZoneGeometry::Interval> horizontalIntervals(qsizetype volumeIndex, const QGeoCoordinate &from, const QGeoCoordinate &to) const;

    /// @return Lower or upper limit of the volume in meters above mean sea level at the position, see
    ///         GeoZoneVertical::limitAMSL
    static double limitAMSL(const Volume &volume, bool upper, const Position &position);

    static VerticalReference verticalReferenceFromString(const QString &reference);
    static Restriction restrictionFromString(const QString &restriction);
    static QString restrictionToString(Restriction restriction);

    /// @return 0 for the most restrictive zones, increasing for less restrictive ones
    static int restrictionSeverity(Restriction restriction);

private:
    static QString _limitToString(double limit, VerticalReference reference, const QString &referenceCode, bool feet, double qnhHPa);

    /// Polygon volumes projected around the zone center, so that queries don't project them again
    struct Projected {
        explicit Projected(const QGeoCoordinate &origin) : projection(origin) {}
        GeoZoneGeometry::LocalProjection projection;
        QList<QList<QList<QPointF>>> volumeRings;   ///< In meters, empty for circles
    };
    /// Made on first use, from any thread, since most zones are never queried
    const Projected &_projected() const;
    /// @return Fractions of the straight segment between the coordinates inside the volume, see horizontalIntervals
    QList<GeoZoneGeometry::Interval> _straightIntervals(qsizetype volumeIndex, const QGeoCoordinate &from, const QGeoCoordinate &to) const;

    const Data _data;
    QList<GeoZoneBox> _volumeBoxes; ///< Outer ring extents of the polygon volumes, invalid for circles
    GeoZoneBox _box;
    std::atomic<bool> _active = true;   ///< Updated on the GUI thread, read by checks on worker threads
    bool _scheduleAssumed = false;
    QString _sourceKey;
    QString _sourceText;
    QString _key;
    mutable QMutex _simplifiedMutex;
    mutable QHash<double, QList<QList<QGeoCoordinate>>> _simplifiedPolygons;    ///< Keyed by tolerance, guarded by _simplifiedMutex
    mutable std::once_flag _projectedOnce;
    mutable std::unique_ptr<const Projected> _projectedData;
};

/// Binary serialization of the zones as parsed, see GeoZoneDataStream. Fields added to them must be serialized too.
QDataStream &operator<<(QDataStream &stream, const GeoZone::Volume &volume);
QDataStream &operator>>(QDataStream &stream, GeoZone::Volume &volume);
QDataStream &operator<<(QDataStream &stream, const GeoZone::Data &data);
QDataStream &operator>>(QDataStream &stream, GeoZone::Data &data);
