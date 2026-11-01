/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneParser.h"
#include "QGCLoggingCategory.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimeZone>
#include <QtCore/QXmlStreamReader>

#include <cmath>

QGC_LOGGING_CATEGORY(GeoZoneParserLog, "qgc.geozone.geozoneparser")

namespace {

constexpr double kMetersPerFoot = 0.3048;
constexpr double kUnlimitedSentinel = 99999.0;   ///< Upper limit of airspaces without a ceiling, as in GeoZone
// Bounds keeping a broken or hostile file from stalling the application
constexpr qsizetype kMaxRingVertices = 200000;
constexpr qsizetype kMaxVolumesPerZone = 10000;
constexpr double kMaxRadiusMeters = 1000000;

/// Preferred language of localized texts for the parse running on this thread
thread_local QString t_language;

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

QList<GeoZone::TimePeriod> _applicability(const QJsonValue &value)
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

bool _ring(const QJsonValue &value, QList<QGeoCoordinate> &ring)
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
        ring.append(coordinate);
    }
    if ((ring.count() > 1) && (ring.first() == ring.last())) {
        ring.removeLast();
    }
    return ring.count() >= 3;
}

bool _polygonRings(const QJsonValue &value, GeoZone::Volume &volume)
{
    volume.isCircle = false;
    volume.rings.clear();
    const QJsonArray rings = value.toArray();
    for (qsizetype i = 0; i < rings.count(); i++) {
        QList<QGeoCoordinate> ring;
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
        if (_position(geometry.value(QStringLiteral("center")), volume.center) && _validRadius(volume.radius)) {
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
        if (_position(geometry.value(QStringLiteral("coordinates")), volume.center) && _validRadius(volume.radius)) {
            _appendVolume(volume, volumes);
        }
    } else {
        qCDebug(GeoZoneParserLog) << "Ignoring unsupported geometry type" << type;
    }
}

GeoZone::Volume _ed318Layer(const QJsonValue &value, const GeoZone::Volume &fallback)
{
    if (!value.isObject()) {
        return fallback;
    }

    const QJsonObject layer = value.toObject();
    GeoZone::Volume volume;
    volume.feet = layer.value(QStringLiteral("uom")).toString().compare(QStringLiteral("ft"), Qt::CaseInsensitive) == 0;
    volume.lowerLimit = _number(layer.value(QStringLiteral("lower")));
    volume.upperLimit = _number(layer.value(QStringLiteral("upper")));
    volume.setReferences(layer.value(QStringLiteral("lowerReference")).toString(), layer.value(QStringLiteral("upperReference")).toString());
    return volume;
}

void _ed318Geometry(const QJsonObject &geometry, const GeoZone::Volume &parentLayer, QList<GeoZone::Volume> &volumes)
{
    const GeoZone::Volume layer = _ed318Layer(geometry.value(QStringLiteral("layer")), parentLayer);

    if (geometry.value(QStringLiteral("type")).toString() == QStringLiteral("GeometryCollection")) {
        for (const QJsonValue &child : geometry.value(QStringLiteral("geometries")).toArray()) {
            _ed318Geometry(child.toObject(), layer, volumes);
        }
        return;
    }

    // ED-318 circle radius is always expressed in meters, the layer uom only applies to vertical limits
    _appendHorizontalGeometry(geometry, layer, 1.0, volumes);
}

void _commonProperties(const QJsonObject &object, const QString &restrictionKey, GeoZone::Data &zone)
{
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

bool _ed269Feature(const QJsonObject &feature, GeoZone::Data &zone)
{
    _commonProperties(feature, QStringLiteral("restriction"), zone);
    zone.applicability = _applicability(feature.value(QStringLiteral("applicability")));

    for (const QJsonValue &geometryValue : feature.value(QStringLiteral("geometry")).toArray()) {
        const QJsonObject geometry = geometryValue.toObject();

        GeoZone::Volume layer;
        layer.feet = geometry.value(QStringLiteral("uomDimensions")).toString().compare(QStringLiteral("FT"), Qt::CaseInsensitive) == 0;
        layer.lowerLimit = _number(geometry.value(QStringLiteral("lowerLimit")));
        layer.upperLimit = _number(geometry.value(QStringLiteral("upperLimit")));
        layer.setReferences(geometry.value(QStringLiteral("lowerVerticalReference")).toString(), geometry.value(QStringLiteral("upperVerticalReference")).toString());

        // ED-269 expresses all dimensions, including the circle radius, in uomDimensions
        _appendHorizontalGeometry(geometry.value(QStringLiteral("horizontalProjection")).toObject(), layer, layer.feet ? kMetersPerFoot : 1.0, zone.volumes);
    }

    return !zone.volumes.isEmpty();
}

bool _ed318Feature(const QJsonObject &feature, GeoZone::Data &zone)
{
    const QJsonObject properties = feature.value(QStringLiteral("properties")).toObject();
    _commonProperties(properties, QStringLiteral("type"), zone);
    if (zone.identifier.isEmpty()) {
        zone.identifier = _text(feature.value(QStringLiteral("id")));
    }
    zone.applicability = _applicability(properties.value(QStringLiteral("limitedApplicability")));

    _ed318Geometry(feature.value(QStringLiteral("geometry")).toObject(), GeoZone::Volume(), zone.volumes);

    return !zone.volumes.isEmpty();
}

/// FAA temporary flight restriction from the tfr.faa.gov WFS GeoJSON, which has no times or vertical limits
bool _faaTfrFeature(const QJsonObject &feature, GeoZone::Data &zone)
{
    const QJsonObject properties = feature.value(QStringLiteral("properties")).toObject();
    const QString notamKey = _text(properties.value(QStringLiteral("NOTAM_KEY")));
    // Keys look like 6/4240-1-FDC-F, the NOTAM number is the part before the first dash
    const QString notam = notamKey.section(QLatin1Char('-'), 0, 0);
    zone.identifier = notamKey;
    zone.country = QStringLiteral("USA");
    zone.name = _text(properties.value(QStringLiteral("TITLE")));
    if (zone.name.isEmpty()) {
        zone.name = GeoZoneParser::tr("TFR %1").arg(notam);
    }
    zone.restrictionCode = QStringLiteral("TFR");
    // Drone flights in most TFRs are prohibited unless specifically authorized
    zone.restriction = GeoZone::Prohibited;
    const QString type = _text(properties.value(QStringLiteral("LEGAL")));
    if (!type.isEmpty()) {
        zone.reasons.append(QStringLiteral("OTHER"));
        zone.otherReasonInfo = type;
    }
    zone.message = GeoZoneParser::tr("Temporary flight restriction, FAA NOTAM %1. The NOTAM defines the times, altitudes and exceptions.").arg(notam);
    zone.authorities.append(QStringLiteral("FAA"));

    _appendHorizontalGeometry(feature.value(QStringLiteral("geometry")).toObject(), GeoZone::Volume(), 1.0, zone.volumes);
    return !zone.volumes.isEmpty();
}

/// Restriction, ED-269 reason and English description of the DiPUL zone types
struct DipulType {
    GeoZone::Restriction restriction;
    const char *reason;
    const char *description;
};

const QHash<QString, DipulType> &_dipulTypes()
{
    static const QHash<QString, DipulType> types = {
        { QStringLiteral("FLUGBESCHRAENKUNGSGEBIET"),   { GeoZone::Prohibited,          "AIR_TRAFFIC",  QT_TRANSLATE_NOOP("GeoZoneParser", "Restricted area") } },
        { QStringLiteral("U_NFZ"),                      { GeoZone::Prohibited,          "OTHER",        QT_TRANSLATE_NOOP("GeoZoneParser", "Temporary no-fly zone") } },
        { QStringLiteral("KONTROLLZONE"),               { GeoZone::ReqAuthorisation,    "AIR_TRAFFIC",  QT_TRANSLATE_NOOP("GeoZoneParser", "Control zone") } },
        { QStringLiteral("FLUGHAFEN"),                  { GeoZone::ReqAuthorisation,    "AIR_TRAFFIC",  QT_TRANSLATE_NOOP("GeoZoneParser", "Airport") } },
        { QStringLiteral("FLUGPLATZ"),                  { GeoZone::ReqAuthorisation,    "AIR_TRAFFIC",  QT_TRANSLATE_NOOP("GeoZoneParser", "Aerodrome") } },
        { QStringLiteral("JUSTIZVOLLZUGSANSTALT"),      { GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "Prison") } },
        { QStringLiteral("MILITAERISCHE_ANLAGE"),       { GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "Military facility") } },
        { QStringLiteral("KRAFTWERK"),                  { GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "Power plant") } },
        { QStringLiteral("INDUSTRIEANLAGE"),            { GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "Industrial facility") } },
        { QStringLiteral("UMSPANNWERK"),                { GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "Substation") } },
        { QStringLiteral("STROMLEITUNG"),               { GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "Power line") } },
        { QStringLiteral("LABOR"),                      { GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "Laboratory") } },
        { QStringLiteral("BEHOERDE"),                   { GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "Government building") } },
        { QStringLiteral("SICHERHEITSBEHOERDE"),        { GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "Security authority") } },
        { QStringLiteral("POLIZEI"),                    { GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "Police") } },
        { QStringLiteral("DIPLOMATISCHE_VERTRETUNG"),   { GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "Diplomatic mission") } },
        { QStringLiteral("INTERNATIONALE_ORGANISATION"),{ GeoZone::ReqAuthorisation,    "SENSITIVE",    QT_TRANSLATE_NOOP("GeoZoneParser", "International organisation") } },
        { QStringLiteral("KRANKENHAUS"),                { GeoZone::ReqAuthorisation,    "EMERGENCY",    QT_TRANSLATE_NOOP("GeoZoneParser", "Hospital") } },
        { QStringLiteral("NATIONALPARK"),               { GeoZone::ReqAuthorisation,    "NATURE",       QT_TRANSLATE_NOOP("GeoZoneParser", "National park") } },
        { QStringLiteral("NATURSCHUTZGEBIET"),          { GeoZone::ReqAuthorisation,    "NATURE",       QT_TRANSLATE_NOOP("GeoZoneParser", "Nature reserve") } },
        { QStringLiteral("VOGELSCHUTZGEBIET"),          { GeoZone::ReqAuthorisation,    "NATURE",       QT_TRANSLATE_NOOP("GeoZoneParser", "Bird sanctuary") } },
        { QStringLiteral("WOHNGRUNDSTUECK"),            { GeoZone::Conditional,         "PRIVACY",      QT_TRANSLATE_NOOP("GeoZoneParser", "Residential area") } },
        { QStringLiteral("FREIBAD"),                    { GeoZone::Conditional,         "POPULATION",   QT_TRANSLATE_NOOP("GeoZoneParser", "Outdoor pool or beach") } },
        { QStringLiteral("BAHNANLAGE"),                 { GeoZone::Conditional,         "OTHER",        QT_TRANSLATE_NOOP("GeoZoneParser", "Railway") } },
        { QStringLiteral("BUNDESAUTOBAHN"),             { GeoZone::Conditional,         "OTHER",        QT_TRANSLATE_NOOP("GeoZoneParser", "Motorway") } },
        { QStringLiteral("BUNDESSTRASSE"),              { GeoZone::Conditional,         "OTHER",        QT_TRANSLATE_NOOP("GeoZoneParser", "Federal road") } },
        { QStringLiteral("BINNENWASSERSTRASSE"),        { GeoZone::Conditional,         "OTHER",        QT_TRANSLATE_NOOP("GeoZoneParser", "Inland waterway") } },
        { QStringLiteral("SEEWASSERSTRASSE"),           { GeoZone::Conditional,         "OTHER",        QT_TRANSLATE_NOOP("GeoZoneParser", "Maritime waterway") } },
        { QStringLiteral("SCHIFFFAHRTSANLAGE"),         { GeoZone::Conditional,         "OTHER",        QT_TRANSLATE_NOOP("GeoZoneParser", "Shipping facility") } },
        { QStringLiteral("WINDKRAFTANLAGE"),            { GeoZone::Conditional,         "OTHER",        QT_TRANSLATE_NOOP("GeoZoneParser", "Wind turbine") } },
    };
    return types;
}

