/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFormat.h"

#include <QtCore/QJsonDocument>

#include <algorithm>

bool GeoZoneFormat::detectXml(QStringView, QStringView) const
{
    return false;
}

bool GeoZoneFormat::parseXml(const QByteArray &, GeoZoneParser::Result &, QString &) const
{
    return false;
}

bool GeoZoneFormat::detect(const QJsonObject &, const QJsonArray &, GeoZoneParser::Format) const
{
    return false;
}

void GeoZoneFormat::readMetadata(const QJsonObject &, GeoZoneParser::Result &) const
{
}

bool GeoZoneFormat::parseFeature(const QJsonObject &, GeoZone::Data &) const
{
    return false;
}

bool GeoZoneFormat::parseFeatureWithCompanions(const QJsonObject &feature, const GeoZoneParser::Companions &, GeoZone::Data &zone) const
{
    return parseFeature(feature, zone);
}

QList<GeoZoneFormat::CompanionRequest> GeoZoneFormat::companionRequests(const QUrl &, const QJsonObject &, const QJsonArray &) const
{
    return {};
}

// Defined by GEOZONE_REGISTER_FORMAT in the Formats directory
const GeoZoneFormat *GeoZoneAixmFormatInstance();
const GeoZoneFormat *GeoZoneDipulFormatInstance();
const GeoZoneFormat *GeoZoneEd269FormatInstance();
const GeoZoneFormat *GeoZoneEd318FormatInstance();
const GeoZoneFormat *GeoZoneFaaTfrFormatInstance();
const GeoZoneFormat *GeoZoneFranceUasFormatInstance();

const QList<const GeoZoneFormat*> &GeoZoneFormat::formats()
{
    static const QList<const GeoZoneFormat*> formats = []() {
        QList<const GeoZoneFormat*> sorted = {
            GeoZoneAixmFormatInstance(),
            GeoZoneDipulFormatInstance(),
            GeoZoneEd269FormatInstance(),
            GeoZoneEd318FormatInstance(),
            GeoZoneFaaTfrFormatInstance(),
            GeoZoneFranceUasFormatInstance(),
        };
        std::stable_sort(sorted.begin(), sorted.end(), [](const GeoZoneFormat *a, const GeoZoneFormat *b) { return a->detectionOrder() < b->detectionOrder(); });
        return sorted;
    }();
    return formats;
}

const GeoZoneFormat *GeoZoneFormat::find(GeoZoneParser::Format format)
{
    for (const GeoZoneFormat *candidate : formats()) {
        if (candidate->format() == format) {
            return candidate;
        }
    }
    return nullptr;
}

const GeoZoneFormat *GeoZoneFormat::detectJson(const QJsonObject &root, const QJsonArray &features, GeoZoneParser::Format formatHint,
                                               const QList<const GeoZoneFormat*> &candidates)
{
    for (const GeoZoneFormat *candidate : candidates) {
        if (!candidate->isXml() && candidate->detect(root, features, formatHint)) {
            return candidate;
        }
    }
    return nullptr;
}

QList<GeoZoneFormat::CompanionRequest> GeoZoneFormat::companionRequestsOf(const QUrl &documentUrl, const QByteArray &document, GeoZoneParser::Format formatHint,
                                                                         QJsonObject &root, const QList<const GeoZoneFormat*> &candidates)
{
    const QJsonDocument json = QJsonDocument::fromJson(document);
    if (!json.isObject()) {
        return {};
    }
    root = json.object();
    const QJsonArray features = root.value(QStringLiteral("features")).toArray();
    const GeoZoneFormat *const format = detectJson(root, features, formatHint, candidates);
    return format ? format->companionRequests(documentUrl, root, features) : QList<CompanionRequest>();
}

bool GeoZoneFormat::isEmptyWfsResponse(const QJsonObject &root)
{
    return root.contains(QStringLiteral("totalFeatures")) || root.contains(QStringLiteral("numberMatched"));
}
