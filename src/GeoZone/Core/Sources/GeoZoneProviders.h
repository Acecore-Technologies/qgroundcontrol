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
        GeoZoneParser::Format format = GeoZoneParser::Format::Unknown;
        QString feedUrl;                    ///< URL of the whole dataset, empty for services only serving areas
        /// URL of the zones in the area, null for services without area downloads
        std::function<QString(const QGeoRectangle &area)> areaUrl;
        QGeoRectangle coverage;             ///< Area served by areaUrl
        /// Links to the current dataset on a download page, with the dataset's effective date (yyyyMMdd) as the
        /// second capture. Empty when the feed URL is the dataset.
        QRegularExpression datasetLinkPattern;
    };

    static constexpr double maxAreaDegrees = 1.0;   ///< Largest area downloaded from services serving zones by area

    static QList<Service> services();

    /// @return Maps with name and url of the services with a whole dataset, for QML
    static QVariantList feedPresets();
    /// @return Maps with name, id and coverage (rectangle) of the services serving zones by area, for QML
    static QVariantList areaProviders();
    /// @return URL downloading the zones of the service in the area, empty for an unknown service or a larger area
    ///         than maxAreaDegrees
    static QString areaFeedUrl(const QString &serviceId, const QGeoRectangle &area);

    /// @return The format to expect from a URL, Unknown for URLs of unknown services
    static GeoZoneParser::Format formatHint(const QString &url);

    /// @return URL of the current dataset linked from a download page, like the AIRAC datasets of NATS, or an empty
    ///         string when the data isn't such a page. The latest dataset effective today or earlier is chosen.
    static QString datasetLink(const QString &url, const QByteArray &data, const QDate &today);

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

private:
    static QString _bboxQuery(const QGeoRectangle &area, const QString &suffix = QString());
    static QString _dipulUrl(const QStringList &layers, const QGeoRectangle &area);
};
