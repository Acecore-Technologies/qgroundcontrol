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

/// FAA temporary flight restriction from the tfr.faa.gov WFS GeoJSON, which has no times or vertical limits
bool _faaTfrFeature(const QJsonObject &feature, GeoZone::Data &zone)
{
    const QJsonObject properties = feature.value(QStringLiteral("properties")).toObject();
    const QString notamKey = _text(properties.value(QStringLiteral("NOTAM_KEY")));
    // Keys look like 6/4240-1-FDC-F, the NOTAM number is the part before the first dash
    const QString notam = notamKey.section(QLatin1Char('-'), 0, 0);
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
    zone.message = GeoZoneParser::tr("Temporary flight restriction, FAA NOTAM %1. The NOTAM defines the times, altitudes and exceptions.").arg(notam);
    zone.authorities.append(QStringLiteral("FAA"));

    _appendHorizontalGeometry(feature.value(QStringLiteral("geometry")).toObject(), GeoZone::Volume(), 1.0, zone.volumes);
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
    bool parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const final { return _faaTfrFeature(feature, zone); }
};

} // namespace

GEOZONE_REGISTER_FORMAT(GeoZoneFaaTfrFormat)
