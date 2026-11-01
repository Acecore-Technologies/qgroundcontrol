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

/// A geozone data format, which recognizes its documents and converts their features into zones. Formats are stateless
/// and used from worker threads. Adding a format takes a class in the Formats directory and an entry in formats().
class GeoZoneFormat
{
public:
    virtual ~GeoZoneFormat() = default;

    virtual GeoZoneParser::Format format() const = 0;
    /// Short name of the format, like "ED-318"
    virtual QString name() const = 0;

    /// XML formats parse whole documents with parseXml(), JSON formats one feature at a time
    virtual bool isXml() const { return false; }
    virtual bool parseXml(const QByteArray &xml, GeoZoneParser::Result &result, QString &errorString) const;

    /// @return true if the JSON document is in this format
    /// @param formatHint Format expected from the URL the document was downloaded from, see GeoZoneProviders::formatHint
    virtual bool detect(const QJsonObject &root, const QJsonArray &features, GeoZoneParser::Format formatHint) const;
    /// Sets the dataset title, provider and issue time of the result from the document
    virtual void readMetadata(const QJsonObject &root, GeoZoneParser::Result &result) const;
    /// @return true if the feature has a usable geometry
    virtual bool parseFeature(const QJsonObject &feature, GeoZone::Data &zone) const;
    /// Services answering with an empty list when no zones apply, unlike files which must have zones
    virtual bool acceptsEmpty() const { return false; }

    /// @return The formats, in the order in which they are detected. ED-269 is last, as the fallback.
    static const QList<const GeoZoneFormat*> &formats();
    /// @return The format, or null for Unknown
    static const GeoZoneFormat *find(GeoZoneParser::Format format);

    /// @return true for the response of a web feature service whose features are all filtered out
    static bool isEmptyWfsResponse(const QJsonObject &root);

    static const GeoZoneFormat *aixmFormat();
    static const GeoZoneFormat *dipulFormat();
    static const GeoZoneFormat *ed269Format();
    static const GeoZoneFormat *ed318Format();
    static const GeoZoneFormat *faaTfrFormat();
    static const GeoZoneFormat *franceUasFormat();
};
