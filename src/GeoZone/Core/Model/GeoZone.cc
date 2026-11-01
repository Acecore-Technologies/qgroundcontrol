/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZone.h"
#include "GeoZoneDataStream.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneVertical.h"

#include <QtCore/QHash>

#include <cmath>

#include <algorithm>
#include <limits>
#include <optional>

namespace {

/// @return The ring with each longitude moved by whole turns to within 180 degrees of the previous one, starting
///         from the reference, so that edges crossing the antimeridian don't wrap around the world. Shared when
///         unchanged.
GeoZone::Ring continuousRing(const GeoZone::Ring &ring, double referenceLongitude)
{
    GeoZone::Ring result = ring;
    double previous = referenceLongitude;
    for (qsizetype i = 0; i < ring.count(); i++) {
        const double longitude = ring[i].x();
        const double continuous = longitude + (360.0 * std::round((previous - longitude) / 360.0));
        if (continuous != longitude) {
            result[i].setX(continuous);
        }
        previous = continuous;
    }
    return result;
}

GeoZone::Data continuousData(const GeoZone::Data &data)
{
    GeoZone::Data result = data;
    for (qsizetype v = 0; v < data.volumes.count(); v++) {
        const GeoZone::Volume &volume = data.volumes[v];
        if (volume.isCircle || volume.rings.isEmpty() || volume.rings.first().isEmpty()) {
            continue;
        }
        const double reference = volume.rings.first().first().x();
        for (qsizetype r = 0; r < volume.rings.count(); r++) {
            const GeoZone::Ring ring = continuousRing(volume.rings[r], reference);
            if (!ring.isSharedWith(volume.rings[r])) {
                result.volumes[v].rings[r] = ring;
            }
        }
    }
    return result;
}

} // namespace

GeoZoneBox GeoZone::Volume::boundingBox() const
{
    if (isCircle) {
        return GeoZoneBox(QGeoCircle(center, radius).boundingGeoRectangle());
    }
    return rings.isEmpty() ? GeoZoneBox() : GeoZoneBox::fromLonLatPath(rings.first());
}

GeoZone::Ring GeoZone::toRing(const QList<QGeoCoordinate> &coordinates)
{
    Ring ring;
    ring.reserve(coordinates.count());
    for (const QGeoCoordinate &coordinate : coordinates) {
        ring.append(QPointF(coordinate.longitude(), coordinate.latitude()));
    }
    return ring;
}

QList<QGeoCoordinate> GeoZone::toCoordinates(const Ring &ring)
{
    QList<QGeoCoordinate> coordinates;
    coordinates.reserve(ring.count());
    for (const QPointF &vertex : ring) {
        coordinates.append(toCoordinate(vertex));
    }
    return coordinates;
}

GeoZone::GeoZone(const Data &data)
    : _data(continuousData(data))
{
    _volumeBoxes.reserve(_data.volumes.count());
    for (const Volume &volume : _data.volumes) {
        const GeoZoneBox box = volume.boundingBox();
        _volumeBoxes.append(volume.isCircle ? GeoZoneBox() : box);
        _box = _box.united(box);
    }

    _active = isActiveAt(GeoZoneEnvironment::currentDateTimeUtc());
    setSourceKey(QString());

    // All daylight events happen at the equator at an equinox, so those without a time there are unknown
    const auto unknownEvent = [](const QString &event) {
        return !event.isEmpty() && !GeoZoneSchedule::daylightEventTime(event, QDate(2000, 3, 20), QGeoCoordinate(0, 0)).isValid();
    };
    for (const TimePeriod &period : _data.applicability) {
        for (const DailyPeriod &daily : period.schedule) {
            _scheduleAssumed = _scheduleAssumed || unknownEvent(daily.startEvent) || unknownEvent(daily.endEvent);
        }
    }
}

void GeoZone::setSourceKey(const QString &sourceKey)
{
    _sourceKey = sourceKey;
    _key = QStringLiteral("%1\x1f%2\x1f%3\x1f%4").arg(sourceKey, _data.identifier, _data.name, QString::number(static_cast<int>(_data.restriction)));
}

