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

/// EUROCAE ED-269 JSON
class GeoZoneEd269Format : public GeoZoneFormat
{
public:
    GeoZoneParser::Format format() const final { return GeoZoneParser::Format::ED269; }
    QString name() const final { return QStringLiteral("ED-269"); }
    /// The fallback, ED-269 has no identifying root members
    bool detect(const QJsonObject &, const QJsonArray &, GeoZoneParser::Format) const final { return true; }
    void readMetadata(const QJsonObject &root, GeoZoneParser::Result &result) const final
    {
        result.title = _text(root.value(QStringLiteral("title")));
    }
    bool parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const final { return _ed269Feature(feature, zone); }
};

} // namespace

const GeoZoneFormat *GeoZoneFormat::ed269Format()
{
    static const GeoZoneEd269Format instance;
    return &instance;
}
