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

#include <QtCore/QRegularExpression>

#include <cmath>
#include <optional>

using namespace GeoZoneParserUtils;

namespace {

/// The layers of the FAA's public ArcGIS feature services, told apart by their fields
enum class FaaLayer {
    Unknown,
    FacilityMap,        ///< UAS facility map grid cells of controlled airspace, with the ceiling LAANC authorises
    NationalSecurity,   ///< National security UAS flight restrictions, 14 CFR 99.7 special security instructions
    ClassAirspace,      ///< Class B, C, D and E airspace
    SpecialUse,         ///< Prohibited, restricted, warning and alert areas and military operations areas
};

FaaLayer _faaLayer(const QJsonObject &properties)
{
    if (properties.contains(QStringLiteral("CEILING")) && properties.contains(QStringLiteral("APT1_LAANC"))) {
        return FaaLayer::FacilityMap;
    }
    if (properties.contains(QStringLiteral("Ceiling")) && properties.contains(QStringLiteral("Floor")) && properties.contains(QStringLiteral("Base"))) {
        return FaaLayer::NationalSecurity;
    }
    if (properties.contains(QStringLiteral("TYPE_CODE")) && properties.contains(QStringLiteral("UPPER_CODE")) && properties.contains(QStringLiteral("LOWER_CODE"))) {
        return properties.contains(QStringLiteral("LOCAL_TYPE")) ? FaaLayer::ClassAirspace : FaaLayer::SpecialUse;
    }
    return FaaLayer::Unknown;
}

QString _trimmed(const QJsonObject &properties, const char *name)
{
    return _text(properties.value(QLatin1String(name))).trimmed();
}

/// Sets a limit of the volume, in feet, from the value, unit and code of the FAA airspace layers: SFC above ground,
/// MSL (or no code) above mean sea level, STD flight levels, UNLTD without limit. Unknown limits stay unspecified.
void _setFaaLimit(GeoZone::Volume &volume, bool upper, const QJsonValue &valueJson, const QString &unit, const QString &code)
{
    double value = _number(valueJson);
    QString reference = code.isEmpty() ? QStringLiteral("MSL") : code;
    if (code == QLatin1String("UNLTD")) {
        value = upper ? kUnlimitedSentinel : 0;
        reference = upper ? QStringLiteral("MSL") : QStringLiteral("SFC");
    } else if (std::isnan(value) || (value <= -9998)) {
        // -9998 is up to the overlying airspace, which the layer doesn't give
        value = qQNaN();
    } else if ((unit == QLatin1String("FL")) || (code == QLatin1String("STD"))) {
        value *= 100;
        reference = QStringLiteral("STD");
    }
    (upper ? volume.upperLimit : volume.lowerLimit) = value;
    volume.setReferences(upper ? volume.lowerReferenceCode : reference, upper ? reference : volume.upperReferenceCode);
}

/// A national security restriction limit in feet with its reference
struct FaaLimitText {
    double feet = qQNaN();
    QString reference;
};

/// @return One limit like "Surface", "400' AGL" or "1,200 ft MSL", nothing when the text isn't one. The reference is
///         empty when the text gives none.
std::optional<FaaLimitText> _faaLimitPart(const QString &text)
{
    static const QStringList surface = { QStringLiteral("SURFACE"), QStringLiteral("SFC"), QStringLiteral("GND"), QStringLiteral("GROUND") };
    if (surface.contains(text)) {
        return FaaLimitText{ 0, QStringLiteral("SFC") };
    }
    // A number with at least a digit, whose commas group thousands, then the unit and the reference
    static const QRegularExpression limitRegExp(QRegularExpression::anchoredPattern(
        QStringLiteral("(\\d{1,3}(?:,\\d{3})+|\\d+)(\\.\\d+)?\\s*(?:'|FT\\.?|FEET|FOOT)?\\s*(AGL|MSL|AMSL)?")));
    const QRegularExpressionMatch match = limitRegExp.match(text);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    bool ok = false;
    const double feet = QString(match.captured(1).remove(QLatin1Char(',')) + match.captured(2)).toDouble(&ok);
    if (!ok || !std::isfinite(feet)) {
        return std::nullopt;
    }
    const QString reference = match.captured(3);
    return FaaLimitText{ feet, reference.isEmpty() ? QString() : ((reference == QStringLiteral("AGL")) ? QStringLiteral("SFC") : QStringLiteral("MSL")) };
}

/// @return The floor or ceiling of a national security restriction from its text, like "Surface", "400' AGL" or
///         "1,200 ft MSL". A range like "0-400 FT AGL" gives its upper bound for the ceiling and its lower bound for
///         the floor. The limit is unknown, with an issue, unless the whole text is understood, tells its reference
///         and gives a ceiling above the surface: the restriction then applies at all altitudes on that side rather
///         than up to a wrong ceiling.
FaaLimitText _faaLimitText(const QString &text, bool upper, QStringList &issues)
{
    const QString value = text.trimmed().toUpper();
    if (value.isEmpty()) {
        return FaaLimitText();
    }
    static const QStringList unlimited = { QStringLiteral("UNLIMITED"), QStringLiteral("UNLTD"), QStringLiteral("UNL") };
    if (upper && unlimited.contains(value)) {
        return FaaLimitText{ kUnlimitedSentinel, QStringLiteral("MSL") };
    }

    std::optional<FaaLimitText> limit = _faaLimitPart(value);
    if (!limit) {
        static const QRegularExpression rangeRegExp(QStringLiteral("^(.+?)\\s*(?:-|\\bTO\\b)\\s*(.+)$"));
        const QRegularExpressionMatch range = rangeRegExp.match(value);
        std::optional<FaaLimitText> first = range.hasMatch() ? _faaLimitPart(range.captured(1)) : std::nullopt;
        const std::optional<FaaLimitText> second = range.hasMatch() ? _faaLimitPart(range.captured(2)) : std::nullopt;
        if (first && second) {
            // "0-400 FT AGL": the reference is that of both bounds
            if (first->reference.isEmpty()) {
                first->reference = second->reference;
            }
            if (first->reference == second->reference) {
                limit = ((first->feet > second->feet) == upper) ? first : second;
            } else {
                limit = upper ? second : first;
            }
        }
    }
    // A ceiling at the surface or the sea level would leave no altitude for the restriction to apply at
    if (!limit || limit->reference.isEmpty() || (upper && (limit->feet <= 0))) {
        constexpr qsizetype maxShown = 40;
        issues.append(upper ? GeoZoneParser::tr("Ceiling \"%1\" isn't understood, the zone has no ceiling").arg(text.trimmed().left(maxShown))
                            : GeoZoneParser::tr("Floor \"%1\" isn't understood, the zone starts at the ground").arg(text.trimmed().left(maxShown)));
        return FaaLimitText();
    }
    return *limit;
}

void _faaCommon(GeoZone::Data &zone)
{
    zone.country = QStringLiteral("USA");
    zone.extendedProperties.append(GeoZoneParser::tr("Source: FAA public data, informational. Check B4UFLY or a LAANC provider before flying."));
}

GeoZoneFormat::FeatureResult _facilityMapFeature(const QJsonObject &feature, const QJsonObject &properties, GeoZone::Data &zone)
{
    const double ceiling = _number(properties.value(QStringLiteral("CEILING")));
    const QString airport = _trimmed(properties, "APT1_NAME");
    const QString icao = _trimmed(properties, "APT1_ICAO");
    const QString airspaceClass = _trimmed(properties, "AIRSPACE_1");
    const bool laanc = _number(properties.value(QStringLiteral("APT1_LAANC"))) == 1;
    const QString ceilingText = QString::number(ceiling, 'f', 0);

    zone.identifier = _trimmed(properties, "GLOBALID");
    if (zone.identifier.isEmpty()) {
        zone.identifier = _text(properties.value(QStringLiteral("OBJECTID")));
    }
    // Short, since the cells are small and many
    const QString airportCode = icao.isEmpty() ? _trimmed(properties, "APT1_FAAID") : icao;
    zone.name = GeoZoneParser::tr("%1 facility map, %2 ft").arg(airportCode.isEmpty() ? airport : airportCode, ceilingText);
    zone.restrictionCode = QStringLiteral("UASFM %1 FT").arg(ceilingText);
    zone.reasons.append(QStringLiteral("AIR_TRAFFIC"));
    const QString airspace = airspaceClass.isEmpty() ? GeoZoneParser::tr("Controlled airspace") : GeoZoneParser::tr("Class %1 airspace").arg(airspaceClass);
    if (ceiling > 0) {
        zone.restriction = GeoZone::ReqAuthorisation;
        zone.restrictionConditions = laanc ? GeoZoneParser::tr("LAANC up to %1 ft AGL").arg(ceilingText)
                                           : GeoZoneParser::tr("Authorisation up to %1 ft AGL through FAADroneZone, no LAANC at this airport").arg(ceilingText);
        zone.message = GeoZoneParser::tr("%1 of %2. Part 107 flights need an FAA airspace authorisation, which can be granted up to %3 ft above ground here; higher flights need further coordination.")
                           .arg(airspace, airport, ceilingText);
    } else {
        zone.restriction = GeoZone::Prohibited;
        zone.restrictionConditions = GeoZoneParser::tr("No flights without further coordination with the FAA");
        zone.message = GeoZoneParser::tr("%1 of %2 where the UAS facility map allows 0 ft: flights are prohibited without further coordination with the FAA (FAADroneZone).")
                           .arg(airspace, airport);
    }
    zone.authorities.append(icao.isEmpty() ? QStringLiteral("FAA") : QStringLiteral("FAA / %1 (%2)").arg(airport, icao));
    const QString effective = _trimmed(properties, "MAP_EFF");
    if (!effective.isEmpty()) {
        zone.extendedProperties.append(GeoZoneParser::tr("Map effective: %1").arg(effective));
    }
    const QString otherAirport = _trimmed(properties, "APT2_NAME");
    if (!otherAirport.isEmpty()) {
        zone.extendedProperties.append(GeoZoneParser::tr("Also in the facility map of: %1").arg(otherAirport));
    }

    // Authorisation is needed from the surface up, whatever the altitude; the ceiling is what can be granted
    GeoZone::Volume layer;
    layer.feet = true;
    layer.lowerLimit = 0;
    layer.upperLimit = kUnlimitedSentinel;
    layer.setReferences(QStringLiteral("SFC"), QStringLiteral("MSL"));
    _appendHorizontalGeometry(feature.value(QStringLiteral("geometry")).toObject(), layer, 1.0, zone.volumes);
    return GeoZoneFormat::featureResult(zone);
}

GeoZoneFormat::FeatureResult _nationalSecurityFeature(const QJsonObject &feature, const QJsonObject &properties, GeoZone::Data &zone)
{
    const QString base = _trimmed(properties, "Base");
    const QString facility = _trimmed(properties, "Facility");
    zone.identifier = _trimmed(properties, "FAA_ID");
    if (zone.identifier.isEmpty()) {
        zone.identifier = _text(properties.value(QStringLiteral("OBJECTID")));
    }
    zone.name = (facility.isEmpty() || (facility == base)) ? base : QStringLiteral("%1, %2").arg(base, facility);
    zone.restrictionCode = QStringLiteral("NSUFR");
    zone.restriction = GeoZone::Prohibited;
    zone.reasons.append(QStringLiteral("SENSITIVE"));
    zone.otherReasonInfo = _trimmed(properties, "Reason");
    zone.message = GeoZoneParser::tr("National security UAS flight restriction (14 CFR 99.7 special security instructions): drone flights are prohibited "
                                     "between its floor and ceiling unless the FAA and the facility authorised them.");
    const QStringList proponent = { _trimmed(properties, "Proponent"), _trimmed(properties, "Branch") };
    QStringList authority;
    for (const QString &part : proponent) {
        if (!part.isEmpty()) {
            authority.append(part);
        }
    }
    zone.authorities.append(authority.isEmpty() ? QStringLiteral("FAA") : authority.join(QStringLiteral(" / ")));
    const QString contact = _trimmed(properties, "POC");
    if (!contact.isEmpty()) {
        zone.authorities.append(GeoZoneParser::tr("Contact: %1").arg(contact));
    }

    GeoZone::Volume layer;
    layer.feet = true;
    const FaaLimitText floor = _faaLimitText(_trimmed(properties, "Floor"), false, zone.issues);
    const FaaLimitText ceiling = _faaLimitText(_trimmed(properties, "Ceiling"), true, zone.issues);
    layer.lowerLimit = floor.feet;
    layer.upperLimit = ceiling.feet;
    layer.setReferences(floor.reference, ceiling.reference);
    _appendHorizontalGeometry(feature.value(QStringLiteral("geometry")).toObject(), layer, 1.0, zone.volumes);
    return GeoZoneFormat::featureResult(zone);
}

/// Class airspace or special use airspace, which share their limit fields
GeoZoneFormat::FeatureResult _airspaceFeature(const QJsonObject &feature, const QJsonObject &properties, FaaLayer layerType, GeoZone::Data &zone)
{
    zone.identifier = _trimmed(properties, "GLOBAL_ID");
    zone.name = _trimmed(properties, "NAME");
    const QString type = _trimmed(properties, "TYPE_CODE");

    if (layerType == FaaLayer::ClassAirspace) {
        // Class A and E airspace starting above the surface don't concern drones flying under 400 ft, and are left out
        // on purpose like the Mode C veils
        const QString localType = _trimmed(properties, "LOCAL_TYPE");
        static const QStringList surfaceClasses = { QStringLiteral("CLASS_B"), QStringLiteral("CLASS_C"), QStringLiteral("CLASS_D"),
                                                    QStringLiteral("CLASS_E2"), QStringLiteral("CLASS_E3"), QStringLiteral("CLASS_E4") };
        if ((type != QStringLiteral("CLASS")) || !surfaceClasses.contains(localType)) {
            return GeoZoneFormat::FeatureResult::Filtered;
        }
        const QString airspaceClass = _trimmed(properties, "CLASS");
        zone.restrictionCode = localType;
        zone.restriction = GeoZone::ReqAuthorisation;
        zone.reasons.append(QStringLiteral("AIR_TRAFFIC"));
        zone.message = GeoZoneParser::tr("Class %1 controlled airspace. Part 107 flights need an FAA airspace authorisation, through LAANC or FAADroneZone; "
                                         "the UAS facility maps tell the heights LAANC can authorise.").arg(airspaceClass);
        const QString hours = _trimmed(properties, "WKHR_RMK");
        const QString hoursCode = _trimmed(properties, "WKHR_CODE");
        if (!hours.isEmpty() || !hoursCode.isEmpty()) {
            zone.extendedProperties.append(GeoZoneParser::tr("Hours: %1").arg(hours.isEmpty() ? hoursCode : hours));
        }
    } else {
        zone.restrictionCode = type;
        const QString timesOfUse = _trimmed(properties, "TIMESOFUSE");
        if (type == QStringLiteral("P")) {
            zone.restriction = GeoZone::Prohibited;
            zone.reasons.append(QStringLiteral("SENSITIVE"));
            zone.message = GeoZoneParser::tr("Prohibited area: flights are prohibited.");
        } else if (type == QStringLiteral("R")) {
            zone.restriction = GeoZone::ReqAuthorisation;
            zone.reasons.append(QStringLiteral("OTHER"));
            zone.otherReasonInfo = GeoZoneParser::tr("Restricted area");
            zone.message = GeoZoneParser::tr("Restricted area: flights need the permission of the using or controlling agency while it is active.");
        } else {
            static const QHash<QString, const char *> kinds = {
                { QStringLiteral("W"), QT_TRANSLATE_NOOP("GeoZoneParser", "Warning area: activities hazardous to aircraft may be in progress.") },
                { QStringLiteral("A"), QT_TRANSLATE_NOOP("GeoZoneParser", "Alert area: a high volume of pilot training or unusual aerial activity.") },
                { QStringLiteral("MOA"), QT_TRANSLATE_NOOP("GeoZoneParser", "Military operations area: military training activities while it is active, flights are allowed with caution.") },
                { QStringLiteral("D"), QT_TRANSLATE_NOOP("GeoZoneParser", "Danger area: activities dangerous to aircraft may be in progress.") },
            };
            zone.restriction = GeoZone::Conditional;
            zone.reasons.append(QStringLiteral("OTHER"));
            zone.otherReasonInfo = GeoZoneParser::tr("Special use airspace (%1)").arg(type);
            zone.message = kinds.contains(type) ? GeoZoneParser::tr(kinds.value(type)) : GeoZoneParser::tr("Special use airspace: check its times of use before flying.");
        }
        if (!timesOfUse.isEmpty()) {
            zone.restrictionConditions = GeoZoneParser::tr("Times of use: %1").arg(timesOfUse);
        }
        const QString remarks = _trimmed(properties, "REMARKS");
        if (!remarks.isEmpty()) {
            zone.extendedProperties.append(GeoZoneParser::tr("Remarks: %1").arg(remarks));
        }
    }

    const QString agency = _trimmed(properties, "CONT_AGENT");
    zone.authorities.append(agency.isEmpty() ? QStringLiteral("FAA") : agency);
    const QString location = QStringList{ _trimmed(properties, "CITY"), _trimmed(properties, "STATE") }.join(QStringLiteral(", "));
    if (location.length() > 2) {
        zone.extendedProperties.append(GeoZoneParser::tr("Location: %1").arg(location));
    }

    GeoZone::Volume layer;
    layer.feet = true;
    _setFaaLimit(layer, false, properties.value(QStringLiteral("LOWER_VAL")), _trimmed(properties, "LOWER_UOM"), _trimmed(properties, "LOWER_CODE"));
    _setFaaLimit(layer, true, properties.value(QStringLiteral("UPPER_VAL")), _trimmed(properties, "UPPER_UOM"), _trimmed(properties, "UPPER_CODE"));
    _appendHorizontalGeometry(feature.value(QStringLiteral("geometry")).toObject(), layer, 1.0, zone.volumes);
    return GeoZoneFormat::featureResult(zone);
}

GeoZoneFormat::FeatureResult _faaAirspaceFeature(const QJsonObject &feature, GeoZone::Data &zone)
{
    const QJsonObject properties = feature.value(QStringLiteral("properties")).toObject();
    const FaaLayer layer = _faaLayer(properties);
    _faaCommon(zone);
    switch (layer) {
    case FaaLayer::FacilityMap:
        return _facilityMapFeature(feature, properties, zone);
    case FaaLayer::NationalSecurity:
        return _nationalSecurityFeature(feature, properties, zone);
    case FaaLayer::ClassAirspace:
    case FaaLayer::SpecialUse:
        return _airspaceFeature(feature, properties, layer, zone);
    case FaaLayer::Unknown:
        break;
    }
    return GeoZoneFormat::FeatureResult::Unusable;
}

/// GeoJSON of the FAA's public ArcGIS feature services: UAS facility maps, national security UAS flight restrictions,
/// class airspace and special use airspace. The data is informational, the FAA doesn't publish it as geozones.
class GeoZoneFaaAirspaceFormat : public GeoZoneFormat
{
public:
    GeoZoneParser::Format format() const final { return GeoZoneParser::Format::FaaAirspace; }
    QString name() const final { return QStringLiteral("FAA airspace"); }
    int detectionOrder() const final { return 15; }
    bool detect(const QJsonObject &, const QJsonArray &features, GeoZoneParser::Format formatHint) const final
    {
        // ArcGIS answers an area without features with an empty collection
        if (features.isEmpty()) {
            return formatHint == GeoZoneParser::Format::FaaAirspace;
        }
        return _faaLayer(features.first().toObject().value(QStringLiteral("properties")).toObject()) != FaaLayer::Unknown;
    }
    void readMetadata(const QJsonObject &root, GeoZoneParser::Result &result) const final
    {
        const QJsonArray features = root.value(QStringLiteral("features")).toArray();
        const FaaLayer layer = features.isEmpty() ? FaaLayer::Unknown : _faaLayer(features.first().toObject().value(QStringLiteral("properties")).toObject());
        switch (layer) {
        case FaaLayer::FacilityMap:
            result.title = GeoZoneParser::tr("UAS facility maps (FAA, informational)");
            break;
        case FaaLayer::NationalSecurity:
            result.title = GeoZoneParser::tr("National security UAS flight restrictions (FAA, informational)");
            break;
        case FaaLayer::ClassAirspace:
            result.title = GeoZoneParser::tr("Controlled airspace (FAA, informational)");
            break;
        case FaaLayer::SpecialUse:
            result.title = GeoZoneParser::tr("Special use airspace (FAA, informational)");
            break;
        case FaaLayer::Unknown:
            result.title = GeoZoneParser::tr("FAA airspace (informational)");
            break;
        }
        result.provider = GeoZoneParser::tr("FAA (informational)");
    }
    bool acceptsEmpty() const final { return true; }
    FeatureResult parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const final { return _faaAirspaceFeature(feature, zone); }
};

} // namespace

GEOZONE_REGISTER_FORMAT(GeoZoneFaaAirspaceFormat)
