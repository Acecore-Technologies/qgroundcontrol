/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneParserUtils.h"
#include "GeoZoneParser.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimeZone>
#include <QtCore/QtMath>

#include <algorithm>
#include <cmath>

namespace GeoZoneParserUtils {

namespace {

constexpr double kMetersPerDegree = 6371008.8 * M_PI / 180.0;

bool _samePosition(const QPointF &a, const QPointF &b)
{
    const double north = (b.y() - a.y()) * kMetersPerDegree;
    const double east = std::remainder(b.x() - a.x(), 360.0) * kMetersPerDegree * std::cos(qDegreesToRadians(a.y()));
    return ((north * north) + (east * east)) < (kDuplicateVertexMeters * kDuplicateVertexMeters);
}

/// @return Largest distance of the points from the line through the first one and the one farthest from it
double _width(const QList<QPointF> &points)
{
    const QPointF &origin = points.constFirst();
    QPointF axis;
    for (const QPointF &point : points) {
        if (QPointF::dotProduct(point - origin, point - origin) > QPointF::dotProduct(axis, axis)) {
            axis = point - origin;
        }
    }
    const double length = std::hypot(axis.x(), axis.y());
    double width = 0;
    for (const QPointF &point : points) {
        const QPointF offset = point - origin;
        width = (std::max)(width, (length > 0) ? (std::abs((axis.x() * offset.y()) - (axis.y() * offset.x())) / length) : 0.0);
    }
    return width;
}

enum class RingProblem {
    None,
    TooFewPoints,
    NoArea,
    Pole,
};

/// Removes the duplicate vertices of the ring, checks it and orients it, counterclockwise when outer
/// @param selfIntersecting Set when the ring crosses or touches itself
RingProblem _normalizeRing(GeoZone::Ring &ring, bool outer, bool &selfIntersecting)
{
    selfIntersecting = false;
    GeoZone::Ring distinct;
    distinct.reserve(ring.count());
    for (const QPointF &vertex : std::as_const(ring)) {
        if (distinct.isEmpty() || !_samePosition(distinct.constLast(), vertex)) {
            distinct.append(vertex);
        }
    }
    while ((distinct.count() > 1) && _samePosition(distinct.constLast(), distinct.constFirst())) {
        distinct.removeLast();
    }
    if (distinct.count() != ring.count()) {
        ring = distinct;
    }
    if (ring.count() < 3) {
        return RingProblem::TooFewPoints;
    }

    // The planar geometry of the zones doesn't hold at the poles. A ring around a pole winds once around the world, so
    // its longitudes don't come back to where they started.
    double winding = 0;
    for (qsizetype i = 0; i < ring.count(); i++) {
        if (std::abs(ring[i].y()) > kMaxPolygonLatitude) {
            return RingProblem::Pole;
        }
        winding += std::remainder(ring[(i + 1) % ring.count()].x() - ring[i].x(), 360.0);
    }
    if (std::abs(winding) > 180) {
        return RingProblem::Pole;
    }

    const GeoZoneGeometry::LocalProjection projection(GeoZoneBox::fromLonLatPath(ring).center());
    const QList<QPointF> points = projection.lonLatToLocal(ring);
    const double area = GeoZoneGeometry::signedArea(points);
    selfIntersecting = !GeoZoneGeometry::isSimple(points);
    // A ring crossing itself may enclose area even when the parts it winds around in opposite directions cancel out
    if ((std::abs(area) < kMinRingAreaSquareMeters) && (!selfIntersecting || (_width(points) < 1))) {
        return RingProblem::NoArea;
    }
    if (outer ? (area < 0) : (area > 0)) {
        std::reverse(ring.begin(), ring.end());
    }
    return RingProblem::None;
}

} // namespace

/// Preferred language of localized texts for the parse running on this thread
thread_local QString t_language;

const QString &language()
{
    return t_language;
}

LanguageScope::LanguageScope(const QString &language)
{
    t_language = language.left(2).toLower();
}

LanguageScope::~LanguageScope()
{
    t_language.clear();
}

thread_local const GeoZoneCancelToken *t_cancelToken = nullptr;

CancelScope::CancelScope(const GeoZoneCancelToken &cancelToken)
{
    t_cancelToken = &cancelToken;
}

CancelScope::~CancelScope()
{
    t_cancelToken = nullptr;
}

bool cancelled()
{
    return t_cancelToken && t_cancelToken->isCancelled();
}

/// Returns text from either a plain string or an ED-318 localized text array ([{ "text": "...", "lang": "en-GB" }])
QString _text(const QJsonValue &value)
{
    if (value.isString()) {
        return value.toString().trimmed();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'g', 15);
    }
    if (value.isObject()) {
        return value.toObject().value(QStringLiteral("text")).toString().trimmed();
    }
    if (!value.isArray()) {
        return QString();
    }

