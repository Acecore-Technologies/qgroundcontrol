/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneSchedule.h"

#include <QtCore/QHash>
#include <QtCore/QTimeZone>

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

QString _formatDateTime(const QDateTime &dateTime)
{
    return dateTime.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'"));
}

QString _dayToString(int day)
{
    static const QStringList days = { QStringLiteral("MON"), QStringLiteral("TUE"), QStringLiteral("WED"), QStringLiteral("THU"),
                                      QStringLiteral("FRI"), QStringLiteral("SAT"), QStringLiteral("SUN") };
    return ((day >= Qt::Monday) && (day <= Qt::Sunday)) ? days[day - 1] : QString();
}

QString _utcOffsetToString(int utcOffsetSecs)
{
    if (utcOffsetSecs == 0) {
        return QStringLiteral("UTC");
    }
    const int absSecs = qAbs(utcOffsetSecs);
    return QStringLiteral("UTC%1%2:%3").arg(utcOffsetSecs < 0 ? QLatin1Char('-') : QLatin1Char('+'))
                                       .arg(absSecs / 3600, 2, 10, QLatin1Char('0'))
                                       .arg((absSecs % 3600) / 60, 2, 10, QLatin1Char('0'));
}

QString _eventToString(const QString &event)
{
    static const QHash<QString, const char*> eventNames = {
        { QStringLiteral("BMCT"),   QT_TRANSLATE_NOOP("GeoZoneSchedule", "morning twilight") },
        { QStringLiteral("SR"),     QT_TRANSLATE_NOOP("GeoZoneSchedule", "sunrise") },
        { QStringLiteral("SS"),     QT_TRANSLATE_NOOP("GeoZoneSchedule", "sunset") },
        { QStringLiteral("EECT"),   QT_TRANSLATE_NOOP("GeoZoneSchedule", "evening twilight") },
    };
    const auto it = eventNames.constFind(event.trimmed().toUpper());
    return (it != eventNames.constEnd()) ? GeoZoneSchedule::tr(it.value()) : event;
}

QString _dailyPeriodToString(const GeoZoneSchedule::DailyPeriod &period)
{
    QStringList days;
    for (const int day : period.days) {
        days.append(_dayToString(day));
    }
    const QString dayString = days.isEmpty() ? GeoZoneSchedule::tr("Every day") : days.join(QStringLiteral(", "));
    const QString start = !period.startEvent.isEmpty() ? _eventToString(period.startEvent) : (period.startTime.isValid() ? period.startTime.toString(QStringLiteral("HH:mm")) : QString());
    const QString end = !period.endEvent.isEmpty() ? _eventToString(period.endEvent) : (period.endTime.isValid() ? period.endTime.toString(QStringLiteral("HH:mm")) : QString());
    if (start.isEmpty() && end.isEmpty()) {
        return dayString;
    }
    const bool timesUsed = (period.startEvent.isEmpty() && period.startTime.isValid()) || (period.endEvent.isEmpty() && period.endTime.isValid());
    const QString text = QStringLiteral("%1 %2-%3").arg(dayString, start.isEmpty() ? QStringLiteral("00:00") : start, end.isEmpty() ? QStringLiteral("24:00") : end);
    return timesUsed ? QStringLiteral("%1 %2").arg(text, _utcOffsetToString(period.utcOffsetSecs)) : text;
}

struct SunEvent {
    bool known = false;     ///< Event code is supported and the location valid
    bool alwaysUp = false;  ///< The sun stays above the event altitude all day
    QDateTime time;         ///< Invalid when the event doesn't occur that day
};

