/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFormat.h"
#include "GeoZoneParser.h"
#include "GeoZoneParserUtils.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QXmlStreamReader>

#include <cmath>

using namespace GeoZoneParserUtils;

namespace {

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

/// AIXM 5.1 airspaces, like the UK flight restriction zones from NATS
class GeoZoneAixmFormat : public GeoZoneFormat
{
public:
    GeoZoneParser::Format format() const final { return GeoZoneParser::Format::Aixm; }
    QString name() const final { return QStringLiteral("AIXM 5.1"); }
    bool isXml() const final { return true; }
    bool parseXml(const QByteArray &xml, GeoZoneParser::Result &result, QString &errorString) const final { return _parseAixm(xml, result, errorString); }
};

} // namespace

const GeoZoneFormat *GeoZoneFormat::aixmFormat()
{
    static const GeoZoneAixmFormat instance;
    return &instance;
}
