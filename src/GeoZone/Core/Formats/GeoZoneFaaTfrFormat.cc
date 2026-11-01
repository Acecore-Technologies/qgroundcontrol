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

#include <QtCore/QSet>
#include <QtCore/QTimeZone>
#include <QtCore/QXmlStreamReader>

#include <algorithm>
#include <cmath>

using namespace GeoZoneParserUtils;

namespace {

/// Times and altitudes of a NOTAM from its tfr.faa.gov detail XML
struct TfrDetail {
    QList<GeoZone::TimePeriod> periods;     ///< Empty when the NOTAM gives none
    GeoZone::Volume limits;                 ///< Lowest floor and highest ceiling of its areas
    bool valid = false;
    /// The merged areas, which the WFS polygons follow, give each of their vertices as a great circle point (GRC)
    bool greatCircleEdges = false;
};

QDateTime _tfrTime(const QString &text)
{
    // NOTAM times are UTC
    QDateTime time = QDateTime::fromString(text.trimmed(), Qt::ISODate);
    if (time.isValid()) {
        time.setTimeZone(QTimeZone::UTC);
    }
    return time;
}

TfrDetail _tfrDetail(const QByteArray &xml)
{
    TfrDetail detail;
    struct Limit {
        QString code;
        double value = qQNaN();
        QString unit;
    };
    struct Area {
        Limit lower;
        Limit upper;
    };
    QList<Area> areas;
    QList<GeoZone::TimePeriod> areaPeriods;
    QDateTime notamStart;
    QDateTime notamEnd;
    GeoZone::TimePeriod schedule;
    bool inArea = false;
    bool inSchedule = false;
    bool inMergedArea = false;
    bool inVertex = false;
    int greatCircleVertices = 0;
    int otherVertices = 0;

    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        const QStringView name = reader.name();
        if (reader.isStartElement()) {
            if (name == QLatin1String("TFRAreaGroup")) {
                inArea = true;
                areas.append(Area());
            } else if (name == QLatin1String("abdMergedArea")) {
                inMergedArea = true;
            } else if (inMergedArea && (name == QLatin1String("Avx"))) {
                inVertex = true;
            } else if (inVertex && (name == QLatin1String("codeType"))) {
                // Like in AIXM 4.5: GRC great circle, RHL rhumb line, CWA and CCA arcs, FNT border
                ((reader.readElementText().trimmed() == QLatin1String("GRC")) ? greatCircleVertices : otherVertices)++;
            } else if (name == QLatin1String("ScheduleGroup")) {
                inSchedule = true;
                schedule = GeoZone::TimePeriod();
            } else if (name == QLatin1String("dateEffective")) {
                const QDateTime time = _tfrTime(reader.readElementText());
                (inSchedule ? schedule.start : (inArea ? schedule.start : notamStart)) = time;
            } else if (name == QLatin1String("dateExpire")) {
                const QDateTime time = _tfrTime(reader.readElementText());
                (inSchedule ? schedule.end : (inArea ? schedule.end : notamEnd)) = time;
            } else if (inArea && !areas.isEmpty()) {
                Area &area = areas.last();
                if (name == QLatin1String("codeDistVerLower")) {
                    area.lower.code = reader.readElementText().trimmed();
                } else if (name == QLatin1String("valDistVerLower")) {
                    area.lower.value = _number(QJsonValue(reader.readElementText()));
                } else if (name == QLatin1String("uomDistVerLower")) {
                    area.lower.unit = reader.readElementText().trimmed();
                } else if (name == QLatin1String("codeDistVerUpper")) {
                    area.upper.code = reader.readElementText().trimmed();
                } else if (name == QLatin1String("valDistVerUpper")) {
                    area.upper.value = _number(QJsonValue(reader.readElementText()));
                } else if (name == QLatin1String("uomDistVerUpper")) {
                    area.upper.unit = reader.readElementText().trimmed();
                }
            }
        } else if (reader.isEndElement()) {
            if (name == QLatin1String("ScheduleGroup")) {
                inSchedule = false;
                if (schedule.start.isValid() || schedule.end.isValid()) {
                    areaPeriods.append(schedule);
                }
            } else if (name == QLatin1String("TFRAreaGroup")) {
                inArea = false;
            } else if (name == QLatin1String("abdMergedArea")) {
                inMergedArea = false;
            } else if (name == QLatin1String("Avx")) {
                inVertex = false;
            }
        }
    }
    if (reader.hasError() || areas.isEmpty()) {
        return detail;
    }

    detail.valid = true;
    detail.greatCircleEdges = (greatCircleVertices >= 3) && (otherVertices == 0);
    if (!areaPeriods.isEmpty()) {
        detail.periods = areaPeriods;
    } else if (notamStart.isValid() || notamEnd.isValid()) {
        GeoZone::TimePeriod period;
        period.start = notamStart;
        period.end = notamEnd;
        detail.periods.append(period);
    }

