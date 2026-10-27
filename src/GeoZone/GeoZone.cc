/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZone.h"

#include <QtCore/QHash>

#include <algorithm>

namespace {

constexpr double kUnlimitedSentinel = 99999.0; // Published as upper limit when a zone has no ceiling

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

QString _dailyPeriodToString(const GeoZone::DailyPeriod &period)
{
    QStringList days;
    for (const int day : period.days) {
        days.append(_dayToString(day));
    }
    const QString dayString = days.isEmpty() ? GeoZone::tr("Every day") : days.join(QStringLiteral(", "));
    const QString start = period.startTime.isValid() ? period.startTime.toString(QStringLiteral("HH:mm")) : period.startEvent;
    const QString end = period.endTime.isValid() ? period.endTime.toString(QStringLiteral("HH:mm")) : period.endEvent;
    if (start.isEmpty() && end.isEmpty()) {
        return dayString;
    }
    return QStringLiteral("%1 %2-%3 %4").arg(dayString, start, end, _utcOffsetToString(period.utcOffsetSecs));
}

bool _dailyPeriodContains(const GeoZone::DailyPeriod &period, const QDateTime &utc)
{
    // Days and times are published in the period's offset, so evaluate in that offset
    const QDateTime local = utc.addSecs(period.utcOffsetSecs);
    const QDate date = local.date();
    const QTime time = local.time();
    const auto dayMatches = [&period](const QDate &day) {
        return period.days.isEmpty() || period.days.contains(day.dayOfWeek());
    };

    // Daylight events would require a sun position computation, so treat them as applicable all day
    if (!period.startTime.isValid() || !period.endTime.isValid()) {
        return dayMatches(date);
    }

    if (period.startTime <= period.endTime) {
        return dayMatches(date) && (time >= period.startTime) && (time <= period.endTime);
    }
    // The period wraps past midnight, so the early morning part belongs to the previous day
    return (dayMatches(date) && (time >= period.startTime)) || (dayMatches(date.addDays(-1)) && (time <= period.endTime));
}

bool _isClockwise(const QList<QGeoCoordinate> &ring)
{
    double sum = 0;
    for (qsizetype i = 0; i < ring.count(); i++) {
        const QGeoCoordinate &coord1 = ring[i];
        const QGeoCoordinate &coord2 = ring[(i + 1) % ring.count()];
        sum += (coord2.longitude() - coord1.longitude()) * (coord2.latitude() + coord1.latitude());
    }
    return sum >= 0;
}

} // namespace

QGeoRectangle GeoZone::Volume::boundingBox() const
{
    if (isCircle) {
        return QGeoCircle(center, radius).boundingGeoRectangle();
    }
    if (rings.isEmpty() || rings.first().isEmpty()) {
        return QGeoRectangle();
    }
    return QGeoRectangle(rings.first());
}

GeoZone::GeoZone(const Data &data, QObject *parent)
    : QObject(parent)
    , _data(data)
{
    for (const Volume &volume : _data.volumes) {
        const QGeoRectangle box = volume.boundingBox();
        if (!box.isValid()) {
            continue;
        }
        _boundingBox = _boundingBox.isValid() ? _boundingBox.united(box) : box;
    }

    _active = isActiveAt(QDateTime::currentDateTimeUtc());
}

void GeoZone::updateActive(const QDateTime &now)
{
    const bool active = isActiveAt(now);
    if (active != _active) {
        _active = active;
        emit activeChanged(_active);
    }
}

bool GeoZone::isActiveAt(const QDateTime &dateTime) const
{
    if (_data.applicability.isEmpty()) {
        return true;
    }

    const QDateTime utc = dateTime.toUTC();
    for (const TimePeriod &period : _data.applicability) {
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
            if (_dailyPeriodContains(dailyPeriod, utc)) {
                return true;
            }
        }
    }

    return false;
}

QList<QList<QGeoCoordinate>> GeoZone::exclusionPolygons() const
{
    QList<QList<QGeoCoordinate>> polygons;
    for (const Volume &volume : _data.volumes) {
        if (volume.isCircle || volume.rings.isEmpty() || (volume.rings.first().count() < 3)) {
            continue;
        }

        QList<QGeoCoordinate> outerRing = volume.rings.first();
        if (!_isClockwise(outerRing)) {
            std::reverse(outerRing.begin(), outerRing.end());
        }
        if (!polygons.contains(outerRing)) {
            polygons.append(outerRing);
        }
    }
    return polygons;
}

