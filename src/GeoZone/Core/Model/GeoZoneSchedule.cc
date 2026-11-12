/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneSchedule.h"
#include "GeoZoneDataStream.h"

#include <QtCore/QHash>
#include <QtCore/QTimeZone>

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

/// Altitudes in degrees of the center of the sun above the horizon at the daylight events: at sunrise and sunset its
/// upper limb is on the horizon, with the refraction of the atmosphere, and civil twilight lasts while it is less
/// than 6 degrees below the horizon
constexpr double kSunriseSunAltitude = -0.833;
constexpr double kCivilTwilightSunAltitude = -6;

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

using DaylightEvent = GeoZoneSchedule::DaylightEvent;

/// @return Name of the event of the code, the code as published when it isn't known
QString _eventToString(const QString &code)
{
    switch (GeoZoneSchedule::daylightEvent(code)) {
    case DaylightEvent::MorningTwilight:
        return GeoZoneSchedule::tr("morning twilight");
    case DaylightEvent::Sunrise:
        return GeoZoneSchedule::tr("sunrise");
    case DaylightEvent::Sunset:
        return GeoZoneSchedule::tr("sunset");
    case DaylightEvent::EveningTwilight:
        return GeoZoneSchedule::tr("evening twilight");
    case DaylightEvent::None:
    case DaylightEvent::Unknown:
        break;
    }
    return code;
}

/// @return true for the events of the morning, when the sun rises through their altitude
bool _risingEvent(DaylightEvent event)
{
    return (event == DaylightEvent::MorningTwilight) || (event == DaylightEvent::Sunrise);
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
    if (!timesUsed) {
        return text;
    }
    if (!period.timeZone.isValid()) {
        return QStringLiteral("%1 %2").arg(text, _utcOffsetToString(period.utcOffsetSecs));
    }
    const QString timeZone = QString::fromLatin1(period.timeZone.id());
    return period.timeZoneAssumed ? QStringLiteral("%1 %2 %3").arg(text, timeZone, GeoZoneSchedule::tr("(time zone assumed)")) : QStringLiteral("%1 %2").arg(text, timeZone);
}

/// How the days and times of a period are read
enum class Reading {
    Published,  ///< In its time zone or offset
    Utc,        ///< In UTC, which they may be in too when the time zone was assumed
};

/// @return UTC time of the date and time of day of the period
QDateTime _toUtc(const GeoZoneSchedule::DailyPeriod &period, const QDate &date, const QTime &time, Reading reading)
{
    if (reading == Reading::Utc) {
        return QDateTime(date, time, QTimeZone::UTC);
    }
    if (period.timeZone.isValid()) {
        // Times skipped when the clocks go forward are taken as after the change, those repeated when they go back
        // as before it
        return QDateTime(date, time, period.timeZone).toUTC();
    }
    return QDateTime(date, time, QTimeZone::UTC).addSecs(-period.utcOffsetSecs);
}

/// @return Date of the UTC time in the time zone or offset of the period
QDate _publishedDate(const GeoZoneSchedule::DailyPeriod &period, const QDateTime &utc, Reading reading)
{
    if (reading == Reading::Utc) {
        return utc.date();
    }
    return period.timeZone.isValid() ? utc.toTimeZone(period.timeZone).date() : utc.addSecs(period.utcOffsetSecs).date();
}

struct SunEvent {
    bool known = false;     ///< Event is known and the location valid
    bool alwaysUp = false;  ///< The sun stays above the event altitude all day
    QDateTime time;         ///< Invalid when the event doesn't occur that day
};

