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
#include "GeoZoneProviders.h"
#include "GeoZoneSource.h"

#include <QtCore/QFile>
#include <QtCore/QUrl>
#include <QtCore/QUuid>

#include <algorithm>

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
    qCDebug(GeoZoneSourceLog) << "Downloading" << _request.url;
    _stage = Stage::Feed;
    // The validators of a feed linking to its dataset are the dataset's, its page is read every time since the
    // link depends on the date, like the current AIRAC cycle
    const bool conditional = _request.datasetUrl.isEmpty();
    _get(_request.url, conditional ? _request.etag : QString(), conditional ? _request.lastModified : QString(),
         [this](const GeoZoneDownloader::Result &download) { _feedReceived(download); });
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

void GeoZoneFeedFetch::_get(const QString &url, const QString &etag, const QString &lastModified, Handler handler)
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
        handler(download);
    }, _tag);
}

void GeoZoneFeedFetch::_feedReceived(const GeoZoneDownloader::Result &download)
{
    _addMaxAge(download.maxAgeSecs);
    if (download.status == GeoZoneDownloader::Result::Status::NotModified) {
        _finish(Result::Status::NotModified);
        return;
    }
    if (download.status != GeoZoneDownloader::Result::Status::Ok) {
        _finish(Result::Status::Failed, download.errorString);
        return;
    }

    // Pages linking to the current dataset, like the AIRAC cycles of NATS, are followed. Pages are small, datasets aren't read here.
    const QByteArray page = GeoZoneDownloader::readFile(download.filePath, maxPageBytes);
    const QString linkedUrl = GeoZoneProviders::datasetLink(_request.url, page, GeoZoneEnvironment::currentDateTimeUtc().date());
    if (linkedUrl.isEmpty()) {
        _documentReceived(download);
        return;
    }

    qCDebug(GeoZoneSourceLog) << "Downloading dataset" << linkedUrl << "linked from" << _request.url;
    (void) QFile::remove(download.filePath);
    _stage = Stage::Dataset;
    _result.datasetUrl = linkedUrl;
    // The dataset already downloaded is confirmed by its validators
    const bool known = (_request.datasetUrl == linkedUrl);
    _get(linkedUrl, known ? _request.etag : QString(), known ? _request.lastModified : QString(),
         [this](const GeoZoneDownloader::Result &dataset) { _datasetReceived(dataset); });
}

void GeoZoneFeedFetch::_datasetReceived(const GeoZoneDownloader::Result &download)
{
    _addMaxAge(download.maxAgeSecs);
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
    if (!_startPages()) {
        _fetchCompanions();
    }
}

bool GeoZoneFeedFetch::_startPages()
{
    // The counts are at the start or the end of the response, which isn't parsed here
    constexpr qint64 countBytes = 4096;
    QFile file(_result.filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray head = file.read(countBytes);
    (void) file.seek((std::max)(qint64{ 0 }, file.size() - countBytes));
    const QByteArray tail = file.read(countBytes);
    file.close();
    const GeoZoneProviders::WfsCounts counts = GeoZoneProviders::wfsCounts(head, tail);
    if ((counts.returned <= 0) || (counts.matched <= counts.returned) || GeoZoneProviders::wfsPageUrl(_request.url, counts.returned).isEmpty()) {
        return false;
    }

    _stage = Stage::Pages;
    _pageSize = counts.returned;
    _pageCount = static_cast<int>((std::min)((counts.matched - 1) / counts.returned, qint64{ GeoZoneProviders::maxWfsPages }));
    qCDebug(GeoZoneSourceLog) << "Downloading" << _pageCount << "more pages of" << counts.matched << "features from" << _request.url;
    _fetchNextPage();
    return true;
}

void GeoZoneFeedFetch::_fetchNextPage()
{
    if (_nextPage > _pageCount) {
        _fetchCompanions();
        return;
    }

    const QString pageUrl = GeoZoneProviders::wfsPageUrl(_request.url, _pageSize * _nextPage);
    _get(pageUrl, QString(), QString(), [this](const GeoZoneDownloader::Result &page) {
        if (page.status == GeoZoneDownloader::Result::Status::Ok) {
            _result.pagePaths.append(page.filePath);
            _nextPage++;
        } else {
            // The pages received are kept, the source shows how many zones the response still misses
            qCWarning(GeoZoneSourceLog) << "Page download failed" << _request.url << page.errorString;
            _nextPage = _pageCount + 1;
        }
        _fetchNextPage();
    });
}

void GeoZoneFeedFetch::_fetchCompanions()
{
    _stage = Stage::Companions;
    // Feeds with companions, like the FAA TFR list, are small enough to read here
    const QList<const GeoZoneFormat*> &formats = _request.formats.isEmpty() ? GeoZoneFormat::formats() : _request.formats;
    const QList<GeoZoneFormat::CompanionRequest> requests = GeoZoneFormat::companionRequestsOf(
        QUrl(_request.url), GeoZoneDownloader::readFile(_result.filePath, maxPageBytes), _request.formatHint, _document, formats);
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

    qCDebug(GeoZoneSourceLog) << "Downloading" << missing.count() << "companion documents of" << _request.url;
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
                qCWarning(GeoZoneSourceLog) << "Companion download failed" << companion.url << download.errorString;
            }
            if (!download.filePath.isEmpty()) {
                (void) QFile::remove(download.filePath);
            }
            if (--_remainingCompanions == 0) {
                _companionsReceived();
            }
        });
    }
}

void GeoZoneFeedFetch::_companionsReceived()
{
    // The document is stored with its companions, so they are available offline
    QFile file(_result.filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        (void) file.write(GeoZoneParser::composite(_document, _companions));
        file.close();
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
        qCDebug(GeoZoneSourceLog) << "Feed" << ((status == Result::Status::NotModified) ? "not modified" : "failed") << _request.url << errorString;
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
    emit finished(_result);
}