    const QString &uiLanguage = t_language;
    QString uiText;
    QString englishText;
    QString firstText;
    QStringList plainStrings;
    for (const QJsonValue &item : value.toArray()) {
        if (item.isString()) {
            plainStrings.append(item.toString().trimmed());
            continue;
        }
        const QJsonObject textObject = item.toObject();
        const QString text = textObject.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty()) {
            continue;
        }
        const QString language = textObject.value(QStringLiteral("lang")).toString().left(2).toLower();
        if (uiText.isEmpty() && !uiLanguage.isEmpty() && (language == uiLanguage)) {
            uiText = text;
        }
        if (englishText.isEmpty() && (language == QStringLiteral("en"))) {
            englishText = text;
        }
        if (firstText.isEmpty()) {
            firstText = text;
        }
    }

    if (!uiText.isEmpty()) {
        return uiText;
    }
    if (!englishText.isEmpty()) {
        return englishText;
    }
    if (!firstText.isEmpty()) {
        return firstText;
    }
    plainStrings.removeAll(QString());
    return plainStrings.join(QStringLiteral("; "));
}

QStringList _stringList(const QJsonValue &value)
{
    QStringList list;
    if (value.isString()) {
        list.append(value.toString().trimmed());
    } else if (value.isArray()) {
        for (const QJsonValue &item : value.toArray()) {
            list.append(item.toString().trimmed());
        }
    }
    list.removeAll(QString());
    return list;
}

double _number(const QJsonValue &value)
{
    double number = qQNaN();
    if (value.isDouble()) {
        number = value.toDouble();
    } else if (value.isString()) {
        bool ok = false;
        number = value.toString().toDouble(&ok);
        if (!ok) {
            number = qQNaN();
        }
    }
    return std::isfinite(number) ? number : qQNaN();
}

/// Parses ISO 8601 dates and times. Times without an offset are UTC in both standards, dates start at midnight UTC.
QDateTime _dateTime(const QJsonValue &value)
{
    const QString string = value.toString().trimmed();
    if (string.isEmpty()) {
        return QDateTime();
    }

    // The time follows a T or a space after the date
    static const QRegularExpression timeSeparatorRegExp(QStringLiteral("^[^Tt ]{8,}[Tt ]"));
    const QRegularExpressionMatch separatorMatch = timeSeparatorRegExp.match(string);
    if (!separatorMatch.hasMatch()) {
        const QDate date = QDate::fromString(string, Qt::ISODate);
        return date.isValid() ? QDateTime(date, QTime(0, 0), QTimeZone::UTC) : QDateTime();
    }

    // Parsed without an offset, Qt would use the local time zone and shift times in its daylight saving gap
    static const QRegularExpression offsetRegExp(QStringLiteral("(Z|[+-]\\d{2}(:?\\d{2})?)$"), QRegularExpression::CaseInsensitiveOption);
    QString isoString = string;
    isoString[separatorMatch.capturedEnd() - 1] = QLatin1Char('T');
    if (!offsetRegExp.match(isoString.mid(separatorMatch.capturedEnd())).hasMatch()) {
        isoString += QLatin1Char('Z');
    }

    const QDateTime dateTime = QDateTime::fromString(isoString, Qt::ISODateWithMs);
    return dateTime.isValid() ? dateTime.toUTC() : QDateTime();
}

