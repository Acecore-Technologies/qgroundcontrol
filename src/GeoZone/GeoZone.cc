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

#include <cmath>

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

QString _eventToString(const QString &event)
{
    static const QHash<QString, const char*> eventNames = {
        { QStringLiteral("BMCT"),   QT_TRANSLATE_NOOP("GeoZone", "morning twilight") },
        { QStringLiteral("SR"),     QT_TRANSLATE_NOOP("GeoZone", "sunrise") },
        { QStringLiteral("SS"),     QT_TRANSLATE_NOOP("GeoZone", "sunset") },
        { QStringLiteral("EECT"),   QT_TRANSLATE_NOOP("GeoZone", "evening twilight") },
    };
    const auto it = eventNames.constFind(event.trimmed().toUpper());
    return (it != eventNames.constEnd()) ? GeoZone::tr(it.value()) : event;
}

QString _dailyPeriodToString(const GeoZone::DailyPeriod &period)
{
    QStringList days;
    for (const int day : period.days) {
        days.append(_dayToString(day));
    }
    const QString dayString = days.isEmpty() ? GeoZone::tr("Every day") : days.join(QStringLiteral(", "));
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
std::optional<std::pair<QDateTime, QDateTime>> _dailyWindow(const GeoZone::DailyPeriod &period, const QDate &date, const QGeoCoordinate &location)
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

bool _dailyPeriodContains(const GeoZone::DailyPeriod &period, const QDateTime &utc, const QGeoCoordinate &location)
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

bool _dailyPeriodOverlaps(const GeoZone::DailyPeriod &period, const QDateTime &startUtc, const QDateTime &endUtc, const QGeoCoordinate &location)
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

QDateTime GeoZone::daylightEventTime(const QString &event, const QDate &date, const QGeoCoordinate &location)
{
    return _sunEvent(event, date, location).time;
}

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
            if (_dailyPeriodContains(dailyPeriod, utc, _boundingBox.center())) {
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
            if (_dailyPeriodOverlaps(dailyPeriod, overlapStart, overlapEnd, _boundingBox.center())) {
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

bool GeoZone::_volumeContains(const Volume &volume, const QGeoCoordinate &coordinate) const
{
    if (volume.isCircle) {
        return volume.center.isValid() && (volume.center.distanceTo(coordinate) <= volume.radius);
    }
    if (volume.rings.isEmpty()) {
        return false;
    }

    const GeoZoneGeometry::LocalProjection projection(coordinate);
    const QPointF point(0, 0);
    if (!GeoZoneGeometry::ringContains(projection.toLocal(volume.rings.first()), point)) {
        return false;
    }
    return std::none_of(volume.rings.cbegin() + 1, volume.rings.cend(), [&projection, &point](const QList<QGeoCoordinate> &hole) {
        return GeoZoneGeometry::ringContains(projection.toLocal(hole), point);
    });
}

bool GeoZone::verticalOverlap(const Volume &volume, const Position &from, const Position &to, double margin)
{
    const double scale = volume.feet ? kMetersPerFoot : 1.0;
    // The altitude range of the positions in the reference of a limit, empty when unknown
    const auto altitudeRange = [&from, &to](const QString &reference) -> std::optional<std::pair<double, double>> {
        double altitude1 = from.altitudeAMSL;
        double altitude2 = to.altitudeAMSL;
        if ((reference == QStringLiteral("AGL")) || (reference == QStringLiteral("SFC"))) {
            altitude1 -= from.terrainAMSL;
            altitude2 -= to.terrainAMSL;
        } else if (reference == QStringLiteral("WGS84")) {
            // Heights above the ellipsoid differ from mean sea level by the geoid height of up to about 100 m
            altitude1 += GeoZoneGeometry::geoidHeight(from.coordinate);
            altitude2 += GeoZoneGeometry::geoidHeight(to.coordinate);
        } else if ((reference != QStringLiteral("AMSL")) && (reference != QStringLiteral("MSL"))) {
            // Flight levels and unknown references can't be compared
            return std::nullopt;
        }
        if (qIsNaN(altitude1) || qIsNaN(altitude2)) {
            return std::nullopt;
        }
        return std::make_pair(std::min(altitude1, altitude2), std::max(altitude1, altitude2));
    };

    // Zones from the ground have no floor, vehicles on the ground or terrain data errors can be slightly below it
    const bool fromGround = ((volume.lowerReference == QStringLiteral("AGL")) || (volume.lowerReference == QStringLiteral("SFC"))) && (volume.lowerLimit <= 0);
    if (!qIsNaN(volume.lowerLimit) && !fromGround) {
        const auto range = altitudeRange(volume.lowerReference);
        if (range && (range->second < ((volume.lowerLimit * scale) - margin))) {
            return false;
        }
    }
    if (!qIsNaN(volume.upperLimit) && (volume.upperLimit < kUnlimitedSentinel)) {
        const auto range = altitudeRange(volume.upperReference);
        if (range && (range->first > ((volume.upperLimit * scale) + margin))) {
            return false;
        }
    }
    return true;
}

bool GeoZone::contains(const Position &position) const
{
    if (!position.coordinate.isValid() || !_boundingBox.contains(position.coordinate)) {
        return false;
    }

    return std::any_of(_data.volumes.cbegin(), _data.volumes.cend(), [this, &position](const Volume &volume) {
        return verticalOverlap(volume, position, position) && _volumeContains(volume, position.coordinate);
    });
}

double GeoZone::distanceTo(const Position &position, double verticalMargin) const
{
    const GeoZoneGeometry::LocalProjection projection(position.coordinate);
    double minDistance = std::numeric_limits<double>::infinity();
    for (const Volume &volume : _data.volumes) {
        if (!verticalOverlap(volume, position, position, verticalMargin)) {
            continue;
        }
        if (_volumeContains(volume, position.coordinate)) {
            return 0;
        }
        if (volume.isCircle) {
            if (volume.center.isValid()) {
                minDistance = std::min(minDistance, std::max(0.0, volume.center.distanceTo(position.coordinate) - volume.radius));
            }
            continue;
        }
        for (const QList<QGeoCoordinate> &ring : volume.rings) {
            minDistance = std::min(minDistance, GeoZoneGeometry::distanceToRing(QPointF(0, 0), projection.toLocal(ring)));
        }
    }
    return minDistance;
}

bool GeoZone::intersectsArea(const Area &area) const
{
    if ((area.polygon.count() < 3) || !_boundingBox.isValid() || !QGeoRectangle(area.polygon).intersects(_boundingBox)) {
        return false;
    }

    const GeoZoneGeometry::LocalProjection projection(area.polygon.first());
    const QList<QPointF> polygon = projection.toLocal(area.polygon);
    for (const Volume &volume : _data.volumes) {
        if (!verticalOverlap(volume, area.lowest, area.highest)) {
            continue;
        }

        if (volume.isCircle) {
            if (!volume.center.isValid()) {
                continue;
            }
            const QPointF center = projection.toLocal(volume.center);
            if (GeoZoneGeometry::ringContains(polygon, center)) {
                return true;
            }
            // The projection is affine, so measure the radius around the center
            const GeoZoneGeometry::LocalProjection centerProjection(volume.center);
            const QList<QPointF> centered = centerProjection.toLocal(area.polygon);
            if (GeoZoneGeometry::distanceToRing(QPointF(0, 0), centered) <= volume.radius) {
                return true;
            }
            continue;
        }

        if (volume.rings.isEmpty()) {
            continue;
        }
        QList<QList<QPointF>> rings;
        for (const QList<QGeoCoordinate> &ring : volume.rings) {
            rings.append(projection.toLocal(ring));
        }
        // Edges crossing, the area inside the volume, or the volume inside the area
        for (qsizetype i = 0; i < polygon.count(); i++) {
            const QPointF &a = polygon[i];
            const QPointF &b = polygon[(i + 1) % polygon.count()];
            if (std::any_of(rings.cbegin(), rings.cend(), [&a, &b](const QList<QPointF> &ring) { return GeoZoneGeometry::segmentIntersectsRing(a, b, ring); })) {
                return true;
            }
        }
        const QPointF &areaPoint = polygon.first();
        const bool areaInVolume = GeoZoneGeometry::ringContains(rings.first(), areaPoint) &&
                                  std::none_of(rings.cbegin() + 1, rings.cend(), [&areaPoint](const QList<QPointF> &hole) { return GeoZoneGeometry::ringContains(hole, areaPoint); });
        if (areaInVolume || GeoZoneGeometry::ringContains(polygon, rings.first().first())) {
            return true;
        }
    }
    return false;
}

bool GeoZone::intersectsPath(const QList<QGeoCoordinate> &path) const
{
    QList<Position> positions;
    positions.reserve(path.count());
    for (const QGeoCoordinate &coordinate : path) {
        positions.append(Position{ coordinate });
    }
    return intersectsPath(positions);
}

bool GeoZone::intersectsPath(const QList<Position> &path) const
{
    if (path.isEmpty() || !_boundingBox.isValid()) {
        return false;
    }
    if (path.count() == 1) {
        return contains(path.first());
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

    // Projected once, and only when a segment comes near, since most paths are far from the zone. The projection
    // is affine in latitude and longitude, so containment and crossing tests are exact.
    QList<QList<QList<QPointF>>> projectedRings;
    std::optional<GeoZoneGeometry::LocalProjection> projection;
    for (qsizetype i = 1; i < path.count(); i++) {
        const Position &from = path[i - 1];
        const Position &to = path[i];
        if (!from.coordinate.isValid() || !to.coordinate.isValid() || !overlapsBoundingBox(from.coordinate, to.coordinate)) {
            continue;
        }

        if (!projection) {
            projection.emplace(path.first().coordinate);
            for (const Volume &volume : _data.volumes) {
                QList<QList<QPointF>> rings;
                for (const QList<QGeoCoordinate> &ring : volume.rings) {
                    rings.append(projection->toLocal(ring));
                }
                projectedRings.append(rings);
            }
        }

        const QPointF a = projection->toLocal(from.coordinate);
        const QPointF b = projection->toLocal(to.coordinate);
        for (qsizetype v = 0; v < _data.volumes.count(); v++) {
            const Volume &volume = _data.volumes[v];
            if (!verticalOverlap(volume, from, to)) {
                continue;
            }

            if (volume.isCircle) {
                if (volume.center.isValid()) {
                    const GeoZoneGeometry::LocalProjection centerProjection(volume.center);
                    if (GeoZoneGeometry::distanceToSegment(QPointF(0, 0), centerProjection.toLocal(from.coordinate), centerProjection.toLocal(to.coordinate)) <= volume.radius) {
                        return true;
                    }
                }
                continue;
            }

            const QList<QList<QPointF>> &rings = projectedRings[v];
            if (rings.isEmpty()) {
                continue;
            }
            const auto insidePolygon = [&rings](const QPointF &point) {
                return GeoZoneGeometry::ringContains(rings.first(), point) &&
                       std::none_of(rings.cbegin() + 1, rings.cend(), [&point](const QList<QPointF> &hole) { return GeoZoneGeometry::ringContains(hole, point); });
            };
            if (insidePolygon(a) || insidePolygon(b)) {
                return true;
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
