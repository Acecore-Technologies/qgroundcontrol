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

#include <QtCore/QJsonArray>

using namespace GeoZoneParserUtils;

namespace {

GeoZone::Volume _ed318Layer(const QJsonValue &value, const GeoZone::Volume &fallback, QStringList &issues)
{
    if (!value.isObject()) {
        return fallback;
    }

    const QJsonObject layer = value.toObject();
    GeoZone::Volume volume;
    // Limits in an unknown unit are unknown, which applies the zone at all altitudes. ENAIRE names the unit UOM.
    const QJsonValue unit = layer.contains(QStringLiteral("uom")) ? layer.value(QStringLiteral("uom")) : layer.value(QStringLiteral("UOM"));
    const bool knownUnit = _lengthUnit(unit, volume.feet, issues);
    volume.lowerLimit = knownUnit ? _number(layer.value(QStringLiteral("lower"))) : qQNaN();
    volume.upperLimit = knownUnit ? _number(layer.value(QStringLiteral("upper"))) : qQNaN();
    volume.setReferences(layer.value(QStringLiteral("lowerReference")).toString(), layer.value(QStringLiteral("upperReference")).toString());
    return volume;
}

void _ed318Geometry(const QJsonObject &geometry, const GeoZone::Volume &parentLayer, QList<GeoZone::Volume> &volumes, QStringList &issues)
{
    const GeoZone::Volume layer = _ed318Layer(geometry.value(QStringLiteral("layer")), parentLayer, issues);

    if (geometry.value(QStringLiteral("type")).toString() == QStringLiteral("GeometryCollection")) {
        for (const QJsonValue &child : geometry.value(QStringLiteral("geometries")).toArray()) {
            _ed318Geometry(child.toObject(), layer, volumes, issues);
        }
        return;
    }

    // ED-318 circle radius is always expressed in meters, the layer uom only applies to vertical limits
    _appendHorizontalGeometry(geometry, layer, 1.0, volumes);
}


/// The properties of a zone published by ENAIRE (Spain), the national ED-318 publication of its AIS: the downloads put
/// the zone's members in a UASZone object and its limits in a verticalLayer object, its feature service has them all
/// as attributes of the feature. Returns the properties as ED-318 has them and the vertical layer of the zone, an
/// empty object when the properties have none.
QJsonObject _enaireProperties(const QJsonObject &properties, QJsonObject &layer)
{
    QJsonObject zoneProperties = properties;
    const QJsonObject uasZone = properties.value(QStringLiteral("UASZone")).toObject();
    if (!uasZone.isEmpty()) {
        for (auto it = uasZone.constBegin(); it != uasZone.constEnd(); ++it) {
            zoneProperties.insert(it.key(), it.value());
        }
        layer = properties.value(QStringLiteral("verticalLayer")).toObject();
        return zoneProperties;
    }

    // The attributes of the feature service
    const auto has = [&properties](const char *key) {
        const QJsonValue value = properties.value(QLatin1String(key));
        return !value.isNull() && !value.isUndefined() && !(value.isString() && value.toString().trimmed().isEmpty());
    };
    if (has("uom") && (has("lower") || has("upper"))) {
        for (const char *key : { "uom", "lower", "lowerReference", "upper", "upperReference" }) {
            layer.insert(QLatin1String(key), properties.value(QLatin1String(key)));
        }
    }
    if (!properties.contains(QStringLiteral("zoneAuthority")) && (has("purpose") || has("email") || has("phone") || has("siteURL") || has("name_authority"))) {
        QJsonObject authority;
        for (const char *key : { "purpose", "intervalBefore", "service", "contactName", "siteURL", "email", "phone" }) {
            authority.insert(QLatin1String(key), properties.value(QLatin1String(key)));
        }
        authority.insert(QStringLiteral("name"), properties.value(QStringLiteral("name_authority")));
        zoneProperties.insert(QStringLiteral("zoneAuthority"), QJsonArray{ authority });
    }
    if (!properties.value(QStringLiteral("limitedApplicability")).isArray() &&
        (has("startDateTime") || has("endDateTime") || has("startTime") || has("endTime") || has("startEvent") || has("endEvent"))) {
        QJsonObject daily;
        for (const char *key : { "day", "startTime", "startEvent", "endTime", "endEvent" }) {
            daily.insert(QLatin1String(key), properties.value(QLatin1String(key)));
        }
        const QJsonObject period{ { QStringLiteral("startDateTime"), properties.value(QStringLiteral("startDateTime")) },
                                  { QStringLiteral("endDateTime"), properties.value(QStringLiteral("endDateTime")) },
                                  { QStringLiteral("schedule"), QJsonArray{ daily } } };
        zoneProperties.insert(QStringLiteral("limitedApplicability"), QJsonArray{ period });
    }
    return zoneProperties;
}

/// @return The applicability without the daily periods which restrict nothing, like ENAIRE's schedules of any day
///         without times, so that such zones are permanent rather than daily
QJsonArray _withoutEmptySchedules(const QJsonValue &value)
{
    const auto empty = [](const QJsonValue &field) { return field.isNull() || field.isUndefined() || (field.isString() && field.toString().trimmed().isEmpty()); };
    QJsonArray periods;
    for (const QJsonValue &periodValue : value.toArray()) {
        QJsonObject period = periodValue.toObject();
        QJsonArray schedule;
        for (const QJsonValue &dailyValue : period.value(QStringLiteral("schedule")).toArray()) {
            const QJsonObject daily = dailyValue.toObject();
            const QString day = daily.value(QStringLiteral("day")).isString() ? daily.value(QStringLiteral("day")).toString().trimmed().toUpper() : QStringLiteral("-");
            const bool anyDay = day.isEmpty() || (day == QStringLiteral("ANY"));
            if (!anyDay || !empty(daily.value(QStringLiteral("startTime"))) || !empty(daily.value(QStringLiteral("endTime"))) ||
                !empty(daily.value(QStringLiteral("startEvent"))) || !empty(daily.value(QStringLiteral("endEvent")))) {
                schedule.append(daily);
            }
        }
        if (schedule.isEmpty()) {
            (void) period.remove(QStringLiteral("schedule"));
        } else {
            period.insert(QStringLiteral("schedule"), schedule);
        }
        if (empty(period.value(QStringLiteral("startDateTime"))) && empty(period.value(QStringLiteral("endDateTime"))) && schedule.isEmpty()) {
            continue;
        }
        periods.append(period);
    }
    return periods;
}

GeoZoneFormat::FeatureResult _ed318Feature(const QJsonObject &feature, GeoZone::Data &zone)
{
    QJsonObject layer;
    const QJsonObject properties = _enaireProperties(feature.value(QStringLiteral("properties")).toObject(), layer);
    _commonProperties(properties, QStringLiteral("type"), zone);
    if (zone.identifier.isEmpty()) {
        zone.identifier = _text(feature.value(QStringLiteral("id")));
    }
    const GeoZone::Volume zoneLayer = layer.isEmpty() ? GeoZone::Volume() : _ed318Layer(layer, GeoZone::Volume(), zone.issues);
    _ed318Geometry(feature.value(QStringLiteral("geometry")).toObject(), zoneLayer, zone.volumes, zone.issues);
    const QJsonValue applicability = properties.value(QStringLiteral("limitedApplicability"));
    zone.applicability = _applicability(layer.isEmpty() ? applicability : QJsonValue(_withoutEmptySchedules(applicability)), zone.country, zone.volumes, zone.issues);

    return GeoZoneFormat::featureResult(zone);
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
    FeatureResult parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const final { return _ed318Feature(feature, zone); }
};

} // namespace

GEOZONE_REGISTER_FORMAT(GeoZoneEd318Format)
