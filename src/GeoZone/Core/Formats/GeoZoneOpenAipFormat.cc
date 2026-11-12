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
#include "GeoZoneVertical.h"

#include <QtCore/QSet>

using namespace GeoZoneParserUtils;

namespace {

/// OpenAIP airspace types, see https://api.core.openaip.net/api/schemas/response/airspace/airspace-schema.json
enum OpenAipType {
    Other = 0, Restricted = 1, Danger = 2, ProhibitedArea = 3, Ctr = 4, Tmz = 5, Rmz = 6, Tma = 7, Tra = 8, Tsa = 9,
    Atz = 13, Matz = 14, Mtr = 16, AlertArea = 17, WarningArea = 18, ProtectedArea = 19, Htz = 20, GlidingSector = 21,
    Mta = 25, Cta = 26, AerialSporting = 28, LowOverflight = 29, MilitaryRoute = 30, Mctr = 36,
};

/// @return The airspace object of an item of the API's list (the item itself) or of a feature of the exports (its
///         properties)
QJsonObject _openAipItem(const QJsonObject &feature)
{
    return (feature.value(QStringLiteral("type")).toString() == QStringLiteral("Feature")) ? feature.value(QStringLiteral("properties")).toObject() : feature;
}

bool _isOpenAipItem(const QJsonObject &item)
{
    return item.contains(QStringLiteral("_id")) && item.value(QStringLiteral("icaoClass")).isDouble() && item.value(QStringLiteral("type")).isDouble();
}

QString _typeName(int type)
{
    static const char *const names[] = {
        "Other", "Restricted", "Danger", "Prohibited", "CTR", "TMZ", "RMZ", "TMA", "TRA", "TSA", "FIR", "UIR", "ADIZ", "ATZ",
        "MATZ", "Airway", "MTR", "Alert area", "Warning area", "Protected area", "HTZ", "Gliding sector", "TRP", "TIZ", "TIA",
        "MTA", "CTA", "ACC sector", "Aerial sporting or recreational activity", "Low altitude overflight restriction", "MRT",
        "TSA/TRA feeding route", "VFR sector", "FIS sector", "LTA", "UTA", "MCTR",
    };
    return ((type >= 0) && (type < static_cast<int>(std::size(names)))) ? QString::fromLatin1(names[type]) : QString::number(type);
}

/// Sets a limit of the volume in feet from an OpenAIP limit: value, unit (0 m, 1 ft, 6 FL) and reference datum
/// (0 GND, 1 MSL, 2 STD). Unknown units leave the limit unspecified with an issue.
void _openAipLimit(const QJsonObject &limit, GeoZoneVertical::Limit side, GeoZone::Volume &volume, QStringList &issues)
{
    const bool upper = (side == GeoZoneVertical::Limit::Upper);
    double value = _number(limit.value(QStringLiteral("value")));
    const int unit = limit.value(QStringLiteral("unit")).toInt(-1);
    const int datum = limit.value(QStringLiteral("referenceDatum")).toInt(-1);
    QString reference = (datum == 0) ? QStringLiteral("AGL") : (datum == 1) ? QStringLiteral("MSL") : (datum == 2) ? QStringLiteral("STD") : QString();
    if (unit == 0) {
        value /= kMetersPerFoot;
    } else if (unit == 6) {
        value *= 100;
        reference = QStringLiteral("STD");
    } else if (unit != 1) {
        if (!limit.isEmpty()) {
            issues.append(GeoZoneParser::tr("Unknown OpenAIP limit unit %1").arg(unit));
        }
        value = qQNaN();
    }
    // Ceilings of airspaces without one are published as very high flight levels
    if (upper && (reference == QStringLiteral("STD")) && (value >= 99900)) {
        value = kUnlimitedSentinel;
    }
    (upper ? volume.upperLimit : volume.lowerLimit) = value;
    volume.setReferences(upper ? volume.lowerReferenceCode : reference, upper ? reference : volume.upperReferenceCode);
}

QString _hoursText(const QJsonObject &hours)
{
    static const char *const days[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    QStringList periods;
    bool always = true;
    const QJsonArray operatingHours = hours.value(QStringLiteral("operatingHours")).toArray();
    for (const QJsonValue &value : operatingHours) {
        const QJsonObject period = value.toObject();
        const int day = period.value(QStringLiteral("dayOfWeek")).toInt(-1);
        const QString start = _text(period.value(QStringLiteral("startTime")));
        const QString end = _text(period.value(QStringLiteral("endTime")));
        QString text = ((day >= 0) && (day < 7)) ? QString::fromLatin1(days[day]) : QStringLiteral("?");
        if (period.value(QStringLiteral("byNotam")).toBool()) {
            text += QStringLiteral(" NOTAM");
        } else {
            text += QStringLiteral(" %1-%2").arg(period.value(QStringLiteral("sunrise")).toBool() ? QStringLiteral("SR") : start,
                                                 period.value(QStringLiteral("sunset")).toBool() ? QStringLiteral("SS") : end);
        }
        always = always && (start == QStringLiteral("00:00")) && (end == QStringLiteral("00:00")) && !period.value(QStringLiteral("byNotam")).toBool();
        periods.append(text);
    }
    return (always && (operatingHours.count() >= 7)) ? QString() : periods.join(QStringLiteral(", "));
}

GeoZoneFormat::FeatureResult _openAipFeature(const QJsonObject &feature, GeoZone::Data &zone)
{
    const QJsonObject item = _openAipItem(feature);
    const int type = item.value(QStringLiteral("type")).toInt(-1);
    const int icaoClass = item.value(QStringLiteral("icaoClass")).toInt(-1);
    const QString className = ((icaoClass >= 0) && (icaoClass <= 6)) ? QString(QChar(QLatin1Char('A').unicode() + icaoClass)) : QString();

    zone.identifier = _text(item.value(QStringLiteral("_id")));
    zone.name = _text(item.value(QStringLiteral("name")));
    zone.country = _text(item.value(QStringLiteral("country")));
    zone.restrictionCode = className.isEmpty() ? _typeName(type) : QStringLiteral("%1 (class %2)").arg(_typeName(type), className);
    zone.authorities.append(GeoZoneParser::tr("OpenAIP community data (unofficial)"));

    // Only the airspaces concerning drones flying low are kept: the restricted ones, the controlled airspace around
    // aerodromes and the areas of hazardous activities. The others are left out on purpose, which isn't a problem of
    // the data.
    switch (type) {
    case ProhibitedArea:
        zone.restriction = GeoZone::Prohibited;
        zone.reasons.append(QStringLiteral("SENSITIVE"));
        zone.message = GeoZoneParser::tr("Prohibited area.");
        break;
    case Restricted:
    case LowOverflight:
        zone.restriction = GeoZone::ReqAuthorisation;
        zone.reasons.append(QStringLiteral("OTHER"));
        zone.otherReasonInfo = _typeName(type);
        zone.message = GeoZoneParser::tr("Restricted airspace: flights need a permission while it is active.");
        break;
    case Ctr:
    case Mctr:
    case Atz:
    case Matz:
    case Htz:
        zone.restriction = GeoZone::ReqAuthorisation;
        zone.reasons.append(QStringLiteral("AIR_TRAFFIC"));
        zone.message = GeoZoneParser::tr("Controlled airspace around an aerodrome: drone flights usually need an authorisation of the air navigation service.");
        break;
    case Tma:
    case Cta:
        // Controlled areas of classes E to G need no authorisation
        if ((icaoClass < 0) || (icaoClass > 3)) {
            return GeoZoneFormat::FeatureResult::Filtered;
        }
        zone.restriction = GeoZone::ReqAuthorisation;
        zone.reasons.append(QStringLiteral("AIR_TRAFFIC"));
        zone.message = GeoZoneParser::tr("Controlled airspace: flights need a clearance between its floor and ceiling.");
        break;
    case Tmz:
    case Rmz:
        zone.restriction = GeoZone::Conditional;
        zone.reasons.append(QStringLiteral("AIR_TRAFFIC"));
        zone.message = (type == Tmz) ? GeoZoneParser::tr("Transponder mandatory zone.") : GeoZoneParser::tr("Radio mandatory zone.");
        break;
    case Danger:
    case Tra:
    case Tsa:
    case Mta:
    case Mtr:
    case MilitaryRoute:
    case AlertArea:
    case WarningArea:
    case GlidingSector:
    case AerialSporting:
        zone.restriction = GeoZone::Conditional;
        zone.reasons.append(QStringLiteral("OTHER"));
        zone.otherReasonInfo = _typeName(type);
        zone.message = GeoZoneParser::tr("Activities hazardous to aircraft may take place while the area is active.");
        break;
    case ProtectedArea:
        zone.restriction = GeoZone::Conditional;
        zone.reasons.append(QStringLiteral("NATURE"));
        zone.message = GeoZoneParser::tr("Protected area: overflights should be avoided.");
        break;
    default:
        // Flight information regions, airways, sectors and the like
        return GeoZoneFormat::FeatureResult::Filtered;
    }

    QStringList conditions;
    if (item.value(QStringLiteral("onDemand")).toBool()) {
        conditions.append(GeoZoneParser::tr("Active on demand"));
    }
    if (item.value(QStringLiteral("onRequest")).toBool()) {
        conditions.append(GeoZoneParser::tr("Active on request"));
    }
    if (item.value(QStringLiteral("byNotam")).toBool()) {
        conditions.append(GeoZoneParser::tr("Activated by NOTAM"));
    }
    if (item.value(QStringLiteral("specialAgreement")).toBool()) {
        conditions.append(GeoZoneParser::tr("Special agreement"));
    }
    if (item.value(QStringLiteral("requestCompliance")).toBool()) {
        conditions.append(GeoZoneParser::tr("Unofficial area whose authority requests compliance"));
    }
    zone.restrictionConditions = conditions.join(QStringLiteral(". "));

    zone.extendedProperties.append(GeoZoneParser::tr("Source: OpenAIP, unofficial community data (CC BY-NC 4.0, www.openaip.net). Not for navigation: check the official AIP and drone zone maps."));
    if (const QString hours = _hoursText(item.value(QStringLiteral("hoursOfOperation")).toObject()); !hours.isEmpty()) {
        zone.extendedProperties.append(GeoZoneParser::tr("Hours of operation: %1").arg(hours));
    }
    for (const QJsonValue &value : item.value(QStringLiteral("frequencies")).toArray()) {
        const QJsonObject frequency = value.toObject();
        if (frequency.value(QStringLiteral("primary")).toBool()) {
            zone.extendedProperties.append(GeoZoneParser::tr("Frequency: %1 MHz %2").arg(_text(frequency.value(QStringLiteral("value"))), _text(frequency.value(QStringLiteral("name")))).trimmed());
        }
    }
    if (const QString remarks = _text(item.value(QStringLiteral("remarks"))); !remarks.isEmpty()) {
        zone.extendedProperties.append(GeoZoneParser::tr("Remarks: %1").arg(remarks));
    }

    GeoZone::Volume layer;
    layer.feet = true;
    _openAipLimit(item.value(QStringLiteral("lowerLimit")).toObject(), GeoZoneVertical::Limit::Lower, layer, zone.issues);
    _openAipLimit(item.value(QStringLiteral("upperLimit")).toObject(), GeoZoneVertical::Limit::Upper, layer, zone.issues);
    // The exports keep the geometry in the feature, the API in the item
    const QJsonObject geometry = feature.contains(QStringLiteral("properties")) ? feature.value(QStringLiteral("geometry")).toObject() : item.value(QStringLiteral("geometry")).toObject();
    _appendHorizontalGeometry(geometry, layer, 1.0, zone.volumes, zone.issues);
    return GeoZoneFormat::featureResult(zone);
}

/// Airspaces of OpenAIP (www.openaip.net), the items of its API's lists or the features of its GeoJSON exports.
/// Unofficial community data under CC BY-NC 4.0, which the zones and the dataset say.
class GeoZoneOpenAipFormat : public GeoZoneFormat
{
public:
    GeoZoneParser::Format format() const final { return GeoZoneParser::Format::OpenAip; }
    QString name() const final { return QStringLiteral("OpenAIP"); }
    int detectionOrder() const final { return 16; }
    bool detect(const QJsonObject &, const QJsonArray &features, GeoZoneParser::Format formatHint) const final
    {
        // The API answers an area without airspaces with an empty list
        if (features.isEmpty()) {
            return formatHint == GeoZoneParser::Format::OpenAip;
        }
        return _isOpenAipItem(_openAipItem(features.first().toObject()));
    }
    void readMetadata(const QJsonObject &root, GeoZoneParser::Result &result) const final
    {
        // The exports have the airspaces of a country
        QSet<QString> countries;
        for (const QJsonValue &feature : root.value(QStringLiteral("features")).toArray()) {
            countries.insert(_text(_openAipItem(feature.toObject()).value(QStringLiteral("country"))));
        }
        result.title = (countries.count() == 1) && !countries.cbegin()->isEmpty()
                           ? GeoZoneParser::tr("Airspaces of %1 (OpenAIP, unofficial, CC BY-NC 4.0)").arg(*countries.cbegin())
                           : GeoZoneParser::tr("Airspaces (OpenAIP, unofficial, CC BY-NC 4.0)");
        result.provider = GeoZoneParser::tr("OpenAIP (unofficial, CC BY-NC 4.0)");
    }
    bool acceptsEmpty() const final { return true; }
    FeatureResult parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const final { return _openAipFeature(feature, zone); }
};

} // namespace

GEOZONE_REGISTER_FORMAT(GeoZoneOpenAipFormat)
