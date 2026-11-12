/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFeedFetch.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneFormat.h"
#include "GeoZoneOrigin.h"
#include "GeoZoneProviders.h"
#include "GeoZoneRefreshScheduler.h"
#include "GeoZoneSource.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QUrl>
#include <QtCore/QUuid>

#include <algorithm>
#include <cmath>

GeoZoneFeedFetch::GeoZoneFeedFetch(GeoZoneDownloader *downloader, const Request &request, const GeoZoneCancelToken &cancelToken, QObject *parent)
    : QObject(parent)
    , _downloader(downloader)
    , _request(request)
    , _cancelToken(cancelToken)
    , _tag(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

GeoZoneFeedFetch::~GeoZoneFeedFetch()
{
    if (_stage != Stage::Finished) {
        cancel();
    }
}

void GeoZoneFeedFetch::start()
{
    // A key typed into the address of a feed stays out of the log, see GeoZoneDownloader::redactedUrl
    qCDebug(GeoZoneSourceLog) << "Downloading" << _logUrl();
    const GeoZoneService &service = _request.service;
    _requestUrl = service.isEmpty() ? _request.url : service.requestUrl(_request.url, _request.area, _request.time);
    _result.requestUrl = _requestUrl;
    _accessToken = _request.accessToken;
    if (service.areaOfInterest && !_request.area.isValid()) {
        _finish(Result::Status::Failed, tr("The service downloads the zones around the mission, the vehicle or the map view. Plan a mission, connect a vehicle or show the area on the map."));
        return;
    }
    if (service.areaOfInterest && !service.acceptsArea(_request.area)) {
        _finish(Result::Status::Failed, tr("The area of the mission and the vehicle (%1 km²) is larger than the %2 km² the service answers.")
                                            .arg(std::ceil(GeoZoneService::areaKm2(_request.area))).arg(service.maxAreaKm2));
        return;
    }
    if (service.needsSecret() && _request.secret.isEmpty()) {
        _finish(Result::Status::Failed, tr("The credentials of the service are missing."));
        return;
    }
    if ((service.auth == GeoZoneService::Auth::OAuth2ClientCredentials) && _accessToken.isEmpty()) {
        _requestToken();
    } else {
        _fetchFeed();
    }
}

void GeoZoneFeedFetch::_requestToken()
{
    qCDebug(GeoZoneSourceLog) << "Requesting an access token from" << GeoZoneDownloader::redactedUrl(_request.service.tokenUrl);
    _stage = Stage::Token;
    _tokenRequested = true;
    _get(_request.service.tokenUrl, QString(), QString(), [this](const GeoZoneDownloader::Result &download) { _tokenReceived(download); },
         _request.service.tokenRequestOptions(_request.secret));
}

void GeoZoneFeedFetch::_tokenReceived(const GeoZoneDownloader::Result &download)
{
    const QByteArray data = (download.status == GeoZoneDownloader::Result::Status::Ok) ? GeoZoneDownloader::readFile(download.filePath, maxTokenBytes) : QByteArray();
    if (!download.filePath.isEmpty()) {
        (void) QFile::remove(download.filePath);
    }
    if (download.status != GeoZoneDownloader::Result::Status::Ok) {
        const QString reason = ((download.statusCode == 400) || (download.statusCode == 401)) ? tr("The client id or secret were refused.") : download.errorString;
        _finish(Result::Status::Failed, tr("The access token request failed. %1").arg(reason));
        return;
    }
    const QJsonObject token = QJsonDocument::fromJson(data).object();
    _accessToken = token.value(QStringLiteral("access_token")).toString();
    if (_accessToken.isEmpty()) {
        _finish(Result::Status::Failed, tr("The access token request failed. The response has no token."));
        return;
    }
    // Requested again a minute before it expires
    const qint64 expiresIn = token.value(QStringLiteral("expires_in")).toInteger(300);
    _result.accessToken = _accessToken;
    _result.tokenExpiresAt = GeoZoneEnvironment::currentDateTimeUtc().addSecs((std::max)(qint64{ 0 }, expiresIn - 60));
    _fetchFeed();
}

void GeoZoneFeedFetch::_fetchFeed()
{
    _stage = Stage::Feed;
    // The validators of a feed linking to its dataset are the dataset's, its page is read every time since the
    // link depends on the date, like the current AIRAC cycle
    const bool conditional = _request.datasetUrl.isEmpty();
    GeoZoneDownloader::Options options;
    if (!_request.service.isEmpty()) {
        options = _request.service.downloadOptions(_request.area, _request.time, _request.secret, _accessToken);
    }
    _get(_requestUrl, conditional ? _request.etag : QString(), conditional ? _request.lastModified : QString(),
         [this](const GeoZoneDownloader::Result &download) { _feedReceived(download); }, options);
}

GeoZoneDownloader::Options GeoZoneFeedFetch::_optionsFor(const QString &url) const
{
    if (_request.service.isEmpty()) {
        return GeoZoneDownloader::Options();
    }
    if (!GeoZoneOrigin::same(QUrl(url), QUrl(_requestUrl))) {
        return GeoZoneDownloader::Options();
    }
    GeoZoneDownloader::Options options = _request.service.downloadOptions(_request.area, _request.time, _request.secret, _accessToken);
    options.post = false;
    options.body.clear();
    options.contentType.clear();
    return options;
}

void GeoZoneFeedFetch::cancel()
{
    _cancelToken.cancel();
    if (_stage == Stage::Finished) {
        // The receiver owns the files
        return;
    }
    _stage = Stage::Finished;
    if (_downloader) {
        _downloader->abort(_tag);
    }
    for (const QString &path : std::as_const(_result.pagePaths) + QStringList{ _result.filePath }) {
        if (!path.isEmpty()) {
            (void) QFile::remove(path);
        }
    }
    _result = Result();
}

void GeoZoneFeedFetch::_get(const QString &url, const QString &etag, const QString &lastModified, Handler handler, const GeoZoneDownloader::Options &options)
{
    if (!_downloader) {
        _finish(Result::Status::Failed, tr("The download was canceled."));
        return;
    }
    _downloader->get(url, etag, lastModified, [fetch = QPointer<GeoZoneFeedFetch>(this), token = _cancelToken, handler = std::move(handler)](const GeoZoneDownloader::Result &download) {
        if (!fetch || token.isCancelled()) {
            if (!download.filePath.isEmpty()) {
                (void) QFile::remove(download.filePath);
            }
            return;
        }
        fetch->_lastUnreachable = download.unreachable;
        handler(download);
    }, _tag, options);
}

void GeoZoneFeedFetch::_feedReceived(const GeoZoneDownloader::Result &download)
{
    const qint64 maxAgeSecs = GeoZoneRefreshScheduler::maxAgeSecsOf(download.headers);
    if (download.status == GeoZoneDownloader::Result::Status::NotModified) {
        _addMaxAge(maxAgeSecs);
        _finish(Result::Status::NotModified);
        return;
    }
    if (download.status != GeoZoneDownloader::Result::Status::Ok) {
        // A token of an earlier fetch may have been revoked or expired early
        if ((download.statusCode == 401) && (_request.service.auth == GeoZoneService::Auth::OAuth2ClientCredentials) && !_tokenRequested) {
            _requestToken();
            return;
        }
        QString errorString = download.errorString;
        if (((download.statusCode == 401) || (download.statusCode == 403)) && _request.service.needsSecret()) {
            errorString = tr("The service refused the credentials (status %1).").arg(download.statusCode);
        } else if (download.statusCode == 413) {
            errorString = tr("The service refused the area as too large.");
        }
        _finish(Result::Status::Failed, errorString);
        return;
    }

    // The pages of the services linking to their current dataset, like the AIRAC cycles of NATS, are followed. Pages
    // are small, datasets aren't read here.
    QString linkedUrl;
    if (GeoZoneProviders::hasDatasetLink(_requestUrl)) {
        QString linkError;
        const QByteArray page = GeoZoneDownloader::readFile(download.filePath, GeoZoneProviders::maxLinkPageBytes);
        linkedUrl = GeoZoneProviders::datasetLink(_requestUrl, page, GeoZoneEnvironment::currentDateTimeUtc().date(), &linkError);
        if (!linkError.isEmpty()) {
            (void) QFile::remove(download.filePath);
            _finish(Result::Status::Failed, linkError);
            return;
        }
    }
    if (linkedUrl.isEmpty()) {
        _addMaxAge(maxAgeSecs);
        _documentReceived(download);
        return;
    }
    // The lifetime of a download page, often a minute for the pages of a web site, isn't that of the dataset it links

    qCDebug(GeoZoneSourceLog) << "Downloading dataset" << GeoZoneDownloader::redactedUrl(linkedUrl) << "linked from" << _logUrl();
    (void) QFile::remove(download.filePath);
    _stage = Stage::Dataset;
    _result.datasetUrl = linkedUrl;
    // The dataset already downloaded is confirmed by its validators
    const bool known = (_request.datasetUrl == linkedUrl);
    _get(linkedUrl, known ? _request.etag : QString(), known ? _request.lastModified : QString(),
         [this](const GeoZoneDownloader::Result &dataset) { _datasetReceived(dataset); }, _optionsFor(linkedUrl));
}

void GeoZoneFeedFetch::_datasetReceived(const GeoZoneDownloader::Result &download)
{
    _addMaxAge(GeoZoneRefreshScheduler::maxAgeSecsOf(download.headers));
    if (download.status == GeoZoneDownloader::Result::Status::NotModified) {
        _finish(Result::Status::NotModified);
    } else if (download.status != GeoZoneDownloader::Result::Status::Ok) {
        _finish(Result::Status::Failed, download.errorString);
    } else {
        _documentReceived(download);
    }
}

void GeoZoneFeedFetch::_documentReceived(const GeoZoneDownloader::Result &download)
{
    _result.filePath = download.filePath;
    _result.etag = download.etag;
    _result.lastModified = download.lastModified;
    _result.finalUrl = download.finalUrl;
    _result.redirects = download.redirects;
    _result.sha256 = download.sha256;

    // Servers may send the SHA-256 of the data in a digest header
    QString headerName;
    const QByteArray headerSha256 = GeoZoneProvenance::digestHeaderSha256(download.headers, headerName);
    if (!headerSha256.isEmpty()) {
        if (headerSha256 != download.sha256) {
            _result.integrity = GeoZoneProvenance::Integrity::Failed;
            _finish(Result::Status::Failed, tr("The integrity check failed: the SHA-256 of the data doesn't match its %1 header.").arg(headerName));
            return;
        }
        _result.integrity = GeoZoneProvenance::Integrity::Verified;
        _result.integrityText = tr("%1 header").arg(headerName);
    }

    if (_request.service.sha256Url.isEmpty()) {
        _fetchRest();
        return;
    }
    _stage = Stage::Integrity;
    QString sha256Url = _request.service.sha256Url;
    (void) sha256Url.replace(QStringLiteral("{url}"), _requestUrl);
    sha256Url = _request.service.requestUrl(sha256Url, _request.area, _request.time);
    _get(sha256Url, QString(), QString(), [this, sha256Url](const GeoZoneDownloader::Result &sha256) { _sha256Received(sha256Url, sha256); },
         _optionsFor(sha256Url));
}

void GeoZoneFeedFetch::_sha256Received(const QString &url, const GeoZoneDownloader::Result &download)
{
    const QByteArray content = (download.status == GeoZoneDownloader::Result::Status::Ok) ? GeoZoneDownloader::readFile(download.filePath, 4096) : QByteArray();
    if (!download.filePath.isEmpty()) {
        (void) QFile::remove(download.filePath);
    }
    // Data which can't be verified isn't loaded, like data which doesn't match
    _result.integrity = GeoZoneProvenance::Integrity::Failed;
    if (download.status != GeoZoneDownloader::Result::Status::Ok) {
        _finish(Result::Status::Failed, tr("The integrity check failed: the SHA-256 of the data could not be downloaded from %1. %2").arg(download.finalUrl, download.errorString));
        return;
    }
    const QByteArray expected = GeoZoneProvenance::parseSha256File(content);
    if (expected.isEmpty()) {
        _finish(Result::Status::Failed, tr("The integrity check failed: %1 has no SHA-256.").arg(download.finalUrl));
        return;
    }
    if (expected != _result.sha256) {
        _finish(Result::Status::Failed, tr("The integrity check failed: the SHA-256 of the data doesn't match %1.").arg(download.finalUrl));
        return;
    }
    qCDebug(GeoZoneSourceLog) << "SHA-256 verified" << GeoZoneDownloader::redactedUrl(url);
    _result.integrity = GeoZoneProvenance::Integrity::Verified;
    _result.integrityText = download.finalUrl;
    _fetchRest();
}

void GeoZoneFeedFetch::_fetchRest()
{
    if (!_startPages()) {
        _fetchCompanions();
    }
}

namespace {

/// The counts of paged responses are at their start or their end, which aren't parsed here
void readHeadAndTail(const QString &filePath, QByteArray &head, QByteArray &tail)
{
    constexpr qint64 countBytes = 4096;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    head = file.read(countBytes);
    (void) file.seek((std::max)(qint64{ 0 }, file.size() - countBytes));
    tail = file.read(countBytes);
}

} // namespace

bool GeoZoneFeedFetch::_startPages()
{
    QByteArray head;
    QByteArray tail;
    readHeadAndTail(_result.filePath, head, tail);
    _paging = GeoZoneProviders::paging(_requestUrl, head, tail);
    if ((_paging.scheme == GeoZoneProviders::Paging::Scheme::None) || (_paging.pageCount <= 0)) {
        return false;
    }

    _stage = Stage::Pages;
    _pagedBytes = QFileInfo(_result.filePath).size();
    // Numbered pages tell their count at once, the pages aren't downloaded for a response which stays incomplete
    if (_paging.truncated && (_paging.scheme == GeoZoneProviders::Paging::Scheme::PageNumber)) {
        _finishTruncated();
        return true;
    }
    qCDebug(GeoZoneSourceLog) << "Downloading up to" << _paging.pageCount << "more pages from" << _logUrl();
    _fetchNextPage();
    return true;
}

void GeoZoneFeedFetch::_finishTruncated()
{
    _finish(Result::Status::Failed, tr("The response has more than %1 pages, the zones of the further pages would be missing. Download a smaller area.")
                                        .arg(GeoZoneProviders::maxWfsPages + 1));
}

void GeoZoneFeedFetch::_fetchNextPage()
{
    if (_nextPage > _paging.pageCount) {
        // Only WFS responses tell how many zones are missing, which their source reports. The others would silently
        // lack the zones of the pages beyond the most downloaded.
        if (_paging.truncated && (_paging.scheme != GeoZoneProviders::Paging::Scheme::WfsStartIndex)) {
            _finishTruncated();
            return;
        }
        _fetchCompanions();
        return;
    }

    const QString pageUrl = GeoZoneProviders::pageUrl(_requestUrl, _paging, _nextPage);
    _get(pageUrl, QString(), QString(), [this](const GeoZoneDownloader::Result &page) {
        QByteArray head;
        QByteArray tail;
        if (page.status == GeoZoneDownloader::Result::Status::Ok) {
            readHeadAndTail(page.filePath, head, tail);
        }
        // ArcGIS services answer errors like their rate limit as JSON with status 200
        static const QRegularExpression errorRegExp(QStringLiteral("^\\s*\\{\\s*\"error\"\\s*:"));
        const bool failed = (page.status != GeoZoneDownloader::Result::Status::Ok) || errorRegExp.match(QString::fromLatin1(head)).hasMatch();
        if (!failed) {
            _result.pagePaths.append(page.filePath);
            // The pages are merged in memory to be loaded: together they are no larger than one download may be
            _pagedBytes += QFileInfo(page.filePath).size();
            if (const qint64 maxBytes = _downloader ? _downloader->maxBytes() : GeoZoneDownloader::defaultMaxBytes; _pagedBytes > maxBytes) {
                _finish(Result::Status::Failed, tr("The pages of the response are larger than %1 together. Download a smaller area.").arg(QLocale().formattedDataSize(maxBytes)));
                return;
            }
            // ArcGIS pages tell whether more follow, the last one downloaded whether the response is still incomplete
            if (_paging.scheme == GeoZoneProviders::Paging::Scheme::ArcGisOffset) {
                if (!GeoZoneProviders::arcGisExceeded(head, tail)) {
                    _paging.pageCount = _nextPage;
                } else if (_nextPage >= _paging.pageCount) {
                    _paging.truncated = true;
                }
            }
            _nextPage++;
            _fetchNextPage();
            return;
        }
        const QString error = (page.status == GeoZoneDownloader::Result::Status::Ok) ? QString::fromUtf8(head.left(300)) : page.errorString;
        qCWarning(GeoZoneSourceLog) << "Page download failed" << _logUrl() << error;
        if (page.status == GeoZoneDownloader::Result::Status::Ok) {
            (void) QFile::remove(page.filePath);
        }
        if (_paging.scheme != GeoZoneProviders::Paging::Scheme::WfsStartIndex) {
            // Only WFS responses tell how many zones are missing, the others would silently lack zones
            _finish(Result::Status::Failed, tr("Page %1 of the response could not be downloaded. %2").arg(_nextPage + 1).arg(error));
            return;
        }
        // The pages received are kept, the source shows how many zones the response still misses
        _nextPage = _paging.pageCount + 1;
        _fetchNextPage();
    }, _optionsFor(pageUrl));
}

void GeoZoneFeedFetch::_fetchCompanions()
{
    _stage = Stage::Companions;
    // Feeds with companions, like the FAA TFR list, are small enough to read here
    const QList<const GeoZoneFormat*> &formats = _request.formats.isEmpty() ? GeoZoneFormat::formats() : _request.formats;
    const QList<GeoZoneFormat::CompanionRequest> requests = GeoZoneFormat::companionRequestsOf(
        QUrl(_requestUrl), GeoZoneDownloader::readFile(_result.filePath, maxPageBytes), _request.formatHint, _document, formats);
    if (requests.isEmpty()) {
        _finish(Result::Status::Ready);
        return;
    }

    QList<GeoZoneFormat::CompanionRequest> missing;
    for (const GeoZoneFormat::CompanionRequest &companion : requests) {
        const auto it = _request.companions.constFind(companion.key);
        if ((it != _request.companions.cend()) && (it->version == companion.version)) {
            _companions.insert(companion.key, it.value());
        } else {
            missing.append(companion);
        }
    }
    if (missing.isEmpty()) {
        _companionsReceived();
        return;
    }

    qCDebug(GeoZoneSourceLog) << "Downloading" << missing.count() << "companion documents of" << _logUrl();
    _remainingCompanions = static_cast<int>(missing.count());
    for (const GeoZoneFormat::CompanionRequest &companion : std::as_const(missing)) {
        _get(companion.url, QString(), QString(), [this, companion](const GeoZoneDownloader::Result &download) {
            // A missing companion leaves its zones as conservative as without it
            if (download.status == GeoZoneDownloader::Result::Status::Ok) {
                const QByteArray data = GeoZoneDownloader::readFile(download.filePath, maxCompanionBytes);
                if (!data.isEmpty()) {
                    _companions.insert(companion.key, GeoZoneParser::Companion{ companion.version, data });
                }
            } else {
                qCWarning(GeoZoneSourceLog) << "Companion download failed" << GeoZoneDownloader::redactedUrl(companion.url) << download.errorString;
            }
            if (!download.filePath.isEmpty()) {
                (void) QFile::remove(download.filePath);
            }
            if (--_remainingCompanions == 0) {
                _companionsReceived();
            }
        }, _optionsFor(companion.url));
        // A download which can't start finishes the fetch at once
        if (_stage == Stage::Finished) {
            return;
        }
    }
}

void GeoZoneFeedFetch::_companionsReceived()
{
    // The document is stored with its companions, so they are available offline. It is written beside the download and
    // replaces it once complete: a short write must not pass for the document.
    const QByteArray composite = GeoZoneParser::composite(_document, _companions);
    QSaveFile file(_result.filePath);
    if (!file.open(QIODevice::WriteOnly) || (file.write(composite) != composite.size()) || !file.commit()) {
        _finish(Result::Status::Failed, tr("The download could not be saved: %1").arg(file.errorString()));
        return;
    }
    _finish(Result::Status::Ready);
}

void GeoZoneFeedFetch::_addMaxAge(qint64 maxAgeSecs)
{
    // A page linking to the dataset may change sooner than the dataset, and the other way around
    if ((maxAgeSecs > 0) && ((_result.maxAgeSecs <= 0) || (maxAgeSecs < _result.maxAgeSecs))) {
        _result.maxAgeSecs = maxAgeSecs;
    }
}

void GeoZoneFeedFetch::_finish(Result::Status status, const QString &errorString)
{
    if (status != Result::Status::Ready) {
        qCDebug(GeoZoneSourceLog) << "Feed" << ((status == Result::Status::NotModified) ? "not modified" : "failed") << _logUrl() << errorString;
        for (const QString &path : std::as_const(_result.pagePaths) + QStringList{ _result.filePath }) {
            if (!path.isEmpty()) {
                (void) QFile::remove(path);
            }
        }
        _result.filePath.clear();
        _result.pagePaths.clear();
    }
    _stage = Stage::Finished;
    _result.status = status;
    _result.errorString = errorString;
    _result.unreachable = (status == Result::Status::Failed) && _lastUnreachable;
    emit finished(_result);
}
