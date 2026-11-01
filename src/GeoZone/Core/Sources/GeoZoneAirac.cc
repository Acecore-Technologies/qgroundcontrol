/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAirac.h"

namespace {

/// @return a / b rounded towards negative infinity
qint64 floorDiv(qint64 a, qint64 b)
{
    return (a / b) - (((a % b) != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

/// @return Number of the cycle in effect on the date, counted from the epoch
qint64 cycleIndex(const QDate &date)
{
    return floorDiv(GeoZoneAirac::epoch().daysTo(date), GeoZoneAirac::cycleDays);
}

} // namespace

namespace GeoZoneAirac {

QDate epoch()
{
    return QDate(2020, 1, 2);
}

QDate cycleStart(const QDate &date)
{
    return date.isValid() ? epoch().addDays(cycleIndex(date) * cycleDays) : QDate();
}

QDate nextCycleStart(const QDate &date)
{
    return date.isValid() ? cycleStart(date).addDays(cycleDays) : QDate();
}

QString cycleName(const QDate &date)
{
    if (!date.isValid()) {
        return QString();
    }
    const QDate start = cycleStart(date);
    // The first cycle of a year is the first one effective in it
    const qint64 firstOfYear = -floorDiv(-epoch().daysTo(QDate(start.year(), 1, 1)), cycleDays);
    const qint64 number = cycleIndex(date) - firstOfYear + 1;
    return QStringLiteral("%1%2").arg(start.year() % 100, 2, 10, QChar(u'0')).arg(number, 2, 10, QChar(u'0'));
}

} // namespace GeoZoneAirac
