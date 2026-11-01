/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QRectF>
#include <QtCore/QStringList>
#include <QtCore/QTime>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include "GeoZoneSchedule.h"

namespace GeoZoneGeometry {
class LocalProjection;
}

/// A UAS geographical zone loaded from an EUROCAE ED-269 or ED-318 data file.
class GeoZone : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString          identifier              READ identifier             CONSTANT FINAL)
    Q_PROPERTY(QString          name                    READ name                   CONSTANT FINAL)
    Q_PROPERTY(QString          displayName             READ displayName            CONSTANT FINAL)   ///< Name, or the identifier of unnamed zones
    Q_PROPERTY(QString          country                 READ country                CONSTANT FINAL)
    Q_PROPERTY(Restriction      restriction             READ restriction            CONSTANT FINAL)
    Q_PROPERTY(QString          restrictionText         READ restrictionText        CONSTANT FINAL)
    Q_PROPERTY(QString          restrictionConditions   READ restrictionConditions  CONSTANT FINAL)
    Q_PROPERTY(QString          reasonText              READ reasonText             CONSTANT FINAL)
    Q_PROPERTY(QString          message                 READ message                CONSTANT FINAL)
    Q_PROPERTY(QString          authorityText           READ authorityText          CONSTANT FINAL)
    Q_PROPERTY(QString          applicabilityText       READ applicabilityText      CONSTANT FINAL)
    Q_PROPERTY(QString          verticalLimitsText      READ verticalLimitsText     CONSTANT FINAL)
    Q_PROPERTY(QGeoCoordinate   center                  READ center                 CONSTANT FINAL)
    Q_PROPERTY(bool             active                  READ active                 NOTIFY activeChanged FINAL)
    Q_PROPERTY(QString          sourceKey               READ sourceKey              CONSTANT FINAL)   ///< Key of the GeoZoneSource
    Q_PROPERTY(QString          sourceText              READ sourceText             NOTIFY sourceTextChanged FINAL)   ///< File or feed the zone was loaded from, with its date

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
    };

    explicit GeoZone(const Data &data, QObject *parent = nullptr);

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
    void updateActive(const QDateTime &now);

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

    /// @return true if the path enters a volume. Altitudes are interpolated between the positions, so paths
    ///         following terrain need a position at each terrain sample.
    bool intersectsPath(const QList<Position> &path) const;
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

signals:
    void activeChanged(bool active);
    void sourceTextChanged();

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