/// Parses ED-269/ED-318 times of day such as "06:00Z", "05:00:00.00Z" or "08:00+02:00". The time is returned
/// as published, utcOffsetSecs is set to its offset from UTC.
QTime _time(const QJsonValue &value, int &utcOffsetSecs)
{
    utcOffsetSecs = 0;

    static const QRegularExpression timeRegExp(QStringLiteral("^(\\d{1,2}):(\\d{2})(?::(\\d{2})(?:\\.\\d+)?)?\\s*(Z|[+-]\\d{2}(?::?\\d{2})?)?$"),
                                               QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch match = timeRegExp.match(value.toString().trimmed());
    if (!match.hasMatch()) {
        return QTime();
    }

    QTime time(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
    if (!time.isValid()) {
        // 24:00 is a valid end of day in ISO 8601
        if ((match.captured(1).toInt() != 24) || (match.captured(2).toInt() != 0)) {
            return QTime();
        }
        time = QTime(23, 59, 59);
    }

    const QString offset = match.captured(4);
    if (!offset.isEmpty() && (offset.toUpper() != QStringLiteral("Z"))) {
        const int sign = offset.startsWith(QLatin1Char('-')) ? -1 : 1;
        const QString digits = QString(offset.mid(1)).remove(QLatin1Char(':'));
        utcOffsetSecs = sign * ((digits.left(2).toInt() * 3600) + (digits.mid(2, 2).toInt() * 60));
    }
    return time;
}

QList<GeoZone::TimePeriod> _applicability(const QJsonValue &value, QStringList &issues)
{
    static const QHash<QString, int> dayMap = {
        { QStringLiteral("MON"), Qt::Monday },
        { QStringLiteral("TUE"), Qt::Tuesday },
        { QStringLiteral("WED"), Qt::Wednesday },
        { QStringLiteral("THU"), Qt::Thursday },
        { QStringLiteral("FRI"), Qt::Friday },
        { QStringLiteral("SAT"), Qt::Saturday },
        { QStringLiteral("SUN"), Qt::Sunday },
    };

    QList<GeoZone::TimePeriod> periods;
    for (const QJsonValue &periodValue : value.toArray()) {
        const QJsonObject periodObject = periodValue.toObject();

        GeoZone::TimePeriod period;
        period.permanent = periodObject.value(QStringLiteral("permanent")).toString().compare(QStringLiteral("YES"), Qt::CaseInsensitive) == 0;
        period.start = _dateTime(periodObject.value(QStringLiteral("startDateTime")));
        period.end = _dateTime(periodObject.value(QStringLiteral("endDateTime")));

        for (const QJsonValue &dailyValue : periodObject.value(QStringLiteral("schedule")).toArray()) {
            const QJsonObject dailyObject = dailyValue.toObject();

            GeoZone::DailyPeriod dailyPeriod;
            bool anyDay = false;
            for (const QString &day : _stringList(dailyObject.value(QStringLiteral("day")))) {
                const QString dayCode = day.toUpper();
                if (dayMap.contains(dayCode)) {
                    dailyPeriod.days.append(dayMap.value(dayCode));
                } else {
                    // ANY, or an unknown code which is treated conservatively as any day
                    anyDay = true;
                    if (dayCode != QStringLiteral("ANY")) {
                        issues.append(GeoZoneParser::tr("Unknown day %1 in the schedule, taken as every day").arg(day));
                    }
                }
            }
            if (anyDay) {
                dailyPeriod.days.clear();
            }
            int startOffsetSecs = 0;
            int endOffsetSecs = 0;
            dailyPeriod.startTime = _time(dailyObject.value(QStringLiteral("startTime")), startOffsetSecs);
            dailyPeriod.endTime = _time(dailyObject.value(QStringLiteral("endTime")), endOffsetSecs);
            // Express both times in the offset of the start time
            dailyPeriod.utcOffsetSecs = dailyPeriod.startTime.isValid() ? startOffsetSecs : endOffsetSecs;
            if (dailyPeriod.endTime.isValid() && (endOffsetSecs != dailyPeriod.utcOffsetSecs)) {
                dailyPeriod.endTime = dailyPeriod.endTime.addSecs(dailyPeriod.utcOffsetSecs - endOffsetSecs);
            }
            dailyPeriod.startEvent = dailyObject.value(QStringLiteral("startEvent")).toString();
            dailyPeriod.endEvent = dailyObject.value(QStringLiteral("endEvent")).toString();
            period.schedule.append(dailyPeriod);
        }

        periods.append(period);
    }
    return periods;
}

QStringList _authorities(const QJsonValue &value)
{
    static const QHash<QString, const char*> purposeNames = {
        { QStringLiteral("AUTHORIZATION"),  QT_TRANSLATE_NOOP("GeoZoneParser", "Authorisation") },
        { QStringLiteral("AUTHORISATION"),  QT_TRANSLATE_NOOP("GeoZoneParser", "Authorisation") },
        { QStringLiteral("NOTIFICATION"),   QT_TRANSLATE_NOOP("GeoZoneParser", "Notification") },
        { QStringLiteral("INFORMATION"),    QT_TRANSLATE_NOOP("GeoZoneParser", "Information") },
    };

    QStringList authorities;
    for (const QJsonValue &authorityValue : value.toArray()) {
        const QJsonObject authority = authorityValue.toObject();

        QStringList names = { _text(authority.value(QStringLiteral("name"))), _text(authority.value(QStringLiteral("service"))) };
        names.removeAll(QString());

        QString siteUrl = _text(authority.value(QStringLiteral("siteURL")));
        if (siteUrl.isEmpty()) {
            siteUrl = _text(authority.value(QStringLiteral("siteUrl")));
        }
        QStringList contacts = { _text(authority.value(QStringLiteral("contactName"))),
                                 _text(authority.value(QStringLiteral("email"))),
                                 _text(authority.value(QStringLiteral("phone"))),
                                 siteUrl };
        contacts.removeAll(QString());

        QString line = names.join(QStringLiteral(" / "));
        if (!contacts.isEmpty()) {
            line = line.isEmpty() ? contacts.join(QStringLiteral(", ")) : QStringLiteral("%1 - %2").arg(line, contacts.join(QStringLiteral(", ")));
        }

        const QString purpose = authority.value(QStringLiteral("purpose")).toString().toUpper();
        const auto purposeIt = purposeNames.constFind(purpose);
        if (purposeIt != purposeNames.constEnd()) {
            line = line.isEmpty() ? GeoZoneParser::tr(purposeIt.value()) : QStringLiteral("%1: %2").arg(GeoZoneParser::tr(purposeIt.value()), line);
        }

        const QString intervalBefore = _text(authority.value(QStringLiteral("intervalBefore")));
        if (!intervalBefore.isEmpty()) {
            line += QStringLiteral(" (%1)").arg(GeoZoneParser::tr("notice %1").arg(intervalBefore));
        }

        if (!line.isEmpty()) {
            authorities.append(line);
        }
    }
    return authorities;
}

bool _position(const QJsonValue &value, QGeoCoordinate &coordinate)
{
    const QJsonArray position = value.toArray();
    if (position.count() < 2) {
        return false;
    }
    // GeoJSON and ED-269 both use [longitude, latitude] order
    coordinate = QGeoCoordinate(_number(position[1]), _number(position[0]));
    return coordinate.isValid();
}

bool _ring(const QJsonValue &value, GeoZone::Ring &ring)
{
    ring.clear();
    const QJsonArray positions = value.toArray();
    if (positions.count() > kMaxRingVertices) {
        qCWarning(GeoZoneParserLog) << "Ignoring ring with" << positions.count() << "vertices";
        return false;
    }
    ring.reserve(positions.count());
    for (const QJsonValue &positionValue : positions) {
        QGeoCoordinate coordinate;
        if (!_position(positionValue, coordinate)) {
            return false;
        }
        ring.append(QPointF(coordinate.longitude(), coordinate.latitude()));
    }
    if ((ring.count() > 1) && (ring.first() == ring.last())) {
        ring.removeLast();
    }
    return !ring.isEmpty();
}

bool _polygonRings(const QJsonValue &value, GeoZone::Volume &volume)
{
    volume.isCircle = false;
    volume.rings.clear();
    const QJsonArray rings = value.toArray();
    for (qsizetype i = 0; i < rings.count(); i++) {
        GeoZone::Ring ring;
        if (!_ring(rings[i], ring)) {
            if (i == 0) {
                return false;
            }
            continue; // A broken hole does not invalidate the outer boundary
        }
        volume.rings.append(ring);
    }
    return !volume.rings.isEmpty();
}

void _appendVolume(const GeoZone::Volume &volume, QList<GeoZone::Volume> &volumes)
{
    if (volumes.count() >= kMaxVolumesPerZone) {
        qCDebug(GeoZoneParserLog) << "Ignoring volumes over" << kMaxVolumesPerZone;
        return;
    }
    volumes.append(volume);
}

bool _validRadius(double radius)
{
    return (radius > 0) && (radius <= kMaxRadiusMeters);
}

bool _normalizeVolumes(GeoZone::Data &zone)
{
    QList<GeoZone::Volume> volumes;
    volumes.reserve(zone.volumes.count());
    for (GeoZone::Volume volume : std::as_const(zone.volumes)) {
        if (volume.isCircle) {
            if (volume.center.isValid() && _validRadius(volume.radius)) {
                volumes.append(volume);
            } else {
                zone.issues.append(GeoZoneParser::tr("Circle without a valid radius ignored"));
            }
            continue;
        }

        bool selfIntersecting = false;
        const RingProblem problem = volume.rings.isEmpty() ? RingProblem::TooFewPoints : _normalizeRing(volume.rings.first(), true, selfIntersecting);
        if (problem != RingProblem::None) {
            switch (problem) {
            case RingProblem::TooFewPoints:
                zone.issues.append(GeoZoneParser::tr("Polygon with fewer than 3 distinct points ignored"));
                break;
            case RingProblem::NoArea:
                zone.issues.append(GeoZoneParser::tr("Polygon without area ignored"));
                break;
            case RingProblem::Pole:
                zone.issues.append(GeoZoneParser::tr("Polygon touching or enclosing a pole ignored"));
                break;
            case RingProblem::None:
                break;
            }
            continue;
        }
        for (qsizetype i = volume.rings.count() - 1; i > 0; i--) {
            bool holeIntersecting = false;
            if (_normalizeRing(volume.rings[i], false, holeIntersecting) == RingProblem::None) {
                selfIntersecting = selfIntersecting || holeIntersecting;
            } else {
                volume.rings.removeAt(i);
                zone.issues.append(GeoZoneParser::tr("Invalid polygon hole ignored, the zone covers it"));
            }
        }
        if (selfIntersecting) {
            zone.issues.append(GeoZoneParser::tr("Polygon boundary crossing itself, areas it encloses twice are outside the zone"));
        }
        volumes.append(volume);
    }
    zone.volumes = volumes;
    return !zone.volumes.isEmpty();
}

/// Adds the volumes described by a GeoJSON-like horizontal geometry. Vertical limits are copied from volumeTemplate.
void _appendHorizontalGeometry(const QJsonObject &geometry, const GeoZone::Volume &volumeTemplate, double radiusScale, QList<GeoZone::Volume> &volumes)
{
    const QString type = geometry.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("Polygon")) {
        GeoZone::Volume volume = volumeTemplate;
        if (_polygonRings(geometry.value(QStringLiteral("coordinates")), volume)) {
            _appendVolume(volume, volumes);
        }
    } else if (type == QStringLiteral("MultiPolygon")) {
        for (const QJsonValue &polygon : geometry.value(QStringLiteral("coordinates")).toArray()) {
            GeoZone::Volume volume = volumeTemplate;
            if (_polygonRings(polygon, volume)) {
                _appendVolume(volume, volumes);
            }
        }
    } else if (type == QStringLiteral("Circle")) {
        // ED-269: { "type": "Circle", "center": [lon, lat], "radius": r }
        GeoZone::Volume volume = volumeTemplate;
        volume.isCircle = true;
        volume.radius = _number(geometry.value(QStringLiteral("radius"))) * radiusScale;
        if (_position(geometry.value(QStringLiteral("center")), volume.center)) {
            _appendVolume(volume, volumes);
        }
    } else if (type == QStringLiteral("Point")) {
        // ED-318: GeoJSON Point with { "extent": { "subType": "Circle", "radius": r } }
        const QJsonObject extent = geometry.value(QStringLiteral("extent")).toObject();
        if (extent.value(QStringLiteral("subType")).toString().compare(QStringLiteral("Circle"), Qt::CaseInsensitive) != 0) {
            return;
        }
        GeoZone::Volume volume = volumeTemplate;
        volume.isCircle = true;
        volume.radius = _number(extent.value(QStringLiteral("radius"))) * radiusScale;
        if (_position(geometry.value(QStringLiteral("coordinates")), volume.center)) {
            _appendVolume(volume, volumes);
        }
    } else {
        qCDebug(GeoZoneParserLog) << "Ignoring unsupported geometry type" << type;
    }
}

bool _lengthUnit(const QJsonValue &value, bool &feet, QStringList &issues)
{
    const QString unit = value.toString().trimmed().toUpper();
    static const QStringList meters = { QString(), QStringLiteral("M"), QStringLiteral("METER"), QStringLiteral("METERS"), QStringLiteral("METRE"), QStringLiteral("METRES") };
    static const QStringList feetUnits = { QStringLiteral("FT"), QStringLiteral("FEET"), QStringLiteral("FOOT") };
    feet = feetUnits.contains(unit);
    if (feet || meters.contains(unit)) {
        return true;
    }
    issues.append(GeoZoneParser::tr("Unknown unit %1, the altitude limits are ignored").arg(value.toString()));
    return false;
}

void _commonProperties(const QJsonObject &object, const QString &restrictionKey, GeoZone::Data &zone)
{
    zone.uSpaceClass = _text(object.value(QStringLiteral("uSpaceClass")));
    zone.regulationExemption = _text(object.value(QStringLiteral("regulationExemption")));
    const QJsonObject extended = object.value(QStringLiteral("extendedProperties")).toObject();
    for (auto it = extended.constBegin(); it != extended.constEnd(); ++it) {
        const QJsonValue &value = it.value();
        const QString text = (value.isObject() || value.isArray()) ?
                                 QString::fromUtf8(value.isObject() ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact) : QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)) :
                                 value.toVariant().toString();
        if (!text.isEmpty()) {
            zone.extendedProperties.append(QStringLiteral("%1: %2").arg(it.key(), text));
        }
    }

    zone.identifier = _text(object.value(QStringLiteral("identifier")));
    zone.country = _text(object.value(QStringLiteral("country")));
    zone.name = _text(object.value(QStringLiteral("name")));
    zone.restrictionCode = _text(object.value(restrictionKey));
    zone.restriction = GeoZone::restrictionFromString(zone.restrictionCode);
    zone.reasons = _stringList(object.value(QStringLiteral("reason")));
    zone.otherReasonInfo = _text(object.value(QStringLiteral("otherReasonInfo")));
    zone.message = _text(object.value(QStringLiteral("message")));
    zone.authorities = _authorities(object.value(QStringLiteral("zoneAuthority")));

    const QJsonValue conditions = object.value(QStringLiteral("restrictionConditions"));
    zone.restrictionConditions = conditions.isArray() ? _stringList(conditions).join(QStringLiteral("; ")) : _text(conditions);
}

} // namespace GeoZoneParserUtils
