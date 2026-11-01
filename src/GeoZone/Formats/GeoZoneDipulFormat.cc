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

using namespace GeoZoneParserUtils;

namespace {

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
    zone.name = ((language() == QStringLiteral("de")) || englishName.isEmpty()) ? germanName : englishName;
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

/// German geographical zones GeoJSON from the DiPUL WFS
class GeoZoneDipulFormat : public GeoZoneFormat
{
public:
    GeoZoneParser::Format format() const final { return GeoZoneParser::Format::Dipul; }
    QString name() const final { return QStringLiteral("DiPUL"); }
    int detectionOrder() const final { return 20; }
    bool detect(const QJsonObject &root, const QJsonArray &features, GeoZoneParser::Format formatHint) const final
    {
        if (features.isEmpty()) {
            return isEmptyWfsResponse(root) && (formatHint == GeoZoneParser::Format::Dipul);
        }
        const QJsonObject properties = features.first().toObject().value(QStringLiteral("properties")).toObject();
        return properties.contains(QStringLiteral("type_code")) && properties.contains(QStringLiteral("legal_ref"));
    }
    void readMetadata(const QJsonObject &root, GeoZoneParser::Result &result) const final
    {
        result.title = GeoZoneParser::tr("DiPUL geographical zones (Germany)");
        result.provider = QStringLiteral("DFS / BMDV");
        result.issued = _dateTime(root.value(QStringLiteral("timeStamp")));
    }
    bool acceptsEmpty() const final { return true; }
    bool parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const final { return _dipulFeature(feature, zone); }
};

} // namespace

GEOZONE_REGISTER_FORMAT(GeoZoneDipulFormat)