QList<QGeoCircle> GeoZone::exclusionCircles() const
{
    QList<QGeoCircle> circles;
    for (const Volume &volume : _data.volumes) {
        if (!volume.isCircle || !volume.center.isValid() || (volume.radius <= 0)) {
            continue;
        }

        const QGeoCircle circle(volume.center, volume.radius);
        if (!circles.contains(circle)) {
            circles.append(circle);
        }
    }
    return circles;
}

QString GeoZone::reasonText() const
{
    static const QHash<QString, const char*> reasonNames = {
        { QStringLiteral("AIR_TRAFFIC"),        QT_TR_NOOP("Air traffic") },
        { QStringLiteral("SENSITIVE"),          QT_TR_NOOP("Sensitive area") },
        { QStringLiteral("PRIVACY"),            QT_TR_NOOP("Privacy") },
        { QStringLiteral("POPULATION"),         QT_TR_NOOP("Population") },
        { QStringLiteral("NATURE"),             QT_TR_NOOP("Nature") },
        { QStringLiteral("NOISE"),              QT_TR_NOOP("Noise") },
        { QStringLiteral("FOREIGN_TERRITORY"),  QT_TR_NOOP("Foreign territory") },
        { QStringLiteral("EMERGENCY"),          QT_TR_NOOP("Emergency") },
        { QStringLiteral("OTHER"),              QT_TR_NOOP("Other") },
    };

    QStringList reasons;
    for (const QString &reason : _data.reasons) {
        const auto it = reasonNames.constFind(reason.toUpper());
        reasons.append((it != reasonNames.constEnd()) ? tr(it.value()) : reason);
    }

    QString text = reasons.join(QStringLiteral(", "));
    if (!_data.otherReasonInfo.isEmpty()) {
        text = text.isEmpty() ? _data.otherReasonInfo : QStringLiteral("%1 (%2)").arg(text, _data.otherReasonInfo);
    }
    return text;
}

QString GeoZone::applicabilityText() const
{
    if (_data.applicability.isEmpty()) {
        return tr("Permanent");
    }

    QStringList lines;
    for (const TimePeriod &period : _data.applicability) {
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

QString GeoZone::verticalLimitsText() const
{
    QStringList lines;
    for (const Volume &volume : _data.volumes) {
        QString line;
        if (qIsNaN(volume.lowerLimit) && qIsNaN(volume.upperLimit)) {
            line = tr("Not specified");
        } else {
            line = QStringLiteral("%1 - %2").arg(_limitToString(volume.lowerLimit, volume.lowerReference, volume.feet),
                                                 _limitToString(volume.upperLimit, volume.upperReference, volume.feet));
        }
        if (!lines.contains(line)) {
            lines.append(line);
        }
    }
    return lines.join(QStringLiteral("\n"));
}

QString GeoZone::_limitToString(double limit, const QString &reference, bool feet)
{
    if (qIsNaN(limit)) {
        return tr("not specified");
    }
    if (limit >= kUnlimitedSentinel) {
        return tr("Unlimited");
    }
    if ((limit == 0) && (reference.compare(QStringLiteral("AGL"), Qt::CaseInsensitive) == 0)) {
        return tr("GND");
    }
    return QStringLiteral("%1 %2 %3").arg(limit).arg(feet ? QStringLiteral("ft") : QStringLiteral("m"), reference);
}

GeoZone::Restriction GeoZone::restrictionFromString(const QString &restriction)
{
    const QString value = restriction.trimmed().toUpper();
    if (value == QStringLiteral("PROHIBITED")) {
        return Prohibited;
    }
    // ED-269 uses the British spelling, ED-318 the American one
    if ((value == QStringLiteral("REQ_AUTHORISATION")) || (value == QStringLiteral("REQ_AUTHORIZATION"))) {
        return ReqAuthorisation;
    }
    if (value == QStringLiteral("CONDITIONAL")) {
        return Conditional;
    }
    if (value == QStringLiteral("NO_RESTRICTION")) {
        return NoRestriction;
    }
    if (value == QStringLiteral("USPACE")) {
        return USpace;
    }
    return Unknown;
}

QString GeoZone::restrictionToString(Restriction restriction)
{
    switch (restriction) {
    case Prohibited:
        return tr("Prohibited");
    case ReqAuthorisation:
        return tr("Authorisation required");
    case Conditional:
        return tr("Conditional");
    case NoRestriction:
        return tr("No restriction");
    case USpace:
        return tr("U-space airspace");
    case Unknown:
    default:
        return tr("Unknown");
    }
}
