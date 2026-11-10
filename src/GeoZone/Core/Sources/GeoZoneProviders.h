/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 *
 ****************************************************************************/

#pragma once

#include "GeoZoneParser.h"
#include "GeoZoneService.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDate>
#include <QtCore/QRegularExpression>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoRectangle>

#include <functional>

/// The national geozone services QGroundControl knows: the feeds offered when adding a feed, the services serving
/// their zones by area, and how to recognize their URLs. Adding a service takes an entry in services().
class GeoZoneProviders
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneProviders)

public:
    struct Service {
        QString id;                         ///< Stable identifier
        QString name;                       ///< Shown to the user, translated
        QString host;                       ///< The service's URLs have this host or a subdomain of it
        QString path;                       ///< and a path starting with this one, for services sharing a host
        GeoZoneParser::Format format = GeoZoneParser::Format::Unknown;
        QString feedUrl;                    ///< URL of the whole dataset, empty for services only serving areas
        /// URL of the zones in the area, null for services without area downloads
        std::function<QString(const QGeoRectangle &area)> areaUrl;
        QGeoRectangle coverage;             ///< Area served by areaUrl
        /// Links to the current dataset on a download page, with the dataset's effective date as the second capture in
        /// datasetDateFormat. Without a second capture, the first link is the current dataset. Empty when the feed URL
        /// is the dataset.
        QRegularExpression datasetLinkPattern;
        QString datasetDateFormat = QStringLiteral("yyyyMMdd");
        bool airac = false;     ///< The datasets take effect by AIRAC cycle, see GeoZoneAirac
    };

    /// A service queried with credentials, an ED-318 POST or by the area of interest, which the user completes with its
    /// URL or secret
    struct ServiceTemplate {
        QString url;                ///< URL template, see GeoZoneService, empty when the user enters it
        GeoZoneService service;     ///< id and name are those of the template
        /// Name of the template when the service has none, whose sources then take the title of their data
        QString name;
        QString note;               ///< How to get access, or what QGroundControl doesn't read yet
        bool supported = true;      ///< false when QGroundControl can't read the data yet: configuration only
        /// Format of the service's responses, see formatHint, Unknown to detect it
        GeoZoneParser::Format format = GeoZoneParser::Format::Unknown;
    };

    static constexpr double maxAreaDegrees = 1.0;   ///< Largest area downloaded from services serving zones by area

    static QList<Service> services();
    static QList<ServiceTemplate> serviceTemplates();
    /// @return Maps with name, url, note, supported and form (GeoZoneService::toForm) of the service templates, and of
    ///         another service last, for the form of QML
    static QVariantList serviceTemplateList();
    /// @return The service of the template of serviceTemplateList at the index with the fields of the form
    static GeoZoneService serviceFromForm(int templateIndex, const QVariantMap &form);

    /// @return Maps with name and url of the services with a whole dataset, for QML
    static QVariantList feedPresets();
    /// @return Maps with name, id and coverage (rectangle) of the services serving zones by area, for QML
    static QVariantList areaProviders();
    /// @return URL downloading the zones of the service in the area, empty for an unknown service or a larger area
    ///         than maxAreaDegrees
    static QString areaFeedUrl(const QString &serviceId, const QGeoRectangle &area);
    /// @return Area of the zones a URL of areaFeedUrl downloads, invalid for URLs without an area
    static QGeoRectangle feedArea(const QString &url);
    /// @return true when one of the feeds downloads the zones of the service in the whole area, so downloading the area
    ///         again would add nothing
    static bool areaDownloaded(const QString &serviceId, const QStringList &feedUrls, const QGeoRectangle &area);

    /// @return The format to expect from a URL, of the services or the service templates, Unknown for URLs of unknown
    ///         services
    static GeoZoneParser::Format formatHint(const QString &url);
    /// @return Service::id of the service of a URL, empty for URLs of unknown services
    static QString serviceId(const QString &url);
    /// @return Service::name of the service whose feed or area URL this is, empty for other URLs
    static QString serviceName(const QString &url);

    /// @return URL of the current dataset linked from a download page, like the AIRAC datasets of NATS, or an empty
    ///         string when the data isn't such a page. The latest dataset effective today or earlier is chosen.
    static QString datasetLink(const QString &url, const QByteArray &data, const QDate &today);
    /// @return Effective date of a dataset linked from a download page (see datasetLink) when its service publishes
    ///         its datasets by AIRAC cycle, invalid otherwise
    static QDate airacDatasetDate(const QString &datasetUrl);

    /// @return true for formats of short-lived restrictions, refreshed and outdated after hours rather than days
    static bool isLive(GeoZoneParser::Format format);

    /// Feature counts of a WFS GeoJSON response, -1 when not given
    struct WfsCounts {
        qint64 matched = -1;    ///< numberMatched or totalFeatures
        qint64 returned = -1;   ///< numberReturned
    };
    /// @return The counts of a response from its first and last bytes, where servers write them, without parsing it
    static WfsCounts wfsCounts(const QByteArray &head, const QByteArray &tail);
    /// @return URL of the WFS GetFeature request returning the features from startIndex on, empty when the URL isn't
    ///         a GetFeature request
    static QString wfsPageUrl(const QString &url, qint64 startIndex);
    static constexpr int maxWfsPages = 50;     ///< Pages downloaded after the first of a response the server limited

    /// How the further pages of a response the server limited are requested
    struct Paging {
        enum class Scheme : quint8 {
            None,           ///< The response is complete
            WfsStartIndex,  ///< WFS GetFeature from STARTINDEX on, the counts of the response tell the pages
            ArcGisOffset,   ///< ArcGIS feature service query from resultOffset on, each page tells whether more follow
            PageNumber,     ///< Numbered pages, the response tells their count, like OpenAIP
        };
        Scheme scheme = Scheme::None;
        qint64 pageSize = 0;    ///< Features per page of the offset schemes
        int pageCount = 0;      ///< Further pages after the first, at most maxWfsPages. For ArcGIS the most requested.
    };
    /// @return How the further pages of a response are requested, from its first and last bytes, where servers write
    ///         their counts, without parsing it
    static Paging paging(const QString &url, const QByteArray &head, const QByteArray &tail);
    /// @return URL of a further page, 1 for the one after the first
    static QString pageUrl(const QString &url, const Paging &paging, int page);
    /// @return true when the page of an ArcGIS feature service query says more features follow
    static bool arcGisExceeded(const QByteArray &head, const QByteArray &tail);

private:
    /// @return Value of the first number member with the name in the bytes, -1 without
    static qint64 _jsonCount(const QByteArray &head, const QByteArray &tail, const char *name);
    /// @return The URL with the query item set, replacing an item with the same name in any case
    static QString _withQueryItem(const QString &url, const QString &name, const QString &value);
    static QString _bboxQuery(const QGeoRectangle &area, const QString &suffix = QString());
    static QString _dipulUrl(const QStringList &layers, const QGeoRectangle &area);
    /// @return URL of the query of a layer of the FAA's ArcGIS feature services, for the area or its {bbox} placeholder
    static QString _faaUrl(const QString &service, const QString &where, const QString &fields, const QString &bbox);
    /// @return true when the URL is one of the service's, see Service::host and Service::path
    static bool _matches(const Service &service, const QUrl &url);
};
