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
#include <QtCore/QSharedPointer>
#include <QtCore/QRectF>
#include <QtCore/QStringList>
#include <QtCore/QTime>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include <optional>
#include <utility>

#include "GeoZoneSchedule.h"

namespace GeoZoneGeometry {
class LocalProjection;
}

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
        FlightLevel,    ///< STD, pressure altitude which depends on the weather
    };

    /// Horizontal projection plus vertical limits of one airspace volume of a zone
    struct Volume {
        bool isCircle = false;
        QList<QList<QGeoCoordinate>> rings; ///< Polygon only: [0] is the outer boundary, others are holes. Rings are not closed.
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

        QGeoRectangle boundingBox() const;
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
    };

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
    QString verticalLimitsText() const;
    QGeoCoordinate center() const { return _boundingBox.center(); }
    QGeoRectangle boundingBox() const { return _boundingBox; }
    bool active() const { return _active; }
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
    ///         when the path doesn't enter the zone. Coordinates, altitudes and terrain elevations are interpolated
    ///         linearly between the positions, so paths following terrain need a position at each terrain sample.
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

    /// @return true if the vertical limits of the volume overlap the altitudes between the positions, extended by
    ///         the margin. Unknown altitudes, limits and references overlap.
    static bool verticalOverlap(const Volume &volume, const Position &from, const Position &to, double margin = 0);

    /// @return Fractions of the straight path between the positions, with altitudes and terrain interpolated linearly,
    ///         which are within the vertical limits of the volume extended by the margin, nothing when none is.
    ///         Unknown altitudes, limits and references don't limit it.
    static std::optional<std::pair<double, double>> verticalInterval(const Volume &volume, const Position &from, const Position &to, double margin = 0);

    /// @return true if the coordinate is inside the horizontal projection of the volume
    static bool volumeContains(const Volume &volume, const QGeoCoordinate &coordinate);

    /// @return Lower or upper limit of the volume in meters above mean sea level at the position, using its terrain
    ///         elevation for limits above ground. NaN when the zone extends to the ground or has no ceiling, or the
    ///         limit can't be converted, like flight levels or limits above unknown terrain.
    static double limitAMSL(const Volume &volume, bool upper, const Position &position);

    static VerticalReference verticalReferenceFromString(const QString &reference);
    static Restriction restrictionFromString(const QString &restriction);
    static QString restrictionToString(Restriction restriction);

    /// @return 0 for the most restrictive zones, increasing for less restrictive ones
    static int restrictionSeverity(Restriction restriction);

private:
    static QString _limitToString(double limit, VerticalReference reference, const QString &referenceCode, bool feet);
    /// @return Approximate distance in meters from the projection origin to the box, 0 inside
    static double _boxDistance(const QRectF &box, const GeoZoneGeometry::LocalProjection &projection);

    const Data _data;
    QList<QRectF> _volumeBoxes;     ///< Outer ring extents of the polygon volumes, x is longitude and y latitude
    QGeoRectangle _boundingBox;
    bool _active = true;
    QString _sourceKey;
    QString _sourceText;
    QString _key;
    mutable QHash<double, QList<QList<QGeoCoordinate>>> _simplifiedPolygons;    ///< Keyed by tolerance
};