/// Sunrise equation, accurate to about a minute outside polar regions
SunEvent _sunEvent(DaylightEvent event, const QDate &date, const QGeoCoordinate &location)
{
    SunEvent result;
    double sunAltitude = 0;
    switch (event) {
    case DaylightEvent::MorningTwilight:
    case DaylightEvent::EveningTwilight:
        sunAltitude = kCivilTwilightSunAltitude;
        break;
    case DaylightEvent::Sunrise:
    case DaylightEvent::Sunset:
        sunAltitude = kSunriseSunAltitude;
        break;
    case DaylightEvent::None:
    case DaylightEvent::Unknown:
        return result;
    }
    const bool rising = _risingEvent(event);
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

/// End times from this time of day on stand for the end of the day: days are published as ending at 23:59, 23:59:59
/// or 24:00, which the parsers read as 23:59:59
constexpr QTime kLastMinuteOfDay = QTime::fromMSecsSinceStartOfDay(((23 * 60) + 59) * 60 * 1000);

/// @return UTC time a daily period ending at its end time on the date applies until. A published time stands for its
///         whole second, and one in the last minute of the day for the end of the day, so that a period followed by
///         one starting at the next second, or at midnight, leaves no time in between.
QDateTime _endTimeUtc(const GeoZoneSchedule::DailyPeriod &period, const QDate &date, Reading reading)
{
    if (period.endTime >= kLastMinuteOfDay) {
        return _toUtc(period, date.addDays(1), QTime(0, 0), reading).addMSecs(-1);
    }
    return _toUtc(period, date, period.endTime, reading).addMSecs(999 - period.endTime.msec());
}

/// @return UTC start and end of a daily period on the date in the period's time zone or offset, nothing when it
///         doesn't apply that day. Ends before the start wrap past midnight, and so do ends at the start when both
///         are times of day: 00:00 to 00:00 is the whole day. See _endTimeUtc for the end.
std::optional<std::pair<QDateTime, QDateTime>> _dailyWindow(const GeoZoneSchedule::DailyPeriod &period, const QDate &date, const QGeoCoordinate &location,
                                                            Reading reading)
{
    if (!period.days.isEmpty() && !period.days.contains(date.dayOfWeek())) {
        return std::nullopt;
    }

    // Days with a daylight saving time change are an hour shorter or longer
    const QDateTime dayStart = _toUtc(period, date, QTime(0, 0), reading);
    const QDateTime dayEnd = _toUtc(period, date.addDays(1), QTime(0, 0), reading).addMSecs(-1);

    // Time of an event. When the sun stays above or below the event altitude all day, rising events happen
    // at the start of the day and setting events at its end, or the other way around, so that daylight periods
    // cover the whole day during polar day and none of it during polar night. Unknown events are ignored.
    const auto eventTime = [&date, &location, &dayStart, &dayEnd](const QString &code, bool isStart) {
        const DaylightEvent daylightEvent = GeoZoneSchedule::daylightEvent(code);
        const SunEvent event = _sunEvent(daylightEvent, date, location);
        if (event.time.isValid()) {
            return event.time;
        }
        if (!event.known) {
            return isStart ? dayStart : dayEnd;
        }
        return (_risingEvent(daylightEvent) == event.alwaysUp) ? dayStart : dayEnd;
    };

    QDateTime start = dayStart;
    if (!period.startEvent.isEmpty()) {
        start = eventTime(period.startEvent, true);
    } else if (period.startTime.isValid()) {
        start = _toUtc(period, date, period.startTime, reading);
    }

    QDateTime end = dayEnd;
    if (!period.endEvent.isEmpty()) {
        end = eventTime(period.endEvent, false);
    } else if (period.endTime.isValid()) {
        end = _toUtc(period, date, period.endTime, reading);
    }

    if ((start == dayEnd) && (end == dayStart)) {
        // No daylight, or no darkness, that day
        return std::nullopt;
    }
    const bool endsAtTime = period.endEvent.isEmpty() && period.endTime.isValid();
    // A period from a time of day, or the start of the day, to the same time is a day long rather than an instant
    const bool wraps = (end < start) || (endsAtTime && period.startEvent.isEmpty() && (end == start));
    if (endsAtTime) {
        end = _endTimeUtc(period, wraps ? date.addDays(1) : date, reading);
    } else if (wraps) {
        end = end.addDays(1);
    }
    return std::make_pair(start, end);
}

bool _dailyPeriodContains(const GeoZoneSchedule::DailyPeriod &period, const QDateTime &utc, const QGeoCoordinate &location, Reading reading)
{
    // Days and times are published in the period's time zone or offset. A period wrapping past midnight started the
    // day before, and a sunrise east of the offset's meridian can be before midnight.
    const QDate date = _publishedDate(period, utc, reading);
    for (const QDate &day : { date.addDays(-1), date, date.addDays(1) }) {
        const auto window = _dailyWindow(period, day, location, reading);
        if (window && (utc >= window->first) && (utc <= window->second)) {
            return true;
        }
    }
    return false;
}

bool _dailyPeriodOverlaps(const GeoZoneSchedule::DailyPeriod &period, const QDateTime &startUtc, const QDateTime &endUtc, const QGeoCoordinate &location,
                          Reading reading)
{
    const QDate firstDate = _publishedDate(period, startUtc, reading).addDays(-1);
    // Windows can start before their date when a sunrise is before midnight. The schedule repeats weekly, so
    // a week past the first date covers every window of longer intervals.
    const QDate lastDate = std::min(_publishedDate(period, endUtc, reading).addDays(1), firstDate.addDays(9));
    for (QDate date = firstDate; date <= lastDate; date = date.addDays(1)) {
        const auto window = _dailyWindow(period, date, location, reading);
        if (window && (window->first <= endUtc) && (window->second >= startUtc)) {
            return true;
        }
    }
    return false;
}

/// Adds the windows of the daily period overlapping the interval to the windows, clipped to it
void _addDailyWindows(const GeoZoneSchedule::DailyPeriod &period, const QDateTime &startUtc, const QDateTime &endUtc, const QGeoCoordinate &location,
                      Reading reading, QList<std::pair<QDateTime, QDateTime>> &windows)
{
    const QDate firstDate = _publishedDate(period, startUtc, reading).addDays(-1);
    const QDate lastDate = _publishedDate(period, endUtc, reading).addDays(1);
    // Longer intervals are not flights, a period with a gap in a week isn't active throughout anyway
    if (firstDate.daysTo(lastDate) > 400) {
        return;
    }
    for (QDate date = firstDate; date <= lastDate; date = date.addDays(1)) {
        const auto window = _dailyWindow(period, date, location, reading);
        if (window && (window->first <= endUtc) && (window->second >= startUtc)) {
            windows.append({ (std::max)(window->first, startUtc), (std::min)(window->second, endUtc) });
        }
    }
}

// Periods whose time zone was assumed apply both in it and in UTC, since their times may have been either

bool _dailyPeriodContains(const GeoZoneSchedule::DailyPeriod &period, const QDateTime &utc, const QGeoCoordinate &location)
{
    return _dailyPeriodContains(period, utc, location, Reading::Published) ||
           (period.timeZoneAssumed && _dailyPeriodContains(period, utc, location, Reading::Utc));
}

bool _dailyPeriodOverlaps(const GeoZoneSchedule::DailyPeriod &period, const QDateTime &startUtc, const QDateTime &endUtc, const QGeoCoordinate &location)
{
    return _dailyPeriodOverlaps(period, startUtc, endUtc, location, Reading::Published) ||
           (period.timeZoneAssumed && _dailyPeriodOverlaps(period, startUtc, endUtc, location, Reading::Utc));
}

/// Local time of a country, see GeoZoneSchedule::countryTimeZone
struct CountryTimeZone {
    const char *alpha3;
    const char *alpha2;
    const char *timeZone;
    double westOf = 180;    ///< Longitude west of which the time zone applies, before those of the same country
};

const CountryTimeZone kCountryTimeZones[] = {
    { "ALB", "AL", "Europe/Tirane" },
    { "AND", "AD", "Europe/Andorra" },
    { "ARM", "AM", "Asia/Yerevan" },
    { "AUT", "AT", "Europe/Vienna" },
    { "AZE", "AZ", "Asia/Baku" },
    { "BEL", "BE", "Europe/Brussels" },
    { "BGR", "BG", "Europe/Sofia" },
    { "BIH", "BA", "Europe/Sarajevo" },
    { "BLR", "BY", "Europe/Minsk" },
    { "CHE", "CH", "Europe/Zurich" },
    { "CYP", "CY", "Asia/Nicosia" },
    { "CZE", "CZ", "Europe/Prague" },
    { "DEU", "DE", "Europe/Berlin" },
    { "DNK", "DK", "Europe/Copenhagen" },
    { "ESP", "ES", "Atlantic/Canary", -12 },
    { "ESP", "ES", "Europe/Madrid" },
    { "EST", "EE", "Europe/Tallinn" },
    { "FIN", "FI", "Europe/Helsinki" },
    { "FRA", "FR", "Europe/Paris" },
    { "FRO", "FO", "Atlantic/Faroe" },
    { "GBR", "GB", "Europe/London" },
    { "GEO", "GE", "Asia/Tbilisi" },
    { "GGY", "GG", "Europe/Guernsey" },
    { "GIB", "GI", "Europe/Gibraltar" },
    { "GRC", "GR", "Europe/Athens" },
    { "HRV", "HR", "Europe/Zagreb" },
    { "HUN", "HU", "Europe/Budapest" },
    { "IMN", "IM", "Europe/Isle_of_Man" },
    { "IRL", "IE", "Europe/Dublin" },
    { "ISL", "IS", "Atlantic/Reykjavik" },
    { "ITA", "IT", "Europe/Rome" },
    { "JEY", "JE", "Europe/Jersey" },
    { "LIE", "LI", "Europe/Vaduz" },
    { "LTU", "LT", "Europe/Vilnius" },
    { "LUX", "LU", "Europe/Luxembourg" },
    { "LVA", "LV", "Europe/Riga" },
    { "MCO", "MC", "Europe/Monaco" },
    { "MDA", "MD", "Europe/Chisinau" },
    { "MKD", "MK", "Europe/Skopje" },
    { "MLT", "MT", "Europe/Malta" },
    { "MNE", "ME", "Europe/Podgorica" },
    { "NLD", "NL", "Europe/Amsterdam" },
    { "NOR", "NO", "Europe/Oslo" },
    { "POL", "PL", "Europe/Warsaw" },
    { "PRT", "PT", "Atlantic/Azores", -22 },
    { "PRT", "PT", "Atlantic/Madeira", -12 },
    { "PRT", "PT", "Europe/Lisbon" },
    { "ROU", "RO", "Europe/Bucharest" },
    { "SMR", "SM", "Europe/San_Marino" },
    { "SRB", "RS", "Europe/Belgrade" },
    { "SVK", "SK", "Europe/Bratislava" },
    { "SVN", "SI", "Europe/Ljubljana" },
    { "SWE", "SE", "Europe/Stockholm" },
    { "TUR", "TR", "Europe/Istanbul" },
    { "UKR", "UA", "Europe/Kyiv" },
    { "VAT", "VA", "Europe/Vatican" },
    { "XKX", "XK", "Europe/Belgrade" },
};

} // namespace