/// Limit in meters from a value, its unit and reference, NaN when missing
struct DipulLimit {
    double meters = qQNaN();
    QString reference;
};

DipulLimit _dipulLimit(const QJsonValue &value, const QString &unit, const QString &reference)
{
    DipulLimit limit;
    const double number = _number(value);
    if (std::isnan(number)) {
        return limit;
    }
    const QString upperUnit = unit.toUpper();
    limit.meters = (upperUnit == QStringLiteral("FT")) ? (number * kMetersPerFoot) : ((upperUnit == QStringLiteral("FL")) ? (number * 100 * kMetersPerFoot) : number);
    limit.reference = reference.toUpper();
    return limit;
}

/// German geographical zone of the DiPUL WFS
bool _dipulFeature(const QJsonObject &feature, GeoZone::Data &zone)
{
    const QJsonObject properties = feature.value(QStringLiteral("properties")).toObject();
    const QString typeCode = _text(properties.value(QStringLiteral("type_code")));
    const auto type = _dipulTypes().constFind(typeCode);

    zone.identifier = _text(properties.value(QStringLiteral("external_reference")));
    if (zone.identifier.isEmpty()) {
        zone.identifier = _text(feature.value(QStringLiteral("id")));
    }
    zone.country = QStringLiteral("DEU");
    const QString germanName = _text(properties.value(QStringLiteral("name")));
    QString englishName = _text(properties.value(QStringLiteral("generated_name_EN")));
    if (englishName.isEmpty()) {
        englishName = _text(properties.value(QStringLiteral("generated_name_en")));
    }
    zone.name = ((t_language == QStringLiteral("de")) || englishName.isEmpty()) ? germanName : englishName;
    zone.restrictionCode = typeCode;
    zone.restriction = (type != _dipulTypes().constEnd()) ? type->restriction : GeoZone::Conditional;
    zone.reasons.append(QString::fromLatin1((type != _dipulTypes().constEnd()) ? type->reason : "OTHER"));
    if (type != _dipulTypes().constEnd()) {
        zone.otherReasonInfo = GeoZoneParser::tr(type->description);
    }
    const QString legalReference = _text(properties.value(QStringLiteral("legal_ref")));
    if (!legalReference.isEmpty()) {
        zone.message = GeoZoneParser::tr("Legal basis: %1").arg(legalReference);
    }
    zone.authorities.append(QStringLiteral("DFS / BMDV (DiPUL)"));

    // Fixed zones give the floor above ground or the sea, the ceiling above the sea or as a flight level. Temporary
    // zones give each limit with its unit and reference.
    DipulLimit lower;
    DipulLimit upper;
    if (properties.contains(QStringLiteral("lower_limit_altitude"))) {
        lower = _dipulLimit(properties.value(QStringLiteral("lower_limit_altitude")), _text(properties.value(QStringLiteral("lower_limit_unit"))),
                            _text(properties.value(QStringLiteral("lower_limit_alt_ref"))));
        upper = _dipulLimit(properties.value(QStringLiteral("upper_limit_altitude")), _text(properties.value(QStringLiteral("upper_limit_unit"))),
                            _text(properties.value(QStringLiteral("upper_limit_alt_ref"))));
    } else {
        lower = _dipulLimit(properties.value(QStringLiteral("lower_limit_m_agl")), QStringLiteral("M"), QStringLiteral("AGL"));
        if (std::isnan(lower.meters)) {
            lower = _dipulLimit(properties.value(QStringLiteral("lower_limit_ft_msl")), QStringLiteral("FT"), QStringLiteral("MSL"));
        }
        upper = _dipulLimit(properties.value(QStringLiteral("upper_limit_ft_msl")), QStringLiteral("FT"), QStringLiteral("MSL"));
        if (std::isnan(upper.meters)) {
            upper = _dipulLimit(properties.value(QStringLiteral("upper_limit_fl_pa")), QStringLiteral("FL"), QStringLiteral("STD"));
        }
    }
    GeoZone::Volume layer;
    layer.lowerLimit = lower.meters;
    layer.upperLimit = upper.meters;
    layer.setReferences(lower.reference, upper.reference);

    const QDateTime start = _dateTime(properties.value(QStringLiteral("start_time")));
    const QDateTime end = _dateTime(properties.value(QStringLiteral("end_time")));
    if (start.isValid() || end.isValid()) {
        GeoZone::TimePeriod period;
        period.start = start;
        period.end = end;
        zone.applicability.append(period);
    }

    _appendHorizontalGeometry(feature.value(QStringLiteral("geometry")).toObject(), layer, 1.0, zone.volumes);
    return !zone.volumes.isEmpty();
}