/// Sunrise equation, accurate to about a minute outside polar regions
SunEvent _sunEvent(const QString &event, const QDate &date, const QGeoCoordinate &location)
{
    SunEvent result;
    const QString code = event.trimmed().toUpper();
    double sunAltitude = 0;
    bool rising = true;
    if (code == QStringLiteral("BMCT")) {
        sunAltitude = -6;
    } else if (code == QStringLiteral("SR")) {
        sunAltitude = -0.833;
    } else if (code == QStringLiteral("SS")) {
        sunAltitude = -0.833;
        rising = false;
    } else if (code == QStringLiteral("EECT")) {
        sunAltitude = -6;
        rising = false;
    } else {
        return result;
    }
    if (!location.isValid() || !date.isValid()) {
        return result;
    }
    result.known = true;

    constexpr double degrees = M_PI / 180.0;
    constexpr double julianDate2000 = 2451545.0;
    const double meanSolarTime = static_cast<double>(date.toJulianDay() - static_cast<qint64>(julianDate2000)) - (location.longitude() / 360.0);
    const double meanAnomaly = std::fmod(357.5291 + (0.98560028 * meanSolarTime), 360.0) * degrees;
    const double center = (1.9148 * std::sin(meanAnomaly)) + (0.02 * std::sin(2 * meanAnomaly)) + (0.0003 * std::sin(3 * meanAnomaly));
    const double eclipticLongitude = std::fmod((meanAnomaly / degrees) + center + 180.0 + 102.9372, 360.0) * degrees;
    const double solarTransit = julianDate2000 + meanSolarTime + (0.0053 * std::sin(meanAnomaly)) - (0.0069 * std::sin(2 * eclipticLongitude));
    const double sinDeclination = std::sin(eclipticLongitude) * std::sin(23.4397 * degrees);
    const double cosDeclination = std::cos(std::asin(sinDeclination));
    const double latitude = location.latitude() * degrees;
    const double cosHourAngle = (std::sin(sunAltitude * degrees) - (std::sin(latitude) * sinDeclination)) / (std::cos(latitude) * cosDeclination);
    if (cosHourAngle < -1) {
        result.alwaysUp = true;
        return result;
    }
    if (cosHourAngle > 1) {
        return result;
    }

    const double hourAngle = std::acos(cosHourAngle) / degrees;
    const double julianDate = solarTransit + ((rising ? -hourAngle : hourAngle) / 360.0);
    result.time = QDateTime::fromMSecsSinceEpoch(std::llround((julianDate - 2440587.5) * 86400000.0), QTimeZone::UTC);
    return result;
}

/// @return UTC start and end of a daily period on the date in the period's offset, nothing when it doesn't
///         apply that day. Ends before the start wrap past midnight.
std::optional<std::pair<QDateTime, QDateTime>> _dailyWindow(const GeoZoneSchedule::DailyPeriod &period, const QDate &date, const QGeoCoordinate &location)
{
    if (!period.days.isEmpty() && !period.days.contains(date.dayOfWeek())) {
        return std::nullopt;
    }

    const QDateTime dayStart = QDateTime(date, QTime(0, 0), QTimeZone::UTC).addSecs(-period.utcOffsetSecs);
    const QDateTime dayEnd = dayStart.addDays(1).addMSecs(-1);

    // Time of an event. When the sun stays above or below the event altitude all day, rising events happen
    // at the start of the day and setting events at its end, or the other way around, so that daylight periods
    // cover the whole day during polar day and none of it during polar night. Unknown events are ignored.
    const auto eventTime = [&date, &location, &dayStart, &dayEnd](const QString &code, bool isStart) {
        const SunEvent event = _sunEvent(code, date, location);
        if (event.time.isValid()) {
            return event.time;
        }
        if (!event.known) {
            return isStart ? dayStart : dayEnd;
        }
        const QString upperCode = code.trimmed().toUpper();
        const bool risingEvent = (upperCode == QStringLiteral("BMCT")) || (upperCode == QStringLiteral("SR"));
        return (risingEvent == event.alwaysUp) ? dayStart : dayEnd;
    };

    QDateTime start = dayStart;
    if (!period.startEvent.isEmpty()) {
        start = eventTime(period.startEvent, true);
    } else if (period.startTime.isValid()) {
        start = QDateTime(date, period.startTime, QTimeZone::UTC).addSecs(-period.utcOffsetSecs);
    }

    QDateTime end = dayEnd;
    if (!period.endEvent.isEmpty()) {
        end = eventTime(period.endEvent, false);
    } else if (period.endTime.isValid()) {
        end = QDateTime(date, period.endTime, QTimeZone::UTC).addSecs(-period.utcOffsetSecs);
    }

    if ((start == dayEnd) && (end == dayStart)) {
        // No daylight, or no darkness, that day
        return std::nullopt;
    }
    if (end < start) {
        end = end.addDays(1);
    }
    return std::make_pair(start, end);
}

