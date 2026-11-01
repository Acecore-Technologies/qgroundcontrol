/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFormat.h"

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

QList<const GeoZoneFormat*> &GeoZoneFormat::_registered()
{
    static QList<const GeoZoneFormat*> registered;
    return registered;
}

GeoZoneFormat::Registration::Registration(const GeoZoneFormat *format)
{
    _registered().append(format);
}

const QList<const GeoZoneFormat*> &GeoZoneFormat::formats()
{
    // Registered while the program starts, before worker threads use them
    static const QList<const GeoZoneFormat*> formats = []() {
        QList<const GeoZoneFormat*> sorted = _registered();
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

bool GeoZoneFormat::isEmptyWfsResponse(const QJsonObject &root)
{
    return root.contains(QStringLiteral("totalFeatures")) || root.contains(QStringLiteral("numberMatched"));
}