/// French UAS restriction of the IGN Géoplateforme, which describe the restriction in text: "Vol interdit" or
/// "Hauteur maximale de vol de 50 m", with conditions in a remark
bool _franceFeature(const QJsonObject &feature, GeoZone::Data &zone)
{
    const QJsonObject properties = feature.value(QStringLiteral("properties")).toObject();
    QString limit = _text(properties.value(QStringLiteral("limite")));
    if (limit.endsWith(QLatin1Char('*'))) {
        limit = limit.chopped(1).trimmed();
    }
    zone.identifier = _text(feature.value(QStringLiteral("id")));
    zone.country = QStringLiteral("FRA");
    zone.name = limit.isEmpty() ? GeoZoneParser::tr("UAS restriction") : limit;
    zone.restrictionCode = limit;
    zone.restriction = GeoZone::Prohibited;
    zone.reasons.append(QStringLiteral("OTHER"));
    zone.restrictionConditions = _text(properties.value(QStringLiteral("remarque")));
    zone.message = GeoZoneParser::tr("Open category and model aircraft restriction. Temporary restrictions are published by NOTAM.");
    zone.authorities.append(QStringLiteral("DGAC / IGN"));

    // Flights are prohibited above the maximum height, or entirely
    GeoZone::Volume layer;
    static const QRegularExpression heightRegExp(QStringLiteral("(\\d+)\\s*m\\b"));
    const QRegularExpressionMatch match = heightRegExp.match(limit);
    if (match.hasMatch()) {
        layer.lowerLimit = match.captured(1).toDouble();
        layer.setReferences(QStringLiteral("AGL"), QString());
    }

    _appendHorizontalGeometry(feature.value(QStringLiteral("geometry")).toObject(), layer, 1.0, zone.volumes);
    return !zone.volumes.isEmpty();
}

