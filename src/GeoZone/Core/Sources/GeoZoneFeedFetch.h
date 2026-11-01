/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>

#include "GeoZoneCancelToken.h"
#include "GeoZoneDownloader.h"
#include "GeoZoneParser.h"
#include "GeoZoneService.h"

class GeoZoneFormat;

/// Downloads one feed for loading, in stages: the OAuth2 token of its service, the feed URL, which may be a page
/// linking to the current dataset, then that dataset, the further pages of a WFS response the server limited, and the
/// companion documents the format of the document refers to. The result is a document ready to load, the confirmation that the stored copy is current,
/// or a failure. Canceling stops the downloads and drops whatever arrives later.
class GeoZoneFeedFetch : public QObject
{
    Q_OBJECT

public:
    enum class Stage : quint8 {
        Idle,
        Token,          ///< Requesting an OAuth2 token of the service
        Feed,           ///< Downloading the feed URL
        Dataset,        ///< Downloading the dataset the feed's page linked to
        Pages,          ///< Downloading the further pages of a WFS response
        Companions,     ///< Downloading the companion documents of the features
        Finished,       ///< finished was emitted, or the fetch was canceled
    };

    struct Request {
        QString url;
        QString etag;               ///< Validators of the stored copy, empty to download unconditionally
        QString lastModified;
        QString datasetUrl;         ///< Dataset the validators belong to, empty when they are those of the feed URL
        GeoZoneParser::Format formatHint = GeoZoneParser::Format::Unknown;  ///< Format of the stored copy or expected from the URL
        GeoZoneParser::Companions companions;   ///< Of the stored copy, those of the current version aren't downloaded again
        QList<const GeoZoneFormat*> formats;    ///< Formats recognizing the document, GeoZoneFormat::formats() when empty
        GeoZoneService service;     ///< How the feed is requested, url being its URL template. Empty for a GET of url.
        QString secret;             ///< Of the service, see GeoZoneCredentials
        QString accessToken;        ///< OAuth2 token of the service still valid, empty to request one
        QGeoRectangle area;         ///< Area of interest of a service taking it
        QDateTime time;             ///< Start of the time of interest
    };

    struct Result {
        enum class Status : quint8 {
            Ready,          ///< filePath is the document to load
            NotModified,    ///< The server confirmed the validators of the request
            Failed,
        };
        Status status = Status::Failed;
        QString filePath;           ///< Document with its companions, which the receiver adopts or removes
        QStringList pagePaths;      ///< Further pages of a WFS response, see GeoZoneLoader::Job::pagePaths
        QString etag;               ///< Validators of the document
        QString lastModified;
        QString datasetUrl;         ///< Dataset the feed's page linked to, empty when the feed URL is the dataset
        QString errorString;
        qint64 maxAgeSecs = -1;     ///< Shortest time the server said the feed or its dataset stays current, negative without
        QString requestUrl;         ///< URL requested, the URL template filled with the area and time of interest
        QString accessToken;        ///< OAuth2 token requested by the fetch, empty when it requested none
        QDateTime tokenExpiresAt;
    };

    /// @param downloader Shared with other fetches, each cancels its own downloads
    /// @param cancelToken Canceled by cancel(), and shared with the load of the result
    GeoZoneFeedFetch(GeoZoneDownloader *downloader, const Request &request, const GeoZoneCancelToken &cancelToken = GeoZoneCancelToken(),
                     QObject *parent = nullptr);
    /// Cancels the fetch if it didn't finish
    ~GeoZoneFeedFetch() override;

    void start();
    /// Aborts the downloads and removes the files received. finished isn't emitted afterwards.
    void cancel();

    const Request &request() const { return _request; }
    Stage stage() const { return _stage; }
    GeoZoneCancelToken cancelToken() const { return _cancelToken; }

    static constexpr qint64 maxPageBytes = 4 * 1024 * 1024;        ///< Downloads read to look for links to datasets and companions
    static constexpr qint64 maxCompanionBytes = 1024 * 1024;
    static constexpr qint64 maxTokenBytes = 64 * 1024;

signals:
    /// Emitted once when the fetch wasn't canceled. The receiver owns the files of the result.
    void finished(const GeoZoneFeedFetch::Result &result);

private:
    using Handler = std::function<void(const GeoZoneDownloader::Result &download)>;

    /// Downloads with this fetch's tag, the handler isn't called once the fetch is canceled
    void _get(const QString &url, const QString &etag, const QString &lastModified, Handler handler,
              const GeoZoneDownloader::Options &options = GeoZoneDownloader::Options());
    /// @return Options of a further download: the headers and credentials of the service for its server, a GET
    GeoZoneDownloader::Options _optionsFor(const QString &url) const;
    void _requestToken();
    void _tokenReceived(const GeoZoneDownloader::Result &download);
    void _fetchFeed();
    void _feedReceived(const GeoZoneDownloader::Result &download);
    void _datasetReceived(const GeoZoneDownloader::Result &download);
    /// Continues with the further pages of the document, then its companions
    void _documentReceived(const GeoZoneDownloader::Result &download);
    /// @return false when the response isn't a WFS response the server limited
    bool _startPages();
    void _fetchNextPage();
    void _fetchCompanions();
    void _companionsReceived();
    void _finish(Result::Status status, const QString &errorString = QString());
    void _addMaxAge(qint64 maxAgeSecs);

    QPointer<GeoZoneDownloader> _downloader;
    Request _request;
    GeoZoneCancelToken _cancelToken;
    QString _tag;           ///< Of the downloads of this fetch
    Stage _stage = Stage::Idle;
    Result _result;
    QString _requestUrl;
    QString _accessToken;
    bool _tokenRequested = false;

    qint64 _pageSize = 0;
    int _pageCount = 0;     ///< After the first
    int _nextPage = 1;

    QJsonObject _document;  ///< Document whose companions are downloaded
    GeoZoneParser::Companions _companions;
    int _remainingCompanions = 0;
};