QTimeZone GeoZoneSchedule::countryTimeZone(const QString &country, const QGeoCoordinate &location)
{
    const QByteArray code = country.trimmed().toUpper().toLatin1();
    if ((code.size() != 2) && (code.size() != 3)) {
        return QTimeZone();
    }
    for (const CountryTimeZone &entry : kCountryTimeZones) {
        if ((code != entry.alpha3) && (code != entry.alpha2)) {
            continue;
        }
        if ((entry.westOf < 180) && (!location.isValid() || (location.longitude() >= entry.westOf))) {
            continue;
        }
        return QTimeZone(QByteArray(entry.timeZone));
    }
    return QTimeZone();
}

GeoZoneSchedule::DaylightEvent GeoZoneSchedule::daylightEvent(const QString &code)
{
    if (code.isEmpty()) {
        return DaylightEvent::None;
    }
    static const QHash<QString, DaylightEvent> events = {
        { QStringLiteral("BMCT"),   DaylightEvent::MorningTwilight },
        { QStringLiteral("SR"),     DaylightEvent::Sunrise },
        { QStringLiteral("SS"),     DaylightEvent::Sunset },
        { QStringLiteral("EECT"),   DaylightEvent::EveningTwilight },
    };
    return events.value(code.trimmed().toUpper(), DaylightEvent::Unknown);
}