/// Limit of an AIXM airspace volume as published
struct AixmLimit {
    double value = qQNaN();
    QString uom;
    QString reference;
};

double _aixmDistance(double value, const QString &uom)
{
    const QString unit = uom.toUpper();
    if ((unit == QStringLiteral("[NMI_I]")) || (unit == QStringLiteral("NM"))) {
        return value * 1852;
    }
    if (unit == QStringLiteral("KM")) {
        return value * 1000;
    }
    if ((unit == QStringLiteral("[FT_I]")) || (unit == QStringLiteral("FT"))) {
        return value * kMetersPerFoot;
    }
    return value;
}

/// Sets the limits of the volume, in feet when both are published in feet or as flight levels, in meters otherwise
void _setAixmLimits(GeoZone::Volume &volume, const AixmLimit &lower, const AixmLimit &upper)
{
    const auto inFeet = [](const AixmLimit &limit) { return std::isnan(limit.value) || (limit.uom == QStringLiteral("FT")) || (limit.uom == QStringLiteral("FL")); };
    volume.feet = inFeet(lower) && inFeet(upper);
    const auto convert = [&volume](const AixmLimit &limit) {
        if (std::isnan(limit.value)) {
            return limit.value;
        }
        const double feet = (limit.uom == QStringLiteral("FL")) ? (limit.value * 100) : limit.value;
        if (volume.feet) {
            return feet;
        }
        return ((limit.uom == QStringLiteral("FT")) || (limit.uom == QStringLiteral("FL"))) ? (feet * kMetersPerFoot) : limit.value;
    };
    volume.lowerLimit = convert(lower);
    volume.upperLimit = convert(upper);
    const auto reference = [](const AixmLimit &limit) { return (limit.reference == QStringLiteral("W84")) ? QStringLiteral("WGS84") : limit.reference; };
    volume.setReferences(reference(lower), reference(upper));
}

