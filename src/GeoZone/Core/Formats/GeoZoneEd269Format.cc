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

#include <algorithm>

using namespace GeoZoneParserUtils;

namespace {

bool _ed269Feature(const QJsonObject &feature, GeoZone::Data &zone)
{
    _commonProperties(feature, QStringLiteral("restriction"), zone);

    for (const QJsonValue &geometryValue : feature.value(QStringLiteral("geometry")).toArray()) {
        const QJsonObject geometry = geometryValue.toObject();

        GeoZone::Volume layer;
        // Limits in an unknown unit are unknown, which applies the zone at all altitudes
        const bool knownUnit = _lengthUnit(geometry.value(QStringLiteral("uomDimensions")), layer.feet, zone.issues);
        layer.lowerLimit = knownUnit ? _number(geometry.value(QStringLiteral("lowerLimit"))) : qQNaN();
        layer.upperLimit = knownUnit ? _number(geometry.value(QStringLiteral("upperLimit"))) : qQNaN();
        layer.setReferences(geometry.value(QStringLiteral("lowerVerticalReference")).toString(), geometry.value(QStringLiteral("upperVerticalReference")).toString());

        // ED-269 expresses all dimensions, including the circle radius, in uomDimensions
        _appendHorizontalGeometry(geometry.value(QStringLiteral("horizontalProjection")).toObject(), layer, layer.feet ? kMetersPerFoot : 1.0, zone.volumes);
    }
    zone.applicability = _applicability(feature.value(QStringLiteral("applicability")), zone.country, zone.volumes, zone.issues);

    return !zone.volumes.isEmpty();
}

/// EUROCAE ED-269 JSON
class GeoZoneEd269Format : public GeoZoneFormat
{
public:
    GeoZoneParser::Format format() const final { return GeoZoneParser::Format::ED269; }
    QString name() const final { return QStringLiteral("ED-269"); }
    int detectionOrder() const final { return 60; }
    /// ED-269 has no identifying root members, its features have a list of geometries with horizontal projections
    bool detect(const QJsonObject &, const QJsonArray &features, GeoZoneParser::Format) const final
    {
        constexpr qsizetype checkedFeatures = 20;
        for (qsizetype i = 0; i < (std::min)(features.count(), checkedFeatures); i++) {
            const QJsonObject feature = features[i].toObject();
            const QJsonValue geometry = feature.value(QStringLiteral("geometry"));
            const QJsonArray geometries = geometry.toArray();
            if (!geometries.isEmpty() && geometries.first().toObject().contains(QStringLiteral("horizontalProjection"))) {
                return true;
            }
            if (geometry.isArray() && feature.contains(QStringLiteral("identifier")) && feature.contains(QStringLiteral("restriction"))) {
                return true;
            }
        }
        return false;
    }
    void readMetadata(const QJsonObject &root, GeoZoneParser::Result &result) const final
    {
        result.title = _text(root.value(QStringLiteral("title")));
    }
    bool parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const final { return _ed269Feature(feature, zone); }
};

} // namespace

GEOZONE_REGISTER_FORMAT(GeoZoneEd269Format)