QDateTime GeoZoneSchedule::daylightEventTime(DaylightEvent event, const QDate &date, const QGeoCoordinate &location)
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

bool GeoZoneSchedule::isActiveThroughout(const QList<TimePeriod> &applicability, const QDateTime &start, const QDateTime &end, const QGeoCoordinate &location)
{
    if (applicability.isEmpty()) {
        return true;
    }
    const QDateTime startUtc = start.toUTC();
    const QDateTime endUtc = end.isValid() ? end.toUTC() : startUtc;
    if (endUtc < startUtc) {
        return false;
    }

    // The windows the periods apply in within the interval, which must cover it
    QList<std::pair<QDateTime, QDateTime>> windows;
    for (const TimePeriod &period : applicability) {
        if (period.permanent) {
            return true;
        }
        const QDateTime from = (period.start.isValid() && (period.start > startUtc)) ? period.start.toUTC() : startUtc;
        const QDateTime to = (period.end.isValid() && (period.end < endUtc)) ? period.end.toUTC() : endUtc;
        if (to < from) {
            continue;
        }
        if (period.schedule.isEmpty()) {
            windows.append({ from, to });
            continue;
        }
        for (const DailyPeriod &dailyPeriod : period.schedule) {
            _addDailyWindows(dailyPeriod, from, to, location, Reading::Published, windows);
            if (dailyPeriod.timeZoneAssumed) {
                _addDailyWindows(dailyPeriod, from, to, location, Reading::Utc, windows);
            }
        }
    }
    std::sort(windows.begin(), windows.end());
    QDateTime covered = startUtc;
    for (const auto &window : std::as_const(windows)) {
        if (covered.secsTo(window.first) > maxGapSecs) {
            return false;
        }
        covered = (std::max)(covered, window.second);
    }
    return !windows.isEmpty() && (covered.secsTo(endUtc) <= maxGapSecs);
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

// Fields added to the periods must be serialized here, see GeoZoneParseCache::formatVersion and GeoZoneParserTest::_testParseCacheRoundTrip
QDataStream &operator<<(QDataStream &stream, const GeoZoneSchedule::DailyPeriod &period)
{
    GeoZoneDataStream::writeCount(stream, period.days.count());
    for (const int day : period.days) {
        stream << static_cast<qint32>(day);
    }
    return stream << period.startTime << period.endTime << static_cast<qint32>(period.utcOffsetSecs) << period.startEvent << period.endEvent
                  << (period.timeZone.isValid() ? period.timeZone.id() : QByteArray()) << period.timeZoneAssumed;
}

QDataStream &operator>>(QDataStream &stream, GeoZoneSchedule::DailyPeriod &period)
{
    period = GeoZoneSchedule::DailyPeriod();
    qsizetype count = 0;
    if (!GeoZoneDataStream::readCount(stream, sizeof(qint32), count)) {
        return stream;
    }
    for (qsizetype i = 0; i < count; i++) {
        qint32 day = 0;
        stream >> day;
        // A period of other days would never apply
        if ((stream.status() != QDataStream::Ok) || (day < Qt::Monday) || (day > Qt::Sunday)) {
            stream.setStatus(QDataStream::ReadCorruptData);
            return stream;
        }
        period.days.append(day);
    }
    qint32 utcOffsetSecs = 0;
    QByteArray timeZone;
    stream >> period.startTime >> period.endTime >> utcOffsetSecs >> period.startEvent >> period.endEvent >> timeZone >> period.timeZoneAssumed;
    period.utcOffsetSecs = utcOffsetSecs;
    if (!timeZone.isEmpty()) {
        period.timeZone = QTimeZone(timeZone);
        // Without the time zone, like one this system doesn't know, the local times would be read as UTC
        if (!period.timeZone.isValid()) {
            stream.setStatus(QDataStream::ReadCorruptData);
        }
    }
    return stream;
}

QDataStream &operator<<(QDataStream &stream, const GeoZoneSchedule::TimePeriod &period)
{
    stream << period.permanent << period.start << period.end;
    GeoZoneDataStream::writeList(stream, period.schedule);
    return stream;
}

QDataStream &operator>>(QDataStream &stream, GeoZoneSchedule::TimePeriod &period)
{
    period = GeoZoneSchedule::TimePeriod();
    stream >> period.permanent >> period.start >> period.end;
    // Days, times, offset, events, time zone and whether it was assumed
    (void) GeoZoneDataStream::readList(stream, period.schedule, (7 * sizeof(quint32)) + 1);
    return stream;
}
