/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "GeoZoneParser.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>

/// A geozone data format, which recognizes its documents and converts their features into zones. Formats are stateless
/// and used from worker threads. Adding a format takes a class in the Formats directory registered with
/// GEOZONE_REGISTER_FORMAT and listed in formats(), a GeoZoneParser::Format value, and its services in
/// GeoZoneProviders::services().
class GeoZoneFormat
{
public:
    virtual ~GeoZoneFormat() = default;

    virtual GeoZoneParser::Format format() const = 0;
    /// Short name of the format, like "ED-318"
    virtual QString name() const = 0;
    /// Formats are detected in increasing order. Those recognized by their feature properties come before ED-318,
    /// whose FeatureCollection type they share.
    virtual int detectionOrder() const = 0;
    /// @return true for short-lived restrictions, refreshed and outdated after hours rather than days
    virtual bool isLive() const { return false; }

    /// XML formats parse whole documents with parseXml(), JSON formats one feature at a time
    virtual bool isXml() const { return false; }
    /// @return true if an XML document with the root element is in this format
    virtual bool detectXml(QStringView rootElement, QStringView namespaceUri) const;
    virtual bool parseXml(const QByteArray &xml, GeoZoneParser::Result &result, QString &errorString) const;

    /// @return true if the JSON document is in this format
    /// @param formatHint Format expected from the URL the document was downloaded from, see GeoZoneProviders::formatHint
    virtual bool detect(const QJsonObject &root, const QJsonArray &features, GeoZoneParser::Format formatHint) const;
    /// Sets the dataset title, provider and issue time of the result from the document
    virtual void readMetadata(const QJsonObject &root, GeoZoneParser::Result &result) const;
    /// @return true if the feature has a usable geometry
    virtual bool parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const;
    /// Like parseFeature, for formats whose features refer to companion documents
    virtual bool parseFeatureWithCompanions(const QJsonObject &feature, const GeoZoneParser::Companions &companions, GeoZone::Data &zone) const;

    /// Companion document of a feed's document, downloaded with it and kept in the stored copy
    struct CompanionRequest {
        QString key;
        QString version;    ///< A known companion with this version isn't downloaded again
        QString url;
    };
    /// @return The companion documents the features refer to, none by default
    /// @param documentUrl Where the document was downloaded from
    virtual QList<CompanionRequest> companionRequests(const QUrl &documentUrl, const QJsonObject &root, const QJsonArray &features) const;
    /// @return The companion documents of a downloaded JSON document, from the first of the formats recognizing it.
    ///         None for documents of other formats, and documents which aren't JSON objects.
    /// @param formatHint See detect()
    /// @param root Set to the document, which GeoZoneParser::composite() stores with its companions
    static QList<CompanionRequest> companionRequestsOf(const QUrl &documentUrl, const QByteArray &document, GeoZoneParser::Format formatHint,
                                                       QJsonObject &root, const QList<const GeoZoneFormat*> &candidates = formats());
    /// Services answering with an empty list when no zones apply, unlike files which must have zones
    virtual bool acceptsEmpty() const { return false; }

    /// @return The registered formats, in the order in which they are detected
    static const QList<const GeoZoneFormat*> &formats();
    /// @return The first of the JSON formats recognizing the document, null if none does
    static const GeoZoneFormat *detectJson(const QJsonObject &root, const QJsonArray &features, GeoZoneParser::Format formatHint,
                                           const QList<const GeoZoneFormat*> &candidates = formats());
    /// @return The format, or null for Unknown
    static const GeoZoneFormat *find(GeoZoneParser::Format format);

    /// @return true for the response of a web feature service whose features are all filtered out
    static bool isEmptyWfsResponse(const QJsonObject &root);
};

/// Defines the instance of a format class for GeoZoneFormat::formats(). Formats don't add themselves while the
/// program starts since the linker leaves the files of the GeoZoneCore static library out unless they are referenced.
#define GEOZONE_REGISTER_FORMAT(Class) \
    const GeoZoneFormat *Class##Instance(); \
    const GeoZoneFormat *Class##Instance() \
    { \
        static const Class instance; \
        return &instance; \
    }
