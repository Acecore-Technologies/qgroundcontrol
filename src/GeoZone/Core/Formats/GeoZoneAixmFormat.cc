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
#include <QtCore/QtMath>
#include <QtCore/QXmlStreamReader>

#include <algorithm>
#include <cmath>

using namespace GeoZoneParserUtils;

namespace {

/// Limit of an AIXM airspace volume as published
struct AixmLimit {
    double value = qQNaN();
    QString uom;
    QString reference;
};

/// @return Meters of a distance in an AIXM or UCUM unit, NaN when the unit is unknown or missing: a radius isn't guessed
double _aixmDistance(double value, const QString &uom)
{
    const QString unit = uom.trimmed().toUpper();
    if ((unit == QStringLiteral("[NMI_I]")) || (unit == QStringLiteral("NM"))) {
        return value * 1852;
    }
    if (unit == QStringLiteral("KM")) {
        return value * 1000;
    }
    if (unit == QStringLiteral("M")) {
        return value;
    }
    if ((unit == QStringLiteral("[FT_I]")) || (unit == QStringLiteral("FT"))) {
        return value * kMetersPerFoot;
    }
    if ((unit == QStringLiteral("[MI_I]")) || (unit == QStringLiteral("MI"))) {
        return value * 1609.344;
    }
    return qQNaN();
}

/// @return Degrees of an angle, NaN when it isn't a number or its unit isn't degrees
double _aixmAngle(const QString &text, QStringView uom)
{
    return (uom.trimmed().compare(QLatin1String("deg"), Qt::CaseInsensitive) == 0) ? _numberFromText(text) : qQNaN();
}

/// Coordinate reference system of the positions of an element: that of its srsName and srsDimension, or of the
/// elements enclosing it. EPSG:4326, with the latitude first, where none is given, as the AIXM profile of GML has it.
struct AixmCrs {
    bool supported = true;
    bool latitudeFirst = true;
    int systemDimension = 2;    ///< Values of a position in the reference system
    int dimension = 2;          ///< Values of a position: 3 with a height, which isn't read
    QString name;               ///< As published, for the issue
};

/// @return The reference system of an element with the attributes, in one with the enclosing one. Positions are read
///         in WGS 84 by latitude and longitude (EPSG:4326, or EPSG:4979 with a height) or by longitude and latitude
///         (OGC CRS84, or CRS84h with a height), named as URN, URL or authority and code. Others aren't supported.
AixmCrs _aixmCrs(const AixmCrs &enclosing, const QXmlStreamAttributes &attributes)
{
    if (attributes.isEmpty()) {
        return enclosing;
    }
    AixmCrs crs = enclosing;
    if (attributes.hasAttribute(QLatin1String("srsName"))) {
        const QString srsName = attributes.value(QLatin1String("srsName")).trimmed().toString();
        // Elements usually repeat the name of the one enclosing them
        if (!crs.supported || (srsName != crs.name)) {
            crs.name = srsName;
            const QString name = srsName.toUpper();
            // urn:ogc:def:crs:EPSG::4326, http://www.opengis.net/def/crs/EPSG/0/4326 or EPSG:4326: the authority and
            // the code are the first and the last part after the prefix
            static const QString urlPrefix = QStringLiteral("://WWW.OPENGIS.NET/DEF/CRS/");
            QStringList parts;
            if (name.startsWith(QStringLiteral("URN:OGC:DEF:CRS:")) || name.startsWith(QStringLiteral("URN:X-OGC:DEF:CRS:"))) {
                parts = name.section(QStringLiteral(":DEF:CRS:"), 1).split(QLatin1Char(':'));
            } else if (name.startsWith(QStringLiteral("HTTP") + urlPrefix) || name.startsWith(QStringLiteral("HTTPS") + urlPrefix)) {
                parts = name.section(urlPrefix, 1).split(QLatin1Char('/'));
            } else {
                parts = name.split(QLatin1Char(':'));
            }
            const bool authorityAndCode = (parts.count() == 2) || (parts.count() == 3);
            const QString authority = parts.constFirst();
            const QString code = parts.constLast();
            if (authorityAndCode && (authority == QStringLiteral("EPSG")) && ((code == QStringLiteral("4326")) || (code == QStringLiteral("4979")))) {
                crs.supported = true;
                crs.latitudeFirst = true;
                crs.systemDimension = (code == QStringLiteral("4979")) ? 3 : 2;
            } else if (authorityAndCode && ((authority == QStringLiteral("OGC")) || (authority == QStringLiteral("CRS"))) &&
                       ((code == QStringLiteral("CRS84")) || (code == QStringLiteral("84")) || (code == QStringLiteral("CRS84H")))) {
                crs.supported = true;
                crs.latitudeFirst = false;
                crs.systemDimension = (code == QStringLiteral("CRS84H")) ? 3 : 2;
            } else {
                crs.supported = false;
            }
        }
        // That of the reference system unless the element gives its own
        crs.dimension = crs.systemDimension;
    }
    if (attributes.hasAttribute(QLatin1String("srsDimension"))) {
        const QStringView srsDimension = attributes.value(QLatin1String("srsDimension"));
        bool ok = false;
        crs.dimension = srsDimension.trimmed().toInt(&ok);
        if (!ok || ((crs.dimension != 2) && (crs.dimension != 3))) {
            crs.supported = false;
            crs.name = QStringLiteral("%1 srsDimension %2").arg(crs.name, srsDimension).trimmed();
        }
    }
    return crs;
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
/// line strings, arcs and circles by center point are supported. As in the AIXM coding guidelines, geodesic strings are
/// great circles, line strings in EPSG:4326 are straight in latitude and longitude, like parallels, and arcs are circles
/// around their center whose angles are bearings. Positions are read in the reference system of their element or of
/// those enclosing it, see _aixmCrs. A volume whose boundary has a position, arc or unit which can't be read, or is in
/// another reference system, is left out with an issue of its airspace, rather than given another shape. Airspaces
/// left without volume are returned too, GeoZoneParser counts them as skipped with their issues.
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
    QList<bool> geodesicEdges;  ///< Of the edges from each ring vertex to the next, see GeoZoneGeometry::densifyGreatCircles
    bool circleOnly = true;
    bool volumeRejected = false;
    QString segment;
    QString segmentIssue;   ///< Why the segment can't be read, empty when it can
    QList<AixmCrs> crsStack;    ///< Of the open elements
    QList<QGeoCoordinate> points;
    double radius = qQNaN();
    double startAngle = qQNaN();
    double endAngle = qQNaN();

    // The edges within a geodesic string are great circles, the others, like the one joining two segments which don't
    // meet, are straight
    const auto appendPoints = [&ring, &geodesicEdges](const QList<QGeoCoordinate> &newPoints, bool geodesic) {
        for (qsizetype i = 0; i < newPoints.count(); i++) {
            if (ring.isEmpty() || (ring.constLast().distanceTo(newPoints[i]) > 0.01)) {
                if (!ring.isEmpty()) {
                    geodesicEdges.append(geodesic && (i > 0));
                }
                ring.append(newPoints[i]);
            }
        }
    };
    // Adds the positions of a pos or posList in the reference system
    // @return Why they can't be read: the reference system isn't supported, a value isn't a number or a position isn't
    //         on the globe. Empty when they are read.
    const auto parsePositions = [](QStringView text, const AixmCrs &crs, QList<QGeoCoordinate> &positions) {
        if (!crs.supported) {
            return GeoZoneParser::tr("Boundary in the unsupported coordinate reference system %1 ignored").arg(crs.name);
        }
        // The values are separated by whitespace. Read in place: a message has a pos for each point of its boundaries.
        double values[3] = {};
        int count = 0;
        qsizetype i = 0;
        while (true) {
            while ((i < text.size()) && text[i].isSpace()) {
                i++;
            }
            if (i >= text.size()) {
                break;
            }
            const qsizetype start = i;
            while ((i < text.size()) && !text[i].isSpace()) {
                i++;
            }
            values[count++] = _numberFromText(text.mid(start, i - start));
            if (count < crs.dimension) {
                continue;
            }
            count = 0;
            const QGeoCoordinate coordinate(crs.latitudeFirst ? values[0] : values[1], crs.latitudeFirst ? values[1] : values[0]);
            // The height isn't used, the limits of the volume are
            if (!coordinate.isValid() || ((crs.dimension == 3) && std::isnan(values[2]))) {
                return GeoZoneParser::tr("Boundary with an invalid position ignored");
            }
            positions.append(coordinate);
        }
        return (count == 0) ? QString() : GeoZoneParser::tr("Boundary with an invalid position ignored");
    };
    // Reads the text of the element just started, which ends it
    const auto elementText = [&reader, &crsStack]() {
        const QString text = reader.readElementText();
        crsStack.removeLast();
        return text;
    };
    // The volume isn't added, its airspace tells why
    const auto rejectVolume = [&volumeRejected, &zone](const QString &issue) {
        volumeRejected = true;
        zone.issues.append(issue);
    };

    while (!reader.atEnd()) {
        reader.readNext();
        const QStringView name = reader.name();
        if (reader.isStartElement()) {
            crsStack.append(_aixmCrs(crsStack.isEmpty() ? AixmCrs() : crsStack.constLast(), reader.attributes()));
            // Dataset title of the message metadata
            if (!titleRead && !inAirspace && (name == QLatin1String("title"))) {
                inTitle = true;
            } else if (inTitle && (name == QLatin1String("CharacterString"))) {
                const QString title = elementText().trimmed();
                if (!title.isEmpty()) {
                    result.title = title;
                }
                inTitle = false;
                titleRead = true;
                // Reading the text moved the reader on, the name is no longer that of this element
                continue;
            }
            if (name == QLatin1String("Airspace")) {
                if (cancelled()) {
                    return false;
                }
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
                geodesicEdges.clear();
                circleOnly = true;
                volumeRejected = false;
            } else if (!inVolume && (name == QLatin1String("type")) && type.isEmpty()) {
                type = elementText().trimmed();
            } else if (!inVolume && (name == QLatin1String("localType")) && localType.isEmpty()) {
                localType = elementText().trimmed();
            } else if (!inVolume && (name == QLatin1String("designator")) && zone.identifier.isEmpty()) {
                zone.identifier = elementText().trimmed();
            } else if (!inVolume && (name == QLatin1String("name")) && zone.name.isEmpty()) {
                zone.name = elementText().trimmed();
            } else if (inVolume && ((name == QLatin1String("upperLimit")) || (name == QLatin1String("lowerLimit")))) {
                AixmLimit &limit = (name == QLatin1String("upperLimit")) ? upper : lower;
                const QString uom = reader.attributes().value(QStringLiteral("uom")).toString();
                limit.uom = uom.trimmed().toUpper();
                const QString text = elementText().trimmed().toUpper();
                if (text == QStringLiteral("GND")) {
                    limit.value = 0;
                } else if (text == QStringLiteral("UNL")) {
                    limit.value = kUnlimitedSentinel;
                    limit.uom = QStringLiteral("FT");
                } else {
                    limit.value = _numberFromText(text);
                    // A limit in an unknown unit is unknown, which applies the volume at all altitudes on its side
                    static const QStringList units = { QStringLiteral("FT"), QStringLiteral("FL"), QStringLiteral("M") };
                    if (!std::isnan(limit.value) && !units.contains(limit.uom)) {
                        limit.value = qQNaN();
                        zone.issues.append(GeoZoneParser::tr("Unknown unit %1, the altitude limits are ignored").arg(uom));
                    }
                }
            } else if (inVolume && (name == QLatin1String("upperLimitReference"))) {
                upper.reference = elementText().trimmed().toUpper();
            } else if (inVolume && (name == QLatin1String("lowerLimitReference"))) {
                lower.reference = elementText().trimmed().toUpper();
            } else if (inVolume && (name == QLatin1String("exterior"))) {
                inExterior = true;
            } else if (inExterior && ((name == QLatin1String("GeodesicString")) || (name == QLatin1String("LineStringSegment")) ||
                                      (name == QLatin1String("ArcByCenterPoint")) || (name == QLatin1String("CircleByCenterPoint")))) {
                segment = name.toString();
                segmentIssue.clear();
                points.clear();
                radius = qQNaN();
                startAngle = qQNaN();
                endAngle = qQNaN();
            } else if (!segment.isEmpty() && ((name == QLatin1String("pos")) || (name == QLatin1String("posList")))) {
                const AixmCrs crs = crsStack.constLast();
                const QString issue = parsePositions(elementText(), crs, points);
                if (segmentIssue.isEmpty()) {
                    segmentIssue = issue;
                }
            } else if (!segment.isEmpty() && (name == QLatin1String("radius"))) {
                const QString uom = reader.attributes().value(QStringLiteral("uom")).toString();
                radius = _aixmDistance(_numberFromText(elementText()), uom);
            } else if (!segment.isEmpty() && (name == QLatin1String("startAngle"))) {
                const QString uom = reader.attributes().value(QStringLiteral("uom")).toString();
                startAngle = _aixmAngle(elementText(), uom);
            } else if (!segment.isEmpty() && (name == QLatin1String("endAngle"))) {
                const QString uom = reader.attributes().value(QStringLiteral("uom")).toString();
                endAngle = _aixmAngle(elementText(), uom);
            }
        } else if (reader.isEndElement()) {
            if (!crsStack.isEmpty()) {
                crsStack.removeLast();
            }
            if (!inAirspace) {
                continue;
            }
            if (!segment.isEmpty() && (name == segment)) {
                if (!segmentIssue.isEmpty()) {
                    rejectVolume(segmentIssue);
                } else if (segment == QLatin1String("ArcByCenterPoint")) {
                    // The center alone would be a vertex of another boundary, and a radius beyond the limit of circles
                    // leaves no step between the points of the arc
                    if ((points.count() == 1) && _validRadius(radius) && std::isfinite(startAngle) && std::isfinite(endAngle)) {
                        // The sweep is signed, clockwise when the end bearing is larger. Arcs of more than a turn are
                        // malformed, they end after one.
                        const double sweep = std::clamp(endAngle - startAngle, -360.0, 360.0);
                        // Points close enough that the chords between them stay within the tolerance of the arc, every
                        // 3 degrees at least
                        const double tolerance = GeoZoneGeometry::kGreatCircleToleranceMeters;
                        const double maxStep = (radius > tolerance) ? (std::min)(3.0, qRadiansToDegrees(2 * std::acos(1 - (tolerance / radius)))) : 3.0;
                        const int steps = std::clamp(static_cast<int>(std::ceil(std::abs(sweep) / maxStep)), 1, 3600);
                        QList<QGeoCoordinate> arc;
                        for (int i = 0; i <= steps; i++) {
                            arc.append(points.constFirst().atDistanceAndAzimuth(radius, startAngle + (sweep * i / steps)));
                        }
                        appendPoints(arc, false);
                        circleOnly = false;
                    } else {
                        rejectVolume(GeoZoneParser::tr("Boundary with an invalid arc ignored"));
                    }
                } else if (segment == QLatin1String("CircleByCenterPoint")) {
                    // An invalid radius is reported by GeoZoneParser
                    if ((points.count() == 1) && !volume.center.isValid()) {
                        volume.center = points.constFirst();
                        volume.radius = radius;
                    } else {
                        rejectVolume(GeoZoneParser::tr("Boundary with an invalid circle ignored"));
                    }
                } else {
                    appendPoints(points, segment == QLatin1String("GeodesicString"));
                    circleOnly = false;
                }
                segment.clear();
            } else if (name == QLatin1String("exterior")) {
                inExterior = false;
            } else if (name == QLatin1String("AirspaceVolume")) {
                inVolume = false;
                _setAixmLimits(volume, lower, upper);
                // Invalid radii and rings are reported by GeoZoneParser
                if (volumeRejected) {
                    // The issue of the airspace tells
                } else if (volume.center.isValid() && !circleOnly) {
                    zone.issues.append(GeoZoneParser::tr("Boundary with an invalid circle ignored"));
                } else if (circleOnly && volume.center.isValid()) {
                    volume.isCircle = true;
                    _appendVolume(volume, zone.volumes, zone.issues);
                } else if (ring.count() > kMaxRingVertices) {
                    zone.issues.append(GeoZoneParser::tr("Boundary with more than %1 points ignored").arg(kMaxRingVertices));
                } else {
                    if ((ring.count() > 1) && (ring.constFirst().distanceTo(ring.constLast()) < 1)) {
                        // The edge to the last vertex now closes the ring
                        ring.removeLast();
                    } else {
                        geodesicEdges.append(false);
                    }
                    if (!ring.isEmpty()) {
                        volume.isCircle = false;
                        const GeoZone::Ring published = GeoZone::toRing(ring);
                        volume.rings = { (ring.count() >= 3) ? GeoZoneGeometry::densifyGreatCircles(published, kMaxRingVertices, geodesicEdges) : published };
                        _appendVolume(volume, zone.volumes, zone.issues);
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
                // Also without volumes, for its issues
                result.zones.append(zone);
            }
        }
    }

    if (reader.hasError()) {
        errorString = GeoZoneParser::tr("Invalid XML: %1 (line %2)").arg(reader.errorString()).arg(reader.lineNumber());
        return false;
    }
    if (result.zones.isEmpty()) {
        errorString = GeoZoneParser::tr("No airspaces were found in the AIXM file.");
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
    int detectionOrder() const final { return 40; }
    bool isXml() const final { return true; }
    bool detectXml(QStringView rootElement, QStringView namespaceUri) const final
    {
        return (rootElement == QLatin1String("AIXMBasicMessage")) || namespaceUri.startsWith(QLatin1String("http://www.aixm.aero/schema/5.1"));
    }
    bool parseXml(const QByteArray &xml, GeoZoneParser::Result &result, QString &errorString) const final { return _parseAixm(xml, result, errorString); }
};

} // namespace

GEOZONE_REGISTER_FORMAT(GeoZoneAixmFormat)