/// Parses the airspaces of an AIXM 5.1 message, like the UK flight restriction zones. Boundaries of geodesic strings,
/// line strings, arcs and circles by center point are supported, arcs use bearings for their angles.
bool _parseAixm(const QByteArray &xml, GeoZoneParser::Result &result, QString &errorString)
{
    result.format = GeoZoneParser::Format::Aixm;
    result.title = GeoZoneParser::tr("AIXM airspaces");
    result.contentHash = QCryptographicHash::hash(xml, QCryptographicHash::Sha1);

    QXmlStreamReader reader(xml);
    bool inTitle = false;
    bool titleRead = false;
    bool inAirspace = false;
    bool inVolume = false;
    bool inExterior = false;
    GeoZone::Data zone;
    QString type;
    QString localType;
    GeoZone::Volume volume;
    AixmLimit lower;
    AixmLimit upper;
    QList<QGeoCoordinate> ring;
    bool circleOnly = true;
    QString segment;
    QList<QGeoCoordinate> points;
    double radius = qQNaN();
    double startAngle = qQNaN();
    double endAngle = qQNaN();

    const auto appendPoints = [&ring](const QList<QGeoCoordinate> &newPoints) {
        for (const QGeoCoordinate &point : newPoints) {
            if (ring.isEmpty() || (ring.constLast().distanceTo(point) > 0.01)) {
                ring.append(point);
            }
        }
    };
    const auto parsePositions = [](const QString &text) {
        QList<QGeoCoordinate> positions;
        const QStringList values = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (qsizetype i = 0; (i + 1) < values.count(); i += 2) {
            // EPSG:4326 lists latitude first
            const QGeoCoordinate coordinate(values[i].toDouble(), values[i + 1].toDouble());
            if (coordinate.isValid()) {
                positions.append(coordinate);
            }
        }
        return positions;
    };

    while (!reader.atEnd()) {
        reader.readNext();
        const QStringView name = reader.name();
        if (reader.isStartElement()) {
            // Dataset title of the message metadata
            if (!titleRead && !inAirspace && (name == QLatin1String("title"))) {
                inTitle = true;
            } else if (inTitle && (name == QLatin1String("CharacterString"))) {
                const QString title = reader.readElementText().trimmed();
                if (!title.isEmpty()) {
                    result.title = title;
                }
                inTitle = false;
                titleRead = true;
            }
            if (name == QLatin1String("Airspace")) {
                inAirspace = true;
                zone = GeoZone::Data();
                type.clear();
                localType.clear();
            } else if (!inAirspace) {
                continue;
            } else if (name == QLatin1String("AirspaceVolume")) {
                inVolume = true;
                volume = GeoZone::Volume();
                lower = AixmLimit();
                upper = AixmLimit();
                ring.clear();
                circleOnly = true;
            } else if (!inVolume && (name == QLatin1String("type")) && type.isEmpty()) {
                type = reader.readElementText().trimmed();
            } else if (!inVolume && (name == QLatin1String("localType")) && localType.isEmpty()) {
                localType = reader.readElementText().trimmed();
            } else if (!inVolume && (name == QLatin1String("designator")) && zone.identifier.isEmpty()) {
                zone.identifier = reader.readElementText().trimmed();
            } else if (!inVolume && (name == QLatin1String("name")) && zone.name.isEmpty()) {
                zone.name = reader.readElementText().trimmed();
            } else if (inVolume && ((name == QLatin1String("upperLimit")) || (name == QLatin1String("lowerLimit")))) {
                AixmLimit &limit = (name == QLatin1String("upperLimit")) ? upper : lower;
                limit.uom = reader.attributes().value(QStringLiteral("uom")).toString().toUpper();
                const QString text = reader.readElementText().trimmed().toUpper();
                if (text == QStringLiteral("GND")) {
                    limit.value = 0;
                } else if (text == QStringLiteral("UNL")) {
                    limit.value = kUnlimitedSentinel;
                    limit.uom = QStringLiteral("FT");
                } else {
                    limit.value = _number(QJsonValue(text));
                }
            } else if (inVolume && (name == QLatin1String("upperLimitReference"))) {
                upper.reference = reader.readElementText().trimmed().toUpper();
            } else if (inVolume && (name == QLatin1String("lowerLimitReference"))) {
                lower.reference = reader.readElementText().trimmed().toUpper();
            } else if (inVolume && (name == QLatin1String("exterior"))) {
                inExterior = true;
            } else if (inExterior && ((name == QLatin1String("GeodesicString")) || (name == QLatin1String("LineStringSegment")) ||
                                      (name == QLatin1String("ArcByCenterPoint")) || (name == QLatin1String("CircleByCenterPoint")))) {
                segment = name.toString();
                points.clear();
                radius = qQNaN();
                startAngle = qQNaN();
                endAngle = qQNaN();
            } else if (!segment.isEmpty() && ((name == QLatin1String("pos")) || (name == QLatin1String("posList")))) {
                points.append(parsePositions(reader.readElementText()));
            } else if (!segment.isEmpty() && (name == QLatin1String("radius"))) {
                const QString uom = reader.attributes().value(QStringLiteral("uom")).toString();
                radius = _aixmDistance(reader.readElementText().trimmed().toDouble(), uom);
            } else if (!segment.isEmpty() && (name == QLatin1String("startAngle"))) {
                startAngle = reader.readElementText().trimmed().toDouble();
            } else if (!segment.isEmpty() && (name == QLatin1String("endAngle"))) {
                endAngle = reader.readElementText().trimmed().toDouble();
            }
        } else if (reader.isEndElement() && inAirspace) {
            if (!segment.isEmpty() && (name == segment)) {
                if ((segment == QLatin1String("ArcByCenterPoint")) && !points.isEmpty() && (radius > 0) && !std::isnan(startAngle) && !std::isnan(endAngle)) {
                    // The sweep is signed, clockwise when the end bearing is larger
                    const double sweep = endAngle - startAngle;
                    const int steps = (std::max)(1, static_cast<int>(std::ceil(std::abs(sweep) / 3)));
                    QList<QGeoCoordinate> arc;
                    for (int i = 0; i <= steps; i++) {
                        arc.append(points.constFirst().atDistanceAndAzimuth(radius, startAngle + (sweep * i / steps)));
                    }
                    appendPoints(arc);
                    circleOnly = false;
                } else if ((segment == QLatin1String("CircleByCenterPoint")) && !points.isEmpty() && (radius > 0)) {
                    volume.center = points.constFirst();
                    volume.radius = radius;
                } else {
                    appendPoints(points);
                    circleOnly = false;
                }
                segment.clear();
            } else if (name == QLatin1String("exterior")) {
                inExterior = false;
            } else if (name == QLatin1String("AirspaceVolume")) {
                inVolume = false;
                _setAixmLimits(volume, lower, upper);
                if (circleOnly && volume.center.isValid() && _validRadius(volume.radius)) {
                    volume.isCircle = true;
                    _appendVolume(volume, zone.volumes);
                } else {
                    if ((ring.count() > 1) && (ring.constFirst().distanceTo(ring.constLast()) < 1)) {
                        ring.removeLast();
                    }
                    if ((ring.count() >= 3) && (ring.count() <= kMaxRingVertices)) {
                        volume.isCircle = false;
                        volume.rings = { ring };
                        _appendVolume(volume, zone.volumes);
                    }
                }
            } else if (name == QLatin1String("Airspace")) {
                inAirspace = false;
                zone.restrictionCode = localType.isEmpty() ? type : QStringLiteral("%1 %2").arg(type, localType);
                const bool restrictionZone = (localType == QStringLiteral("FRZ")) || (localType == QStringLiteral("RPZ"));
                if (type == QStringLiteral("P")) {
                    zone.restriction = GeoZone::Prohibited;
                } else if ((type == QStringLiteral("R")) || restrictionZone) {
                    zone.restriction = GeoZone::ReqAuthorisation;
                } else {
                    zone.restriction = GeoZone::Conditional;
                }
                zone.reasons.append(restrictionZone ? QStringLiteral("AIR_TRAFFIC") : QStringLiteral("OTHER"));
                if (restrictionZone) {
                    zone.message = GeoZoneParser::tr("Flight restriction zone of a protected aerodrome. Permission from its air traffic control or operator is required.");
                } else if (type == QStringLiteral("D")) {
                    zone.message = GeoZoneParser::tr("Danger area, check its activity before flying.");
                }
                if (zone.identifier.startsWith(QStringLiteral("EG"))) {
                    zone.country = QStringLiteral("GBR");
                    zone.authorities.append(QStringLiteral("UK CAA / NATS"));
                }
                if (zone.volumes.isEmpty()) {
                    result.skippedFeatures++;
                } else {
                    result.zones.append(zone);
                }
            }
        }
    }

    if (reader.hasError()) {
        errorString = GeoZoneParser::tr("Invalid XML: %1 (line %2)").arg(reader.errorString()).arg(reader.lineNumber());
        return false;
    }
    if (result.zones.isEmpty()) {
        errorString = GeoZoneParser::tr("No airspaces with a supported geometry were found in the AIXM file.");
        return false;
    }
    return true;
}

