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

/// French UAS restrictions GeoJSON of the IGN Géoplateforme WFS
class GeoZoneFranceUasFormat : public GeoZoneFormat
{
public:
    GeoZoneParser::Format format() const final { return GeoZoneParser::Format::FranceUas; }
    QString name() const final { return QStringLiteral("IGN UAS"); }
    bool detect(const QJsonObject &root, const QJsonArray &features, GeoZoneParser::Format formatHint) const final
    {
        if (features.isEmpty()) {
            return isEmptyWfsResponse(root) && (formatHint == GeoZoneParser::Format::FranceUas);
        }
        const QJsonObject properties = features.first().toObject().value(QStringLiteral("properties")).toObject();
        return properties.contains(QStringLiteral("limite"));
    }
    void readMetadata(const QJsonObject &root, GeoZoneParser::Result &result) const final
    {
        result.title = GeoZoneParser::tr("UAS restrictions (France)");
        result.provider = QStringLiteral("DGAC / IGN");
        result.issued = _dateTime(root.value(QStringLiteral("timeStamp")));
    }
    bool acceptsEmpty() const final { return true; }
    bool parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const final { return _franceFeature(feature, zone); }
};

} // namespace

const GeoZoneFormat *GeoZoneFormat::franceUasFormat()
{
    static const GeoZoneFranceUasFormat instance;
    return &instance;
}