    // The shapes of a NOTAM don't say which area they belong to, so the zone spans the altitudes of all areas.
    // Limits given in different references can't be combined and stay unknown.
    const auto reference = [](const Limit &limit) {
        if (limit.code == QLatin1String("HEI")) {
            return QStringLiteral("AGL");
        }
        if (limit.code == QLatin1String("ALT")) {
            return QStringLiteral("AMSL");
        }
        if ((limit.code == QLatin1String("STD")) || (limit.unit == QLatin1String("FL"))) {
            return QStringLiteral("STD");
        }
        return QString();
    };
    const auto feet = [](const Limit &limit) {
        if (limit.unit == QLatin1String("FL")) {
            return limit.value * 100;
        }
        return (limit.unit == QLatin1String("FT")) ? limit.value : qQNaN();
    };
    QString lowerReference = reference(areas.first().lower);
    QString upperReference = reference(areas.first().upper);
    double lower = qInf();
    double upper = -qInf();
    for (const Area &area : std::as_const(areas)) {
        if ((reference(area.lower) != lowerReference) || (reference(area.upper) != upperReference)) {
            lowerReference.clear();
            upperReference.clear();
        }
        lower = (std::min)(lower, feet(area.lower));
        upper = (std::max)(upper, feet(area.upper));
    }
    GeoZone::Volume &limits = detail.limits;
    limits.feet = true;
    limits.lowerLimit = (lowerReference.isEmpty() || !std::isfinite(lower)) ? qQNaN() : lower;
    limits.upperLimit = (upperReference.isEmpty() || !std::isfinite(upper)) ? qQNaN() : upper;
    limits.setReferences(lowerReference, upperReference);
    return detail;
}

QString _tfrNotam(const QJsonObject &feature)
{
    // Keys look like 6/4240-1-FDC-F, the NOTAM number is the part before the first dash
    return _text(feature.value(QStringLiteral("properties")).toObject().value(QStringLiteral("NOTAM_KEY"))).section(QLatin1Char('-'), 0, 0);
}

/// FAA temporary flight restriction from the tfr.faa.gov WFS GeoJSON, with the times and altitudes of its NOTAM
/// detail when available
bool _faaTfrFeature(const QJsonObject &feature, const GeoZoneParser::Companions &companions, GeoZone::Data &zone)
{
    const QJsonObject properties = feature.value(QStringLiteral("properties")).toObject();
    const QString notamKey = _text(properties.value(QStringLiteral("NOTAM_KEY")));
    const QString notam = _tfrNotam(feature);
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
    zone.authorities.append(QStringLiteral("FAA"));

    // Without its detail the TFR applies at all times and altitudes
    GeoZone::Volume layer;
    const TfrDetail detail = companions.contains(notam) ? _tfrDetail(companions.value(notam).data) : TfrDetail();
    if (detail.valid) {
        layer = detail.limits;
        zone.applicability = detail.periods;
        zone.message = GeoZoneParser::tr("Temporary flight restriction, FAA NOTAM %1. The times and altitudes are those of the NOTAM, which also defines the exceptions.").arg(notam);
    } else {
        zone.message = GeoZoneParser::tr("Temporary flight restriction, FAA NOTAM %1. The NOTAM defines the times, altitudes and exceptions.").arg(notam);
    }

    _appendHorizontalGeometry(feature.value(QStringLiteral("geometry")).toObject(), layer, 1.0, zone.volumes);
    // The GeoJSON edges are straight in latitude and longitude unless the NOTAM says they are great circles
    if (detail.greatCircleEdges) {
        _densifyGreatCircles(zone.volumes);
    }
    return !zone.volumes.isEmpty();
}

/// FAA temporary flight restrictions GeoJSON from tfr.faa.gov
class GeoZoneFaaTfrFormat : public GeoZoneFormat
{
public:
    GeoZoneParser::Format format() const final { return GeoZoneParser::Format::FaaTfr; }
    QString name() const final { return QStringLiteral("FAA TFR"); }
    bool isLive() const final { return true; }
    int detectionOrder() const final { return 10; }
    bool detect(const QJsonObject &root, const QJsonArray &features, GeoZoneParser::Format formatHint) const final
    {
        if (features.isEmpty()) {
            // An empty list from the FAA means no restrictions are in effect, other GeoServer feeds have the same WFS counts
            return (formatHint == GeoZoneParser::Format::FaaTfr) && root.contains(QStringLiteral("totalFeatures")) && root.contains(QStringLiteral("timeStamp"));
        }
        return features.first().toObject().value(QStringLiteral("properties")).toObject().contains(QStringLiteral("NOTAM_KEY"));
    }
    void readMetadata(const QJsonObject &root, GeoZoneParser::Result &result) const final
    {
        result.title = GeoZoneParser::tr("FAA temporary flight restrictions");
        result.provider = QStringLiteral("FAA");
        result.issued = _dateTime(root.value(QStringLiteral("timeStamp")));
    }
    bool acceptsEmpty() const final { return true; }
    bool parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const final { return _faaTfrFeature(feature, {}, zone); }
    bool parseFeatureWithCompanions(const QJsonObject &feature, const GeoZoneParser::Companions &companions, GeoZone::Data &zone) const final
    {
        return _faaTfrFeature(feature, companions, zone);
    }
    /// The detail XML of each NOTAM, downloaded again when the NOTAM changes
    QList<CompanionRequest> companionRequests(const QUrl &documentUrl, const QJsonObject &, const QJsonArray &features) const final
    {
        // The details are served next to the list, like https://tfr.faa.gov/download/detail_6_4240.xml
        const QString base = documentUrl.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
        QList<CompanionRequest> requests;
        QSet<QString> notams;
        for (const QJsonValue &feature : features) {
            const QString notam = _tfrNotam(feature.toObject());
            if (notam.isEmpty() || notams.contains(notam)) {
                continue;
            }
            notams.insert(notam);
            const QString modified = _text(feature.toObject().value(QStringLiteral("properties")).toObject().value(QStringLiteral("LAST_MODIFICATION_DATETIME")));
            requests.append(CompanionRequest{ notam, modified, QStringLiteral("%1/download/detail_%2.xml").arg(base, QString(notam).replace(QLatin1Char('/'), QLatin1Char('_'))) });
        }
        return requests;
    }
};

} // namespace

GEOZONE_REGISTER_FORMAT(GeoZoneFaaTfrFormat)
