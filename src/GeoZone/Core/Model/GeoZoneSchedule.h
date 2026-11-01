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
#include <QtCore/QTime>
#include <QtPositioning/QGeoCoordinate>

class QDataStream;

/// When a geozone applies: validity periods with optional daily schedules, whose times can be daylight events
class GeoZoneSchedule
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneSchedule)

public:
    struct DailyPeriod {
        QList<int> days;        ///< Qt::DayOfWeek values, empty for any day
        QTime startTime;        ///< Invalid when not given or given as a daylight event. Ends before the start wrap past midnight.
        QTime endTime;
        int utcOffsetSecs = 0;  ///< Offset from UTC of the days and times as published
        QString startEvent;     ///< Daylight event code (BMCT, SR, SS, EECT), used instead of the time when given
        QString endEvent;
    };

    struct TimePeriod {
        bool permanent = false;
        QDateTime start;    ///< Invalid when open ended
        QDateTime end;
        QList<DailyPeriod> schedule;
    };

    /// @return true if a period applies at the time. Daylight events are computed at the location.
    ///         An empty applicability always applies.
    static bool isActiveAt(const QList<TimePeriod> &applicability, const QDateTime &dateTime, const QGeoCoordinate &location);

    /// @return true if a period applies at any time between start and end inclusive
    static bool isActiveDuring(const QList<TimePeriod> &applicability, const QDateTime &start, const QDateTime &end, const QGeoCoordinate &location);

    /// @return Human readable periods, one per line
    static QString text(const QList<TimePeriod> &applicability);

    /// Time of a daylight event used by schedules: BMCT (begin of morning civil twilight), SR (sunrise),
    /// SS (sunset) or EECT (end of evening civil twilight)
    /// @return UTC time of the event on the date at the location. Invalid for unknown events and when the
    ///         sun doesn't cross the event altitude that day.
    static QDateTime daylightEventTime(const QString &event, const QDate &date, const QGeoCoordinate &location);
};

/// Binary serialization, see GeoZoneDataStream
QDataStream &operator<<(QDataStream &stream, const GeoZoneSchedule::DailyPeriod &period);
QDataStream &operator>>(QDataStream &stream, GeoZoneSchedule::DailyPeriod &period);
QDataStream &operator<<(QDataStream &stream, const GeoZoneSchedule::TimePeriod &period);
QDataStream &operator>>(QDataStream &stream, GeoZoneSchedule::TimePeriod &period);
