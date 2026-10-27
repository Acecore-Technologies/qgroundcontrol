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
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QTime>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

/// A UAS geographical zone loaded from an EUROCAE ED-269 or ED-318 data file.
class GeoZone : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString          identifier              READ identifier             CONSTANT)
    Q_PROPERTY(QString          name                    READ name                   CONSTANT)
    Q_PROPERTY(QString          country                 READ country                CONSTANT)
    Q_PROPERTY(Restriction      restriction             READ restriction            CONSTANT)
    Q_PROPERTY(QString          restrictionText         READ restrictionText        CONSTANT)
    Q_PROPERTY(QString          restrictionConditions   READ restrictionConditions  CONSTANT)
    Q_PROPERTY(QString          reasonText              READ reasonText             CONSTANT)
    Q_PROPERTY(QString          message                 READ message                CONSTANT)
    Q_PROPERTY(QString          authorityText           READ authorityText          CONSTANT)
    Q_PROPERTY(QString          applicabilityText       READ applicabilityText      CONSTANT)
    Q_PROPERTY(QString          verticalLimitsText      READ verticalLimitsText     CONSTANT)
    Q_PROPERTY(QGeoCoordinate   center                  READ center                 CONSTANT)
    Q_PROPERTY(bool             active                  READ active                 NOTIFY activeChanged)

public:
    enum Restriction {
        Prohibited,
        ReqAuthorisation,
        Conditional,
        NoRestriction,
        USpace,
        Unknown
    };
    Q_ENUM(Restriction)

    /// Horizontal projection plus vertical limits of one airspace volume of a zone
    struct Volume {
        bool isCircle = false;
        QList<QList<QGeoCoordinate>> rings; ///< Polygon only: [0] is the outer boundary, others are holes. Rings are not closed.
        QGeoCoordinate center;              ///< Circle only
        double radius = 0;                  ///< Circle only, meters
        double lowerLimit = qQNaN();        ///< As published, in feet when \c feet is set. NaN when not specified.
        double upperLimit = qQNaN();
        QString lowerReference;             ///< AGL, AMSL or WGS84
        QString upperReference;
        bool feet = false;

        QGeoRectangle boundingBox() const;
    };

    struct DailyPeriod {
        QList<int> days;        ///< Qt::DayOfWeek values, empty for any day
        QTime startTime;        ///< Invalid when not given or given as a daylight event. Ends before startTime wrap past midnight.
        QTime endTime;
        int utcOffsetSecs = 0;  ///< Offset from UTC of the days and times as published
        QString startEvent;     ///< Daylight event code (BMCT, SR, SS, EECT) when used instead of a time
        QString endEvent;
    };

    struct TimePeriod {
        bool permanent = false;
        QDateTime start;    ///< Invalid when open ended
        QDateTime end;
        QList<DailyPeriod> schedule;
    };

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

    /// Re-evaluates the active state against the specified UTC time
    void updateActive(const QDateTime &now);

    /// @return true if the zone applies at the specified time. Daily periods defined by daylight
    ///         events (sunrise/sunset) cannot be evaluated and are treated as applicable.
    bool isActiveAt(const QDateTime &dateTime) const;

    /// Outer boundaries of all polygon volumes, clockwise and without duplicates. Holes are dropped
    /// so that excluding these polygons is always conservative.
    QList<QList<QGeoCoordinate>> exclusionPolygons() const;

    /// Circle volumes without duplicates
    QList<QGeoCircle> exclusionCircles() const;

    static Restriction restrictionFromString(const QString &restriction);
    static QString restrictionToString(Restriction restriction);

signals:
    void activeChanged(bool active);

private:
    static QString _limitToString(double limit, const QString &reference, bool feet);

    const Data _data;
    QGeoRectangle _boundingBox;
    bool _active = true;
};