/// @return Dipul or FranceUas for their WFS responses, from the properties of the features or the hint for empty ones
GeoZoneParser::Format _wfsFormat(const QJsonObject &root, const QJsonArray &features, GeoZoneParser::Format formatHint)
{
    if (features.isEmpty()) {
        const bool wfsResponse = root.contains(QStringLiteral("totalFeatures")) || root.contains(QStringLiteral("numberMatched"));
        const bool wfsHint = (formatHint == GeoZoneParser::Format::Dipul) || (formatHint == GeoZoneParser::Format::FranceUas);
        return (wfsResponse && wfsHint) ? formatHint : GeoZoneParser::Format::Unknown;
    }
    const QJsonObject properties = features.first().toObject().value(QStringLiteral("properties")).toObject();
    if (properties.contains(QStringLiteral("type_code")) && properties.contains(QStringLiteral("legal_ref"))) {
        return GeoZoneParser::Format::Dipul;
    }
    if (properties.contains(QStringLiteral("limite"))) {
        return GeoZoneParser::Format::FranceUas;
    }
    return GeoZoneParser::Format::Unknown;
}

bool _isFaaTfr(const QJsonObject &root, const QJsonArray &features, GeoZoneParser::Format formatHint)
{
    if (features.isEmpty()) {
        // An empty list from the FAA means no restrictions are in effect, other GeoServer feeds have the same WFS counts
        return (formatHint == GeoZoneParser::Format::FaaTfr) && root.contains(QStringLiteral("totalFeatures")) && root.contains(QStringLiteral("timeStamp"));
    }
    return features.first().toObject().value(QStringLiteral("properties")).toObject().contains(QStringLiteral("NOTAM_KEY"));
}

} // namespace