void GeoZone::setSourceText(const QString &sourceText)
{
    _sourceText = sourceText;
}

bool GeoZone::isActiveAt(const QDateTime &dateTime) const
{
    return GeoZoneSchedule::isActiveAt(_data.applicability, dateTime, _box.center());
}

bool GeoZone::isActiveDuring(const QDateTime &start, const QDateTime &end) const
{
    return GeoZoneSchedule::isActiveDuring(_data.applicability, start, end, _box.center());
}

bool GeoZone::updateActive(const QDateTime &now)
{
    const bool active = isActiveAt(now);
    if (active == _active.load(std::memory_order_relaxed)) {
        return false;
    }
    _active = active;
    return true;
}

QList<QList<QGeoCoordinate>> GeoZone::exclusionPolygons(double simplifyTolerance) const
{
    if (simplifyTolerance > 0) {
        {
            const QMutexLocker locker(&_simplifiedMutex);
            const auto cached = _simplifiedPolygons.constFind(simplifyTolerance);
            if (cached != _simplifiedPolygons.constEnd()) {
                return cached.value();
            }
        }

        QList<QList<QGeoCoordinate>> simplified;
        for (const QList<QGeoCoordinate> &polygon : exclusionPolygons()) {
            const QList<QGeoCoordinate> ring = GeoZoneGeometry::simplifyOutward(polygon, simplifyTolerance);
            if (!simplified.contains(ring)) {
                simplified.append(ring);
            }
        }
        const QMutexLocker locker(&_simplifiedMutex);
        _simplifiedPolygons.insert(simplifyTolerance, simplified);
        return simplified;
    }

    QList<Ring> rings;
    for (const Volume &volume : _data.volumes) {
        if (volume.isCircle || volume.rings.isEmpty() || (volume.rings.first().count() < 3)) {
            continue;
        }

        Ring outerRing = volume.rings.first();
        if (GeoZoneGeometry::signedArea(outerRing) > 0) {
            std::reverse(outerRing.begin(), outerRing.end());
        }
        if (!rings.contains(outerRing)) {
            rings.append(outerRing);
        }
    }

    QList<QList<QGeoCoordinate>> polygons;
    polygons.reserve(rings.count());
    for (const Ring &ring : std::as_const(rings)) {
        polygons.append(toCoordinates(ring));
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

const GeoZone::Projected &GeoZone::_projected() const
{
    std::call_once(_projectedOnce, [this]() {
        auto projected = std::make_unique<Projected>(_box.center());
        projected->volumeRings.reserve(_data.volumes.count());
        for (const Volume &volume : _data.volumes) {
            QList<QList<QPointF>> rings;
            if (!volume.isCircle) {
                rings.reserve(volume.rings.count());
                for (const Ring &ring : volume.rings) {
                    rings.append(projected->projection.lonLatToLocal(ring));
                }
            }
            projected->volumeRings.append(rings);
        }
        _projectedData = std::move(projected);
    });
    return *_projectedData;
}

bool GeoZone::volumeContains(qsizetype volumeIndex, const QGeoCoordinate &coordinate) const
{
    const Volume &volume = _data.volumes[volumeIndex];
    if (volume.isCircle) {
        return volume.center.isValid() && (volume.center.distanceTo(coordinate) <= volume.radius);
    }
    if (volume.rings.isEmpty() || !_volumeBoxes[volumeIndex].contains(coordinate)) {
        return false;
    }

    const Projected &projected = _projected();
    return GeoZoneGeometry::polygonContains(projected.volumeRings[volumeIndex], projected.projection.toLocal(coordinate));
}

QList<GeoZoneGeometry::Interval> GeoZone::horizontalIntervals(qsizetype volumeIndex, const QGeoCoordinate &from, const QGeoCoordinate &to) const
{
    if (!from.isValid() || !to.isValid()) {
        return {};
    }
    const QPointF a(from.longitude(), from.latitude());
    const QPointF b(to.longitude(), to.latitude());
    const QList<double> fractions = GeoZoneGeometry::greatCircleFractions(a, b);
    if (fractions.count() <= 2) {
        return _straightIntervals(volumeIndex, from, to);
    }

    // Pieces of the great circle, with their intervals scaled to fractions of the whole
    QList<GeoZoneGeometry::Interval> intervals;
    QGeoCoordinate pieceFrom = from;
    for (qsizetype i = 1; i < fractions.count(); i++) {
        const QGeoCoordinate pieceTo = (i == (fractions.count() - 1)) ? to : toCoordinate(GeoZoneGeometry::greatCircleInterpolate(a, b, fractions[i]));
        const auto scaled = [&fractions, i](double fraction) {
            return (fraction <= 0) ? fractions[i - 1] : ((fraction >= 1) ? fractions[i] : (fractions[i - 1] + ((fractions[i] - fractions[i - 1]) * fraction)));
        };
        for (const GeoZoneGeometry::Interval &interval : _straightIntervals(volumeIndex, pieceFrom, pieceTo)) {
            const GeoZoneGeometry::Interval whole{ scaled(interval.first), scaled(interval.second) };
            if (!intervals.isEmpty() && (whole.first <= intervals.last().second)) {
                intervals.last().second = (std::max)(intervals.last().second, whole.second);
            } else {
                intervals.append(whole);
            }
        }
        pieceFrom = pieceTo;
    }
    return intervals;
}

QList<GeoZoneGeometry::Interval> GeoZone::_straightIntervals(qsizetype volumeIndex, const QGeoCoordinate &from, const QGeoCoordinate &to) const
{
    const Volume &volume = _data.volumes[volumeIndex];

    QList<GeoZoneGeometry::Interval> intervals;
    if (volume.isCircle) {
        if (!volume.center.isValid()) {
            return {};
        }
        const GeoZoneGeometry::LocalProjection centerProjection(volume.center);
        const QPointF a = centerProjection.toLocal(from);
        if (const auto interval = GeoZoneGeometry::segmentCircleInterval(a, centerProjection.toLocal(to, a), volume.radius)) {
            intervals.append(*interval);
        }
        return intervals;
    }
    if (volume.rings.isEmpty() || !_volumeBoxes[volumeIndex].intersects(GeoZoneBox::fromPath({ from, to }))) {
        return {};
    }

    // The projection is affine in latitude and longitude, so containment and crossing tests are exact. The end
    // continues from the start, so that segments far from the projection origin don't wrap around the world.
    const Projected &projected = _projected();
    const QPointF a = projected.projection.toLocal(from);
    QList<GeoZoneGeometry::Interval> raw = GeoZoneGeometry::segmentPolygonIntervals(a, projected.projection.toLocal(to, a), projected.volumeRings[volumeIndex]);
    std::sort(raw.begin(), raw.end());
    for (const GeoZoneGeometry::Interval &interval : std::as_const(raw)) {
        if (!intervals.isEmpty() && (interval.first <= intervals.last().second)) {
            intervals.last().second = (std::max)(intervals.last().second, interval.second);
        } else {
            intervals.append(interval);
        }
    }
    return intervals;
}

bool GeoZone::verticalOverlap(const Volume &volume, const Position &from, const Position &to, double margin)
{
    return GeoZoneVertical::overlap(volume, from, to, margin);
}

std::optional<std::pair<double, double>> GeoZone::verticalInterval(const Volume &volume, const Position &from, const Position &to, double margin)
{
    return GeoZoneVertical::interval(volume, from, to, margin);
}

GeoZone::VerticalPosition GeoZone::verticalPosition(const Volume &volume, const Position &position)
{
    return GeoZoneVertical::position(volume, position);
}

double GeoZone::limitAMSL(const Volume &volume, bool upper, const Position &position)
{
    return GeoZoneVertical::limitAMSL(volume, upper, position);
}

bool GeoZone::contains(const Position &position) const
{
    if (!position.coordinate.isValid() || !_box.contains(position.coordinate)) {
        return false;
    }

    for (qsizetype i = 0; i < _data.volumes.count(); i++) {
        if (GeoZoneVertical::overlap(_data.volumes[i], position, position) && volumeContains(i, position.coordinate)) {
            return true;
        }
    }
    return false;
}

double GeoZone::distanceTo(const Position &position, double verticalMargin, double maxDistance) const
{
    double minDistance = std::numeric_limits<double>::infinity();
    for (qsizetype i = 0; i < _data.volumes.count(); i++) {
        const Volume &volume = _data.volumes[i];
        if (!GeoZoneVertical::overlap(volume, position, position, verticalMargin)) {
            continue;
        }
        if (volume.isCircle) {
            if (volume.center.isValid()) {
                minDistance = (std::min)(minDistance, (std::max)(0.0, volume.center.distanceTo(position.coordinate) - volume.radius));
            }
            continue;
        }
        if (volume.rings.isEmpty() || (_volumeBoxes[i].distanceTo(position.coordinate) > (std::min)(minDistance, maxDistance))) {
            continue;
        }

        // Measured like in a projection around the position, by scaling east distances of the zone projection
        const Projected &projected = _projected();
        const QList<QList<QPointF>> &rings = projected.volumeRings[i];
        const QPointF point = projected.projection.toLocal(position.coordinate);
        if (GeoZoneGeometry::polygonContains(rings, point)) {
            return 0;
        }
        const double eastScale = projected.projection.eastScaleAt(position.coordinate.latitude());
        for (const QList<QPointF> &ring : rings) {
            minDistance = (std::min)(minDistance, GeoZoneGeometry::distanceToRing(point, ring, eastScale));
        }
    }
    return minDistance;
}

bool GeoZone::intersectsArea(const Area &area) const
{
    if ((area.polygon.count() < 3) || !_box.intersects(GeoZoneBox::fromPath(area.polygon))) {
        return false;
    }

    const Projected &projected = _projected();
    const GeoZoneGeometry::LocalProjection &projection = projected.projection;
    const QList<QPointF> polygon = projection.toLocal(area.polygon);
    for (qsizetype v = 0; v < _data.volumes.count(); v++) {
        const Volume &volume = _data.volumes[v];
        if (!GeoZoneVertical::overlap(volume, area.lowest, area.highest)) {
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
        const QList<QList<QPointF>> &rings = projected.volumeRings[v];
        // Edges crossing, the area inside the volume, or the volume inside the area
        for (qsizetype i = 0; i < polygon.count(); i++) {
            const QPointF &a = polygon[i];
            const QPointF &b = polygon[(i + 1) % polygon.count()];
            if (std::any_of(rings.cbegin(), rings.cend(), [&a, &b](const QList<QPointF> &ring) { return GeoZoneGeometry::segmentIntersectsRing(a, b, ring); })) {
                return true;
            }
        }
        if (GeoZoneGeometry::polygonContains(rings, polygon.first()) || GeoZoneGeometry::ringContains(polygon, rings.first().first())) {
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

std::optional<GeoZone::PathEntry> GeoZone::pathEntry(const QList<Position> &path) const
{
    if (path.isEmpty() || !_box.isValid()) {
        return std::nullopt;
    }
    if (path.count() == 1) {
        return contains(path.first()) ? std::make_optional(PathEntry{ 0, 0, path.first() }) : std::nullopt;
    }

    for (qsizetype i = 1; i < path.count(); i++) {
        const Position &from = path[i - 1];
        const Position &to = path[i];
        if (!from.coordinate.isValid() || !to.coordinate.isValid()) {
            continue;
        }
        // The segment follows the great circle the shorter way, which may cross the antimeridian and bulge towards the
        // pole beyond the bounds of its ends
        const QPointF a(from.coordinate.longitude(), from.coordinate.latitude());
        const QPointF b(to.coordinate.longitude(), to.coordinate.latitude());
        const QList<double> fractions = GeoZoneGeometry::greatCircleFractions(a, b);
        QList<QGeoCoordinate> greatCircle = { from.coordinate };
        for (qsizetype k = 1; k < (fractions.count() - 1); k++) {
            greatCircle.append(toCoordinate(GeoZoneGeometry::greatCircleInterpolate(a, b, fractions[k])));
        }
        greatCircle.append(to.coordinate);
        if (!_box.intersects(GeoZoneBox::fromPath(greatCircle))) {
            continue;
        }

        // The earliest fraction of the segment inside a volume horizontally and vertically
        double entry = qInf();
        for (qsizetype v = 0; v < _data.volumes.count(); v++) {
            const auto vertical = GeoZoneVertical::interval(_data.volumes[v], from, to);
            if (!vertical || (vertical->first >= entry)) {
                continue;
            }
            for (const GeoZoneGeometry::Interval &interval : horizontalIntervals(v, from.coordinate, to.coordinate)) {
                if ((interval.second >= vertical->first) && (interval.first <= vertical->second)) {
                    entry = (std::min)(entry, (std::max)(interval.first, vertical->first));
                    break;
                }
            }
        }
        if (std::isinf(entry)) {
            continue;
        }

        const auto interpolate = [entry](double value1, double value2) {
            return (entry <= 0) ? value1 : ((entry >= 1) ? value2 : (value1 + ((value2 - value1) * entry)));
        };
        PathEntry result;
        result.segment = i - 1;
        result.fraction = entry;
        if ((entry <= 0) || (entry >= 1)) {
            result.position.coordinate = (entry <= 0) ? from.coordinate : to.coordinate;
        } else if (fractions.count() > 2) {
            result.position.coordinate = toCoordinate(GeoZoneGeometry::greatCircleInterpolate(a, b, entry));
        } else {
            // The projection is affine in latitude and longitude, so the entry is on the segment the containment tests use
            const GeoZoneGeometry::LocalProjection &projection = _projected().projection;
            const QPointF localFrom = projection.toLocal(from.coordinate);
            const QPointF localTo = projection.toLocal(to.coordinate, localFrom);
            result.position.coordinate = projection.toGeo(localFrom + ((localTo - localFrom) * entry));
        }
        result.position.altitudeAMSL = interpolate(from.altitudeAMSL, to.altitudeAMSL);
        result.position.terrainAMSL = interpolate(from.terrainAMSL, to.terrainAMSL);
        return result;
    }
    return std::nullopt;
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

QString GeoZone::regulationExemptionText() const
{
    const QString code = _data.regulationExemption.trimmed().toUpper();
    if (code == QStringLiteral("YES")) {
        return tr("Exemptions possible");
    }
    if (code == QStringLiteral("NO")) {
        return tr("No exemptions");
    }
    return _data.regulationExemption;
}

QString GeoZone::applicabilityText() const
{
    return GeoZoneSchedule::text(_data.applicability);
}

QString GeoZone::verticalLimitsText() const
{
    const double qnh = GeoZoneEnvironment::qnhHPa();
    const auto convertedFlightLevel = [](double limit, VerticalReference reference) {
        return (reference == VerticalReference::FlightLevel) && !qIsNaN(limit) && (limit < GeoZoneVertical::unlimitedSentinel);
    };
    QStringList lines;
    for (const Volume &volume : _data.volumes) {
        QString line;
        if (qIsNaN(volume.lowerLimit) && qIsNaN(volume.upperLimit)) {
            line = tr("Not specified");
        } else {
            line = QStringLiteral("%1 - %2").arg(_limitToString(volume.lowerLimit, volume.lowerReference, volume.lowerReferenceCode, volume.feet, qnh),
                                                 _limitToString(volume.upperLimit, volume.upperReference, volume.upperReferenceCode, volume.feet, qnh));
            if (convertedFlightLevel(volume.lowerLimit, volume.lowerReference) || convertedFlightLevel(volume.upperLimit, volume.upperReference)) {
                line += QStringLiteral(" ") + tr("(QNH %1 hPa)").arg(QString::number(qnh, 'g', 6));
            }
        }
        if (!lines.contains(line)) {
            lines.append(line);
        }
    }
    return lines.join(QStringLiteral("\n"));
}

bool GeoZone::approximateLimits() const
{
    return std::any_of(_data.volumes.cbegin(), _data.volumes.cend(), [](const Volume &volume) { return volume.approximateLimits(); });
}

QString GeoZone::_limitToString(double limit, VerticalReference reference, const QString &referenceCode, bool feet, double qnhHPa)
{
    if (qIsNaN(limit)) {
        return tr("not specified");
    }
    if (limit >= GeoZoneVertical::unlimitedSentinel) {
        return tr("Unlimited");
    }
    if ((limit == 0) && (reference == VerticalReference::Ground)) {
        return tr("GND");
    }
    if (reference == VerticalReference::FlightLevel) {
        // In hundreds of feet of pressure altitude, with the height it is at with the QNH in the standard atmosphere
        const double pressureAltitude = feet ? (limit * GeoZoneVertical::metersPerFoot) : limit;
        const double flightLevel = std::round(pressureAltitude / GeoZoneVertical::metersPerFoot / 10) / 10;
        const double amsl = GeoZoneEnvironment::metersToVerticalDistanceUnits(GeoZoneVertical::pressureAltitudeToAMSL(pressureAltitude, qnhHPa));
        return tr("FL %1 ≈ %2 %3 AMSL").arg(flightLevel).arg(qRound(amsl)).arg(GeoZoneEnvironment::verticalDistanceUnits());
    }

    const QString publishedUnits = feet ? QStringLiteral("ft") : QStringLiteral("m");
    const QString userUnits = GeoZoneEnvironment::verticalDistanceUnits();
    if (userUnits == publishedUnits) {
        return QStringLiteral("%1 %2 %3").arg(limit).arg(publishedUnits, referenceCode);
    }

    // Show the published value too since the converted value is rounded
    const double meters = feet ? (limit * GeoZoneVertical::metersPerFoot) : limit;
    const double converted = GeoZoneEnvironment::metersToVerticalDistanceUnits(meters);
    return QStringLiteral("%1 %2 %3 (%4 %5)").arg(qRound(converted)).arg(userUnits, referenceCode).arg(limit).arg(publishedUnits);
}

void GeoZone::Volume::setReferences(const QString &lowerCode, const QString &upperCode)
{
    lowerReferenceCode = lowerCode.trimmed().toUpper();
    upperReferenceCode = upperCode.trimmed().toUpper();
    lowerReference = verticalReferenceFromString(lowerReferenceCode);
    upperReference = verticalReferenceFromString(upperReferenceCode);
}

GeoZone::VerticalReference GeoZone::verticalReferenceFromString(const QString &reference)
{
    const QString value = reference.trimmed().toUpper();
    if ((value == QStringLiteral("AGL")) || (value == QStringLiteral("SFC"))) {
        return VerticalReference::Ground;
    }
    if ((value == QStringLiteral("AMSL")) || (value == QStringLiteral("MSL"))) {
        return VerticalReference::MeanSeaLevel;
    }
    if (value == QStringLiteral("WGS84")) {
        return VerticalReference::Ellipsoid;
    }
    if (value == QStringLiteral("STD")) {
        return VerticalReference::FlightLevel;
    }
    return VerticalReference::Unknown;
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
        break;
    }
    return tr("Unknown");
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
        break;
    }
    return 5;
}

// Fields added to the zones must be serialized here, see GeoZoneParseCache::formatVersion
static_assert((sizeof(void*) != 8) || (sizeof(GeoZone::Volume) == 128), "Serialize the new fields of GeoZone::Volume");
static_assert((sizeof(void*) != 8) || (sizeof(GeoZone::Data) == 392), "Serialize the new fields of GeoZone::Data");

QDataStream &operator<<(QDataStream &stream, const GeoZone::Volume &volume)
{
    stream << volume.isCircle;
    GeoZoneDataStream::writeCount(stream, volume.rings.count());
    for (const GeoZone::Ring &ring : volume.rings) {
        GeoZoneDataStream::writePoints(stream, ring);
    }
    stream << volume.center.latitude() << volume.center.longitude() << volume.center.altitude() << volume.radius;
    stream << volume.lowerLimit << volume.upperLimit;
    stream << static_cast<quint8>(volume.lowerReference) << static_cast<quint8>(volume.upperReference);
    return stream << volume.lowerReferenceCode << volume.upperReferenceCode << volume.feet;
}

QDataStream &operator>>(QDataStream &stream, GeoZone::Volume &volume)
{
    volume = GeoZone::Volume();
    stream >> volume.isCircle;
    qsizetype ringCount = 0;
    if (!GeoZoneDataStream::readCount(stream, sizeof(quint32), ringCount)) {
        return stream;
    }
    for (qsizetype i = 0; i < ringCount; i++) {
        GeoZone::Ring ring;
        if (!GeoZoneDataStream::readPoints(stream, ring)) {
            return stream;
        }
        volume.rings.append(ring);
    }
    double latitude = qQNaN();
    double longitude = qQNaN();
    double altitude = qQNaN();
    stream >> latitude >> longitude >> altitude >> volume.radius >> volume.lowerLimit >> volume.upperLimit;
    // Set one at a time, the constructor drops coordinates out of range
    volume.center.setLatitude(latitude);
    volume.center.setLongitude(longitude);
    volume.center.setAltitude(altitude);
    if (!GeoZoneDataStream::readEnum(stream, volume.lowerReference, GeoZone::VerticalReference::FlightLevel) ||
        !GeoZoneDataStream::readEnum(stream, volume.upperReference, GeoZone::VerticalReference::FlightLevel)) {
        return stream;
    }
    return stream >> volume.lowerReferenceCode >> volume.upperReferenceCode >> volume.feet;
}

QDataStream &operator<<(QDataStream &stream, const GeoZone::Data &data)
{
    stream << data.identifier << data.country << data.name << data.restrictionCode << static_cast<quint8>(data.restriction);
    GeoZoneDataStream::writeList(stream, data.reasons);
    stream << data.otherReasonInfo << data.restrictionConditions << data.message;
    GeoZoneDataStream::writeList(stream, data.authorities);
    GeoZoneDataStream::writeList(stream, data.applicability);
    GeoZoneDataStream::writeList(stream, data.volumes);
    stream << data.uSpaceClass << data.regulationExemption;
    GeoZoneDataStream::writeList(stream, data.extendedProperties);
    GeoZoneDataStream::writeList(stream, data.issues);
    return stream << data.contentHash;
}

QDataStream &operator>>(QDataStream &stream, GeoZone::Data &data)
{
    data = GeoZone::Data();
    stream >> data.identifier >> data.country >> data.name >> data.restrictionCode;
    if (!GeoZoneDataStream::readEnum(stream, data.restriction, GeoZone::Unknown) || !GeoZoneDataStream::readStrings(stream, data.reasons)) {
        return stream;
    }
    stream >> data.otherReasonInfo >> data.restrictionConditions >> data.message;
    // Periods take at least their flag, two dates of a day, a time and a time spec each, and a count; volumes their
    // flag, count, four coordinates, two limits, references, codes and unit
    if (!GeoZoneDataStream::readStrings(stream, data.authorities) || !GeoZoneDataStream::readList(stream, data.applicability, 31) ||
        !GeoZoneDataStream::readList(stream, data.volumes, 63)) {
        return stream;
    }
    stream >> data.uSpaceClass >> data.regulationExemption;
    if (!GeoZoneDataStream::readStrings(stream, data.extendedProperties) || !GeoZoneDataStream::readStrings(stream, data.issues)) {
        return stream;
    }
    return stream >> data.contentHash;
}
