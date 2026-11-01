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

#include <cmath>

#include <algorithm>
#include <limits>
#include <optional>

namespace {

constexpr double kUnlimitedSentinel = 99999.0; // Published as upper limit when a zone has no ceiling
constexpr double kMetersPerFoot = 0.3048;

/// @return Heights of the positions in the datum of a limit, nothing when they can't be compared
std::optional<std::pair<double, double>> referenceHeights(GeoZone::VerticalReference reference, const GeoZone::Position &from, const GeoZone::Position &to)
{
    double height1 = from.altitudeAMSL;
    double height2 = to.altitudeAMSL;
    switch (reference) {
    case GeoZone::VerticalReference::Ground:
        height1 -= from.terrainAMSL;
        height2 -= to.terrainAMSL;
        break;
    case GeoZone::VerticalReference::Ellipsoid:
        // Heights above the ellipsoid differ from mean sea level by the geoid height of up to about 100 m
        height1 += GeoZoneGeometry::geoidHeight(from.coordinate);
        height2 += GeoZoneGeometry::geoidHeight(to.coordinate);
        break;
    case GeoZone::VerticalReference::MeanSeaLevel:
        break;
    case GeoZone::VerticalReference::FlightLevel:
    case GeoZone::VerticalReference::Unknown:
        // Flight levels depend on the air pressure and unknown references can't be compared
        return std::nullopt;
    }
    if (qIsNaN(height1) || qIsNaN(height2)) {
        return std::nullopt;
    }
    return std::make_pair(height1, height2);
}

/// Zones from the ground have no floor, vehicles on the ground or terrain data errors can be slightly below it
bool hasFloor(const GeoZone::Volume &volume)
{
    return !qIsNaN(volume.lowerLimit) && !((volume.lowerReference == GeoZone::VerticalReference::Ground) && (volume.lowerLimit <= 0));
}

bool hasCeiling(const GeoZone::Volume &volume)
{
    return !qIsNaN(volume.upperLimit) && (volume.upperLimit < kUnlimitedSentinel);
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

GeoZone::GeoZone(const Data &data)
    : _data(data)
{
    _volumeBoxes.reserve(_data.volumes.count());
    for (const Volume &volume : _data.volumes) {
        QRectF ringBox;
        if (!volume.isCircle && !volume.rings.isEmpty() && !volume.rings.first().isEmpty()) {
            double south = 90, north = -90, west = 180, east = -180;
            for (const QGeoCoordinate &coordinate : volume.rings.first()) {
                south = (std::min)(south, coordinate.latitude());
                north = (std::max)(north, coordinate.latitude());
                west = (std::min)(west, coordinate.longitude());
                east = (std::max)(east, coordinate.longitude());
            }
            ringBox = QRectF(QPointF(west, south), QPointF(east, north));
        }
        _volumeBoxes.append(ringBox);

        const QGeoRectangle box = volume.boundingBox();
        if (!box.isValid()) {
            continue;
        }
        _boundingBox = _boundingBox.isValid() ? _boundingBox.united(box) : box;
    }

    _active = isActiveAt(QDateTime::currentDateTimeUtc());
    setSourceKey(QString());
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
    return GeoZoneSchedule::isActiveAt(_data.applicability, dateTime, _boundingBox.center());
}

bool GeoZone::isActiveDuring(const QDateTime &start, const QDateTime &end) const
{
    return GeoZoneSchedule::isActiveDuring(_data.applicability, start, end, _boundingBox.center());
}

bool GeoZone::updateActive(const QDateTime &now)
{
    const bool active = isActiveAt(now);
    if (active == _active) {
        return false;
    }
    _active = active;
    return true;
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
        if (GeoZoneGeometry::signedArea(outerRing) > 0) {
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

bool GeoZone::volumeContains(const Volume &volume, const QGeoCoordinate &coordinate)
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
    // Holes are only projected when the outer ring contains the point
    return std::none_of(volume.rings.cbegin() + 1, volume.rings.cend(), [&projection, &point](const QList<QGeoCoordinate> &hole) {
        return GeoZoneGeometry::ringContains(projection.toLocal(hole), point);
    });
}

bool GeoZone::verticalOverlap(const Volume &volume, const Position &from, const Position &to, double margin)
{
    const double scale = volume.feet ? kMetersPerFoot : 1.0;
    if (hasFloor(volume)) {
        const auto heights = referenceHeights(volume.lowerReference, from, to);
        if (heights && ((std::max)(heights->first, heights->second) < ((volume.lowerLimit * scale) - margin))) {
            return false;
        }
    }
    if (hasCeiling(volume)) {
        const auto heights = referenceHeights(volume.upperReference, from, to);
        if (heights && ((std::min)(heights->first, heights->second) > ((volume.upperLimit * scale) + margin))) {
            return false;
        }
    }
    return true;
}

std::optional<std::pair<double, double>> GeoZone::verticalInterval(const Volume &volume, const Position &from, const Position &to, double margin)
{
    const double scale = volume.feet ? kMetersPerFoot : 1.0;
    double first = 0;
    double last = 1;
    // Keeps the fractions at which the height in the datum of the limit is on the inner side of it
    const auto constrain = [&first, &last, &from, &to](VerticalReference reference, double limit, bool upper) {
        const auto heights = referenceHeights(reference, from, to);
        if (!heights) {
            return;
        }
        const double slope = heights->second - heights->first;
        if (std::abs(slope) < 1e-9) {
            if (upper ? (heights->first > limit) : (heights->first < limit)) {
                first = 1;
                last = 0;
            }
            return;
        }
        const double crossing = (limit - heights->first) / slope;
        if (upper == (slope > 0)) {
            last = (std::min)(last, crossing);
        } else {
            first = (std::max)(first, crossing);
        }
    };
    if (hasFloor(volume)) {
        constrain(volume.lowerReference, (volume.lowerLimit * scale) - margin, false);
    }
    if (hasCeiling(volume)) {
        constrain(volume.upperReference, (volume.upperLimit * scale) + margin, true);
    }
    if (first > last) {
        return std::nullopt;
    }
    return std::make_pair(first, last);
}

double GeoZone::limitAMSL(const Volume &volume, bool upper, const Position &position)
{
    const double limit = upper ? volume.upperLimit : volume.lowerLimit;
    const VerticalReference reference = upper ? volume.upperReference : volume.lowerReference;
    if (qIsNaN(limit) || (upper && (limit >= kUnlimitedSentinel)) || (!upper && (reference == VerticalReference::Ground) && (limit <= 0))) {
        return qQNaN();
    }

    const double meters = limit * (volume.feet ? kMetersPerFoot : 1.0);
    switch (reference) {
    case VerticalReference::Ground:
        return meters + position.terrainAMSL;
    case VerticalReference::MeanSeaLevel:
        return meters;
    case VerticalReference::Ellipsoid:
        return meters - GeoZoneGeometry::geoidHeight(position.coordinate);
    case VerticalReference::FlightLevel:
    case VerticalReference::Unknown:
        break;
    }
    return qQNaN();
}

bool GeoZone::contains(const Position &position) const
{
    if (!position.coordinate.isValid() || !_boundingBox.contains(position.coordinate)) {
        return false;
    }

    return std::any_of(_data.volumes.cbegin(), _data.volumes.cend(), [this, &position](const Volume &volume) {
        return verticalOverlap(volume, position, position) && volumeContains(volume, position.coordinate);
    });
}

double GeoZone::distanceTo(const Position &position, double verticalMargin, double maxDistance) const
{
    const GeoZoneGeometry::LocalProjection projection(position.coordinate);
    const QPointF origin(0, 0);
    double minDistance = std::numeric_limits<double>::infinity();
    for (qsizetype i = 0; i < _data.volumes.count(); i++) {
        const Volume &volume = _data.volumes[i];
        if (!verticalOverlap(volume, position, position, verticalMargin)) {
            continue;
        }
        if (volume.isCircle) {
            if (volume.center.isValid()) {
                minDistance = (std::min)(minDistance, (std::max)(0.0, volume.center.distanceTo(position.coordinate) - volume.radius));
            }
            continue;
        }
        if (volume.rings.isEmpty() || (_boxDistance(_volumeBoxes[i], projection) > (std::min)(minDistance, maxDistance))) {
            continue;
        }

        QList<QList<QPointF>> rings;
        rings.reserve(volume.rings.count());
        for (const QList<QGeoCoordinate> &ring : volume.rings) {
            rings.append(projection.toLocal(ring));
        }
        if (GeoZoneGeometry::polygonContains(rings, origin)) {
            return 0;
        }
        for (const QList<QPointF> &ring : std::as_const(rings)) {
            minDistance = (std::min)(minDistance, GeoZoneGeometry::distanceToRing(origin, ring));
        }
    }
    return minDistance;
}

double GeoZone::_boxDistance(const QRectF &box, const GeoZoneGeometry::LocalProjection &projection)
{
    // Distance from the projection origin to the box corners projected around it, close enough to skip far volumes
    const QPointF southWest = projection.toLocal(QGeoCoordinate(box.top(), box.left()));
    const QPointF northEast = projection.toLocal(QGeoCoordinate(box.bottom(), box.right()));
    const double dx = (std::max)({ southWest.x(), -northEast.x(), 0.0 });
    const double dy = (std::max)({ southWest.y(), -northEast.y(), 0.0 });
    return std::hypot(dx, dy);
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
    if (path.isEmpty() || !_boundingBox.isValid()) {
        return std::nullopt;
    }
    if (path.count() == 1) {
        return contains(path.first()) ? std::make_optional(PathEntry{ 0, 0, path.first() }) : std::nullopt;
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
            projection.emplace(from.coordinate);
            for (const Volume &volume : _data.volumes) {
                QList<QList<QPointF>> rings;
                for (const QList<QGeoCoordinate> &ring : volume.rings) {
                    rings.append(projection->toLocal(ring));
                }
                projectedRings.append(rings);
            }
        }

        // The earliest fraction of the segment inside a volume horizontally and vertically
        const QPointF a = projection->toLocal(from.coordinate);
        const QPointF b = projection->toLocal(to.coordinate);
        double entry = qInf();
        for (qsizetype v = 0; v < _data.volumes.count(); v++) {
            const Volume &volume = _data.volumes[v];
            const auto vertical = verticalInterval(volume, from, to);
            if (!vertical || (vertical->first >= entry)) {
                continue;
            }

            QList<GeoZoneGeometry::Interval> horizontal;
            if (volume.isCircle) {
                if (!volume.center.isValid()) {
                    continue;
                }
                const GeoZoneGeometry::LocalProjection centerProjection(volume.center);
                if (const auto interval = GeoZoneGeometry::segmentCircleInterval(centerProjection.toLocal(from.coordinate), centerProjection.toLocal(to.coordinate), volume.radius)) {
                    horizontal.append(*interval);
                }
            } else {
                horizontal = GeoZoneGeometry::segmentPolygonIntervals(a, b, projectedRings[v]);
            }
            for (const GeoZoneGeometry::Interval &interval : std::as_const(horizontal)) {
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
        result.position.coordinate = (entry <= 0) ? from.coordinate : ((entry >= 1) ? to.coordinate : projection->toGeo(a + ((b - a) * entry)));
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
    QStringList lines;
    for (const Volume &volume : _data.volumes) {
        QString line;
        if (qIsNaN(volume.lowerLimit) && qIsNaN(volume.upperLimit)) {
            line = tr("Not specified");
        } else {
            line = QStringLiteral("%1 - %2").arg(_limitToString(volume.lowerLimit, volume.lowerReference, volume.lowerReferenceCode, volume.feet),
                                                 _limitToString(volume.upperLimit, volume.upperReference, volume.upperReferenceCode, volume.feet));
        }
        if (!lines.contains(line)) {
            lines.append(line);
        }
    }
    return lines.join(QStringLiteral("\n"));
}

QString GeoZone::_limitToString(double limit, VerticalReference reference, const QString &referenceCode, bool feet)
{
    if (qIsNaN(limit)) {
        return tr("not specified");
    }
    if (limit >= kUnlimitedSentinel) {
        return tr("Unlimited");
    }
    if ((limit == 0) && (reference == VerticalReference::Ground)) {
        return tr("GND");
    }

    const QString publishedUnits = feet ? QStringLiteral("ft") : QStringLiteral("m");
    const QString userUnits = FactMetaData::appSettingsVerticalDistanceUnitsString();
    if (userUnits == publishedUnits) {
        return QStringLiteral("%1 %2 %3").arg(limit).arg(publishedUnits, referenceCode);
    }

    // Show the published value too since the converted value is rounded
    const double meters = feet ? (limit * kMetersPerFoot) : limit;
    const double converted = FactMetaData::metersToAppSettingsVerticalDistanceUnits(meters).toDouble();
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
