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

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimeZone>

QGC_LOGGING_CATEGORY(GeoZoneParserLog, "qgc.geozone.geozoneparser")

namespace {

constexpr double kMetersPerFoot = 0.3048;

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

    // The default locale follows the QGC language setting
    const QString uiLanguage = QLocale().name().left(2).toLower();
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
        if (uiText.isEmpty() && (language == uiLanguage)) {
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
    if (value.isDouble()) {
        return value.toDouble();
    }
    if (value.isString()) {
        bool ok = false;
        const double number = value.toString().toDouble(&ok);
        if (ok) {
            return number;
        }
    }
    return qQNaN();
}

QDateTime _dateTime(const QJsonValue &value)
{
    const QString string = value.toString().trimmed();
    if (string.isEmpty()) {
        return QDateTime();
    }

    QDateTime dateTime = QDateTime::fromString(string, Qt::ISODateWithMs);
    if (!dateTime.isValid()) {
        dateTime = QDateTime::fromString(string, Qt::ISODate);
    }
    if (dateTime.isValid() && (dateTime.timeSpec() == Qt::LocalTime)) {
        // Times without an offset are UTC in both standards
        dateTime.setTimeZone(QTimeZone::UTC);
    }
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
    for (const QJsonValue &positionValue : value.toArray()) {
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

/// Adds the volumes described by a GeoJSON-like horizontal geometry. Vertical limits are copied from volumeTemplate.
void _appendHorizontalGeometry(const QJsonObject &geometry, const GeoZone::Volume &volumeTemplate, double radiusScale, QList<GeoZone::Volume> &volumes)
{
    const QString type = geometry.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("Polygon")) {
        GeoZone::Volume volume = volumeTemplate;
        if (_polygonRings(geometry.value(QStringLiteral("coordinates")), volume)) {
            volumes.append(volume);
        }
    } else if (type == QStringLiteral("MultiPolygon")) {
        for (const QJsonValue &polygon : geometry.value(QStringLiteral("coordinates")).toArray()) {
            GeoZone::Volume volume = volumeTemplate;
            if (_polygonRings(polygon, volume)) {
                volumes.append(volume);
            }
        }
    } else if (type == QStringLiteral("Circle")) {
        // ED-269: { "type": "Circle", "center": [lon, lat], "radius": r }
        GeoZone::Volume volume = volumeTemplate;
        volume.isCircle = true;
        volume.radius = _number(geometry.value(QStringLiteral("radius"))) * radiusScale;
        if (_position(geometry.value(QStringLiteral("center")), volume.center) && (volume.radius > 0)) {
            volumes.append(volume);
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
        if (_position(geometry.value(QStringLiteral("coordinates")), volume.center) && (volume.radius > 0)) {
            volumes.append(volume);
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
    volume.lowerReference = layer.value(QStringLiteral("lowerReference")).toString().toUpper();
    volume.upperReference = layer.value(QStringLiteral("upperReference")).toString().toUpper();
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
        layer.lowerReference = geometry.value(QStringLiteral("lowerVerticalReference")).toString().toUpper();
        layer.upperReference = geometry.value(QStringLiteral("upperVerticalReference")).toString().toUpper();

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

bool GeoZoneParser::parse(const QByteArray &bytes, Result &result, QString &errorString)
{
    result = Result();
    errorString.clear();

    QByteArray json = bytes;
    // Several national feeds are published with a UTF-8 byte order mark which QJsonDocument rejects
    if (json.startsWith("\xEF\xBB\xBF")) {
        json.remove(0, 3);
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
        errorString = tr("Not a geozone file. Only EUROCAE ED-269 and ED-318 files are supported.");
        return false;
    }
    const QJsonArray features = featuresValue.toArray();

    if (root.value(QStringLiteral("type")).toString() == QStringLiteral("FeatureCollection")) {
        result.format = Format::ED318;
        result.title = _text(root.value(QStringLiteral("name")));
        if (result.title.isEmpty()) {
            result.title = _text(root.value(QStringLiteral("title")));
        }
    } else {
        result.format = Format::ED269;
        result.title = _text(root.value(QStringLiteral("title")));
    }

    for (const QJsonValue &featureValue : features) {
        const QJsonObject feature = featureValue.toObject();

        GeoZone::Data zone;
        const bool valid = (result.format == Format::ED318) ? _ed318Feature(feature, zone) : _ed269Feature(feature, zone);
        if (valid) {
            result.zones.append(zone);
        } else {
            qCDebug(GeoZoneParserLog) << "Skipping feature without usable geometry" << zone.identifier;
            result.skippedFeatures++;
        }
    }

    if (result.zones.isEmpty()) {
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
    case Format::Unknown:
    default:
        return tr("Unknown");
    }
}
