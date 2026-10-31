/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZone.h"
#include "FactMetaData.h"
#include "GeoZoneGeometry.h"

#include <QtCore/QHash>
#include <QtCore/QTimeZone>

#include <algorithm>
#include <limits>
#include <optional>

namespace {

constexpr double kUnlimitedSentinel = 99999.0; // Published as upper limit when a zone has no ceiling
constexpr double kMetersPerFoot = 0.3048;

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

bool _dailyPeriodOverlaps(const GeoZone::DailyPeriod &period, const QDateTime &startUtc, const QDateTime &endUtc)
{
    const bool eventBased = !period.startTime.isValid() || !period.endTime.isValid();
    const QDate firstDate = startUtc.addSecs(period.utcOffsetSecs).date().addDays(-1);
    // The schedule repeats weekly, so a week past the first date covers every window of longer intervals
    const QDate lastDate = std::min(endUtc.addSecs(period.utcOffsetSecs).date(), firstDate.addDays(8));
    for (QDate date = firstDate; date <= lastDate; date = date.addDays(1)) {
        if (!period.days.isEmpty() && !period.days.contains(date.dayOfWeek())) {
            continue;
        }

        QDateTime windowStart;
        QDateTime windowEnd;
        if (eventBased) {
            // Treated as applicable all day like in _dailyPeriodContains
            windowStart = QDateTime(date, QTime(0, 0), QTimeZone::UTC);
            windowEnd = QDateTime(date, QTime(23, 59, 59, 999), QTimeZone::UTC);
        } else {
            windowStart = QDateTime(date, period.startTime, QTimeZone::UTC);
            windowEnd = QDateTime((period.startTime <= period.endTime) ? date : date.addDays(1), period.endTime, QTimeZone::UTC);
        }
        windowStart = windowStart.addSecs(-period.utcOffsetSecs);
        windowEnd = windowEnd.addSecs(-period.utcOffsetSecs);

        if ((windowStart <= endUtc) && (windowEnd >= startUtc)) {
            return true;
        }
    }
    return false;
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

bool GeoZone::isActiveDuring(const QDateTime &start, const QDateTime &end) const
{
    if (_data.applicability.isEmpty()) {
        return true;
    }

    const QDateTime startUtc = start.toUTC();
    const QDateTime endUtc = end.toUTC();
    if (endUtc < startUtc) {
        return false;
    }

    for (const TimePeriod &period : _data.applicability) {
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
            if (_dailyPeriodOverlaps(dailyPeriod, overlapStart, overlapEnd)) {
                return true;
            }
        }
    }

    return false;
}

QList<QList<QGeoCoordinate>> GeoZone::exclusionPolygons(double simplifyTolerance) const
{
    if (simplifyTolerance > 0) {
        const auto cached = _simplifiedPolygons.constFind(simplifyTolerance);
        if (cached != _simplifiedPolygons.constEnd()) {
            return cached.value();
        }

        QList<QList<QGeoCoordinate>> simplified;
        for (const QList<QGeoCoordinate> &polygon : exclusionPolygons()) {
            const QList<QGeoCoordinate> ring = GeoZoneGeometry::simplifyOutward(polygon, simplifyTolerance);
            if (!simplified.contains(ring)) {
                simplified.append(ring);
            }
        }
        _simplifiedPolygons.insert(simplifyTolerance, simplified);
        return simplified;
    }

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

bool GeoZone::contains(const QGeoCoordinate &coordinate) const
{
    if (!coordinate.isValid() || !_boundingBox.contains(coordinate)) {
        return false;
    }

    const GeoZoneGeometry::LocalProjection projection(coordinate);
    const QPointF point(0, 0);
    for (const Volume &volume : _data.volumes) {
        if (volume.isCircle) {
            if (volume.center.isValid() && (volume.center.distanceTo(coordinate) <= volume.radius)) {
                return true;
            }
            continue;
        }
        if (volume.rings.isEmpty() || !GeoZoneGeometry::ringContains(projection.toLocal(volume.rings.first()), point)) {
            continue;
        }
        const bool inHole = std::any_of(volume.rings.cbegin() + 1, volume.rings.cend(), [&projection, &point](const QList<QGeoCoordinate> &hole) {
            return GeoZoneGeometry::ringContains(projection.toLocal(hole), point);
        });
        if (!inHole) {
            return true;
        }
    }
    return false;
}

double GeoZone::distanceTo(const QGeoCoordinate &coordinate) const
{
    if (contains(coordinate)) {
        return 0;
    }

    const GeoZoneGeometry::LocalProjection projection(coordinate);
    double minDistance = std::numeric_limits<double>::max();
    for (const Volume &volume : _data.volumes) {
        if (volume.isCircle) {
            if (volume.center.isValid()) {
                minDistance = std::min(minDistance, std::max(0.0, volume.center.distanceTo(coordinate) - volume.radius));
            }
            continue;
        }
        for (const QList<QGeoCoordinate> &ring : volume.rings) {
            minDistance = std::min(minDistance, GeoZoneGeometry::distanceToRing(QPointF(0, 0), projection.toLocal(ring)));
        }
    }
    return minDistance;
}

bool GeoZone::intersectsPath(const QList<QGeoCoordinate> &path) const
{
    if (path.isEmpty() || !_boundingBox.isValid()) {
        return false;
    }

    // QGeoRectangle can't represent a single point or a meridian segment, so compare the ranges directly
    const double north = _boundingBox.topLeft().latitude();
    const double south = _boundingBox.bottomLeft().latitude();
    const double west = _boundingBox.topLeft().longitude();
    const double east = _boundingBox.bottomRight().longitude();
    const auto overlapsBoundingBox = [north, south, west, east](const QGeoCoordinate &a, const QGeoCoordinate &b) {
        return (std::max(a.latitude(), b.latitude()) >= south) && (std::min(a.latitude(), b.latitude()) <= north) &&
               (std::max(a.longitude(), b.longitude()) >= west) && (std::min(a.longitude(), b.longitude()) <= east);
    };

    if (path.count() == 1) {
        return contains(path.first());
    }

    // Projected lazily since most segments are far from the zone
    QList<std::pair<const Volume*, QList<QList<QPointF>>>> projectedRings;
    std::optional<GeoZoneGeometry::LocalProjection> projection;
    for (qsizetype i = 1; i < path.count(); i++) {
        const QGeoCoordinate &from = path[i - 1];
        const QGeoCoordinate &to = path[i];
        if (!overlapsBoundingBox(from, to)) {
            continue;
        }
        if (contains(from) || contains(to)) {
            return true;
        }

        if (!projection) {
            projection.emplace(path.first());
            for (const Volume &volume : _data.volumes) {
                QList<QList<QPointF>> rings;
                if (!volume.isCircle) {
                    for (const QList<QGeoCoordinate> &ring : volume.rings) {
                        rings.append(projection->toLocal(ring));
                    }
                }
                projectedRings.append({ &volume, rings });
            }
        }

        const QPointF a = projection->toLocal(from);
        const QPointF b = projection->toLocal(to);
        for (const auto &[volume, rings] : std::as_const(projectedRings)) {
            if (volume->isCircle) {
                if (volume->center.isValid()) {
                    const GeoZoneGeometry::LocalProjection centerProjection(volume->center);
                    if (GeoZoneGeometry::distanceToSegment(QPointF(0, 0), centerProjection.toLocal(from), centerProjection.toLocal(to)) <= volume->radius) {
                        return true;
                    }
                }
                continue;
            }
            // Neither end is inside, so the segment enters the volume only by crossing a ring edge
            for (const QList<QPointF> &ring : rings) {
                if (GeoZoneGeometry::segmentIntersectsRing(a, b, ring)) {
                    return true;
                }
            }
        }
    }
    return false;
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

    const QString publishedUnits = feet ? QStringLiteral("ft") : QStringLiteral("m");
    const QString userUnits = FactMetaData::appSettingsVerticalDistanceUnitsString();
    if (userUnits == publishedUnits) {
        return QStringLiteral("%1 %2 %3").arg(limit).arg(publishedUnits, reference);
    }

    // Show the published value too since the converted value is rounded
    const double meters = feet ? (limit * kMetersPerFoot) : limit;
    const double converted = FactMetaData::metersToAppSettingsVerticalDistanceUnits(meters).toDouble();
    return QStringLiteral("%1 %2 %3 (%4 %5)").arg(qRound(converted)).arg(userUnits, reference).arg(limit).arg(publishedUnits);
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

int GeoZone::restrictionSeverity(Restriction restriction)
{
    switch (restriction) {
    case Prohibited:
        return 0;
    case ReqAuthorisation:
        return 1;
    case Conditional:
        return 2;
    case USpace:
        return 3;
    case NoRestriction:
        return 4;
    case Unknown:
    default:
        return 5;
    }
}
