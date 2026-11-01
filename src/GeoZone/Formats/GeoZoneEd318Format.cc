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
    volume.setReferences(layer.value(QStringLiteral("lowerReference")).toString(), layer.value(QStringLiteral("upperReference")).toString());
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

/// EUROCAE ED-318 GeoJSON
class GeoZoneEd318Format : public GeoZoneFormat
{
public:
    GeoZoneParser::Format format() const final { return GeoZoneParser::Format::ED318; }
    QString name() const final { return QStringLiteral("ED-318"); }
    int detectionOrder() const final { return 50; }
    bool detect(const QJsonObject &root, const QJsonArray &, GeoZoneParser::Format) const final
    {
        return root.value(QStringLiteral("type")).toString() == QStringLiteral("FeatureCollection");
    }
    void readMetadata(const QJsonObject &root, GeoZoneParser::Result &result) const final
    {
        result.title = _text(root.value(QStringLiteral("name")));
        if (result.title.isEmpty()) {
            result.title = _text(root.value(QStringLiteral("title")));
        }
    }
    bool parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const final { return _ed318Feature(feature, zone); }
};

} // namespace

GEOZONE_REGISTER_FORMAT(GeoZoneEd318Format)