bool GeoZoneParser::parseFile(const QString &fileName, Result &result, QString &errorString)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        errorString = tr("Unable to open file %1: %2").arg(fileName, file.errorString());
        return false;
    }
    return parse(file.readAll(), result, errorString);
}

bool GeoZoneParser::parse(const QByteArray &bytes, Result &result, QString &errorString, Format formatHint, const QString &language)
{
    result = Result();
    errorString.clear();

    const struct LanguageScope {
        explicit LanguageScope(const QString &language) { t_language = language.left(2).toLower(); }
        ~LanguageScope() { t_language.clear(); }
    } languageScope(language);

    QByteArray json = bytes;
    // Several national feeds are published with a UTF-8 byte order mark which QJsonDocument rejects
    if (json.startsWith("\xEF\xBB\xBF")) {
        json.remove(0, 3);
    }
    if (json.trimmed().startsWith('<')) {
        return _parseAixm(json, result, errorString);
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        errorString = tr("Invalid JSON: %1 (offset %2)").arg(parseError.errorString()).arg(parseError.offset);
        return false;
    }

    const QJsonObject root = document.object();
    const QJsonValue featuresValue = root.value(QStringLiteral("features"));
    if (!document.isObject() || !featuresValue.isArray()) {
        errorString = tr("Not a geozone file. EUROCAE ED-269 and ED-318 files, AIXM 5.1 airspaces and the FAA, DiPUL and IGN GeoJSON are supported.");
        return false;
    }
    const QJsonArray features = featuresValue.toArray();

    result.contentHash = QCryptographicHash::hash(QJsonDocument(features).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1);

    // A WFS response the server limited, which misses zones
    const double matched = _number(root.value(QStringLiteral("numberMatched")));
    const double returned = _number(root.value(QStringLiteral("numberReturned")));
    if (!std::isnan(matched) && !std::isnan(returned) && (matched > returned)) {
        result.missingFeatures = static_cast<int>(matched - returned);
    }

    const Format wfsFormat = _wfsFormat(root, features, formatHint);
    if (_isFaaTfr(root, features, formatHint)) {
        result.format = Format::FaaTfr;
        result.title = tr("FAA temporary flight restrictions");
        result.provider = QStringLiteral("FAA");
        result.issued = _dateTime(root.value(QStringLiteral("timeStamp")));
    } else if (wfsFormat == Format::Dipul) {
        result.format = Format::Dipul;
        result.title = tr("DiPUL geographical zones (Germany)");
        result.provider = QStringLiteral("DFS / BMDV");
        result.issued = _dateTime(root.value(QStringLiteral("timeStamp")));
    } else if (wfsFormat == Format::FranceUas) {
        result.format = Format::FranceUas;
        result.title = tr("UAS restrictions (France)");
        result.provider = QStringLiteral("DGAC / IGN");
        result.issued = _dateTime(root.value(QStringLiteral("timeStamp")));
    } else if (root.value(QStringLiteral("type")).toString() == QStringLiteral("FeatureCollection")) {
        result.format = Format::ED318;
        result.title = _text(root.value(QStringLiteral("name")));
        if (result.title.isEmpty()) {
            result.title = _text(root.value(QStringLiteral("title")));
        }
    } else {
        result.format = Format::ED269;
        result.title = _text(root.value(QStringLiteral("title")));
    }

    // ED-318 names the dataset metadata "metadata", some publishers use "datasetMetadata"
    QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    if (metadata.isEmpty()) {
        metadata = root.value(QStringLiteral("datasetMetadata")).toObject();
    }
    if (!metadata.isEmpty()) {
        result.provider = _text(metadata.value(QStringLiteral("provider")));
        result.issued = _dateTime(metadata.value(QStringLiteral("issued")));
        result.validFrom = _dateTime(metadata.value(QStringLiteral("validFrom")));
        result.validTo = _dateTime(metadata.value(QStringLiteral("validTo")));
    }

    for (const QJsonValue &featureValue : features) {
        const QJsonObject feature = featureValue.toObject();

        GeoZone::Data zone;
        bool valid = false;
        switch (result.format) {
        case Format::ED318:
            valid = _ed318Feature(feature, zone);
            break;
        case Format::FaaTfr:
            valid = _faaTfrFeature(feature, zone);
            break;
        case Format::Dipul:
            valid = _dipulFeature(feature, zone);
            break;
        case Format::FranceUas:
            valid = _franceFeature(feature, zone);
            break;
        case Format::ED269:
        case Format::Aixm:
        case Format::Unknown:
            valid = _ed269Feature(feature, zone);
            break;
        }
        if (valid) {
            result.zones.append(zone);
        } else {
            qCDebug(GeoZoneParserLog) << "Skipping feature without usable geometry" << zone.identifier;
            result.skippedFeatures++;
        }
    }

    // An empty list from a WFS means no zones are in the area or in effect
    const bool wfs = (result.format == Format::FaaTfr) || (result.format == Format::Dipul) || (result.format == Format::FranceUas);
    if (result.zones.isEmpty() && !wfs) {
        errorString = tr("No geozones with a supported geometry were found in the %1 file.").arg(formatToString(result.format));
        return false;
    }

    qCDebug(GeoZoneParserLog) << formatToString(result.format) << "zones:" << result.zones.count() << "skipped:" << result.skippedFeatures;

    return true;
}

QString GeoZoneParser::formatToString(Format format)
{
    switch (format) {
    case Format::ED269:
        return QStringLiteral("ED-269");
    case Format::ED318:
        return QStringLiteral("ED-318");
    case Format::FaaTfr:
        return QStringLiteral("FAA TFR");
    case Format::Dipul:
        return QStringLiteral("DiPUL");
    case Format::FranceUas:
        return QStringLiteral("IGN UAS");
    case Format::Aixm:
        return QStringLiteral("AIXM 5.1");
    case Format::Unknown:
        break;
    }
    return tr("Unknown");
}