bool _dailyPeriodContains(const GeoZoneSchedule::DailyPeriod &period, const QDateTime &utc, const QGeoCoordinate &location)
{
    // Days and times are published in the period's offset. A period wrapping past midnight started the day before,
    // and a sunrise east of the offset's meridian can be before midnight.
    const QDate date = utc.addSecs(period.utcOffsetSecs).date();
    for (const QDate &day : { date.addDays(-1), date, date.addDays(1) }) {
        const auto window = _dailyWindow(period, day, location);
        if (window && (utc >= window->first) && (utc <= window->second)) {
            return true;
        }
    }
    return false;
}

bool _dailyPeriodOverlaps(const GeoZoneSchedule::DailyPeriod &period, const QDateTime &startUtc, const QDateTime &endUtc, const QGeoCoordinate &location)
{
    const QDate firstDate = startUtc.addSecs(period.utcOffsetSecs).date().addDays(-1);
    // Windows can start before their date when a sunrise is before midnight. The schedule repeats weekly, so
    // a week past the first date covers every window of longer intervals.
    const QDate lastDate = std::min(endUtc.addSecs(period.utcOffsetSecs).date().addDays(1), firstDate.addDays(9));
    for (QDate date = firstDate; date <= lastDate; date = date.addDays(1)) {
        const auto window = _dailyWindow(period, date, location);
        if (window && (window->first <= endUtc) && (window->second >= startUtc)) {
            return true;
        }
    }
    return false;
}

} // namespace

QDateTime GeoZoneSchedule::daylightEventTime(const QString &event, const QDate &date, const QGeoCoordinate &location)
{
    return _sunEvent(event, date, location).time;
}

bool GeoZoneSchedule::isActiveAt(const QList<TimePeriod> &applicability, const QDateTime &dateTime, const QGeoCoordinate &location)
{
    if (applicability.isEmpty()) {
        return true;
    }

    const QDateTime utc = dateTime.toUTC();
    for (const TimePeriod &period : applicability) {
        if (period.permanent) {
            return true;
        }
        if (period.start.isValid() && (utc < period.start)) {
            continue;
        }
        if (period.end.isValid() && (utc > period.end)) {
            continue;
        }
        if (period.schedule.isEmpty()) {
            return true;
        }
        for (const DailyPeriod &dailyPeriod : period.schedule) {
            if (_dailyPeriodContains(dailyPeriod, utc, location)) {
                return true;
            }
        }
    }

    return false;
}

bool GeoZoneSchedule::isActiveDuring(const QList<TimePeriod> &applicability, const QDateTime &start, const QDateTime &end, const QGeoCoordinate &location)
{
    if (applicability.isEmpty()) {
        return true;
    }

    const QDateTime startUtc = start.toUTC();
    const QDateTime endUtc = end.toUTC();
    if (endUtc < startUtc) {
        return false;
    }

    for (const TimePeriod &period : applicability) {
        if (period.permanent) {
            return true;
        }

        // Limit the interval to the period's validity
        const QDateTime overlapStart = (period.start.isValid() && (period.start > startUtc)) ? period.start.toUTC() : startUtc;
        const QDateTime overlapEnd = (period.end.isValid() && (period.end < endUtc)) ? period.end.toUTC() : endUtc;
        if (overlapEnd < overlapStart) {
            continue;
        }
        if (period.schedule.isEmpty()) {
            return true;
        }
        for (const DailyPeriod &dailyPeriod : period.schedule) {
            if (_dailyPeriodOverlaps(dailyPeriod, overlapStart, overlapEnd, location)) {
                return true;
            }
        }
    }

    return false;
}

QString GeoZoneSchedule::text(const QList<TimePeriod> &applicability)
{
    if (applicability.isEmpty()) {
        return tr("Permanent");
    }

    QStringList lines;
    for (const TimePeriod &period : applicability) {
        if (period.permanent) {
            lines.append(tr("Permanent"));
        } else if (period.start.isValid() && period.end.isValid()) {
            lines.append(tr("From %1 to %2").arg(_formatDateTime(period.start), _formatDateTime(period.end)));
        } else if (period.start.isValid()) {
            lines.append(tr("From %1").arg(_formatDateTime(period.start)));
        } else if (period.end.isValid()) {
            lines.append(tr("Until %1").arg(_formatDateTime(period.end)));
        } else if (period.schedule.isEmpty()) {
            lines.append(tr("Permanent"));
        }

        for (const DailyPeriod &dailyPeriod : period.schedule) {
            lines.append(QStringLiteral("  %1").arg(_dailyPeriodToString(dailyPeriod)));
        }
    }
    return lines.join(QStringLiteral("\n"));
}
