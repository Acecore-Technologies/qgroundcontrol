/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneDownloader.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneOrigin.h"
#include "GeoZoneSource.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QLocale>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#if QT_CONFIG(ssl)
#include <QtNetwork/QSslError>
#endif

GeoZoneDownloader::GeoZoneDownloader(QObject *parent)
    : QObject(parent)
    , _directory(QDir::tempPath())
{
}

void GeoZoneDownloader::abort(const QString &tag)
{
    QList<QNetworkReply*> replies;
    for (auto it = _downloads.begin(); it != _downloads.end(); ++it) {
        if (it->tag == tag) {
            it->aborted = true;
            replies.append(const_cast<QNetworkReply*>(qobject_cast<const QNetworkReply*>(it.key())));
        }
    }
    for (QNetworkReply *const reply : std::as_const(replies)) {
        reply->abort();
    }
}

QByteArray GeoZoneDownloader::readFile(const QString &filePath, qint64 maxBytes)
{
    QFile file(filePath);
    if ((file.size() > maxBytes) || !file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

void GeoZoneDownloader::get(const QString &url, const QString &etag, const QString &lastModified, Callback callback, const QString &tag)
{
    get(url, etag, lastModified, std::move(callback), tag, Options());
}

void GeoZoneDownloader::get(const QString &url, const QString &etag, const QString &lastModified, Callback callback, const QString &tag,
                            const Options &options)
{
    Download download;
    download.url = url;
    download.tag = tag;
    download.etag = etag;
    download.lastModified = lastModified;
    download.options = options;
    download.callback = std::move(callback);
    (void) QDir().mkpath(_directory);
    download.file = QSharedPointer<QFile>::create(QDir(_directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".part")));
    if (!download.file->open(QIODevice::WriteOnly)) {
        // Nothing is requested: without its file, the response would pile up in memory. Reported like the other
        // failures, once the caller is done.
        Result result;
        result.errorString = tr("The download could not be saved: %1").arg(download.file->errorString());
        result.finalUrl = redacted(url, options.secrets);
        (void) QMetaObject::invokeMethod(this, [callback = std::move(download.callback), result]() { callback(result); }, Qt::QueuedConnection);
        return;
    }
    download.hash = QSharedPointer<QCryptographicHash>::create(QCryptographicHash::Sha256);
    _send(download, QUrl(url));
}

QString GeoZoneDownloader::redacted(const QString &text, const QStringList &secrets)
{
    QString result = text;
    for (const QString &secret : secrets) {
        if (secret.isEmpty()) {
            continue;
        }
        (void) result.replace(secret, QStringLiteral("***"));
        (void) result.replace(QString::fromLatin1(QUrl::toPercentEncoding(secret)), QStringLiteral("***"));
    }
    return result;
}

void GeoZoneDownloader::_send(Download download, const QUrl &url)
{
    if (!_networkManager) {
        _networkManager = new QNetworkAccessManager(this);
    }

    // Credentials are for the server they were configured for, a redirect to another one doesn't get them
    const bool sameOrigin = GeoZoneOrigin::same(url, QUrl(download.url));
    QUrl requestUrl = url;
    if (sameOrigin && !download.options.authQuery.isEmpty()) {
        QUrlQuery query(requestUrl);
        for (const auto &[name, value] : std::as_const(download.options.authQuery)) {
            if (!query.hasQueryItem(name)) {
                query.addQueryItem(QString::fromLatin1(QUrl::toPercentEncoding(name)), QString::fromLatin1(QUrl::toPercentEncoding(value)));
            }
        }
        requestUrl.setQuery(query);
    }

    QNetworkRequest request{ requestUrl };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(_timeout);
    // Servers answer 304 Not Modified instead of sending unchanged data again
    if (!download.etag.isEmpty()) {
        request.setRawHeader("If-None-Match", download.etag.toUtf8());
    }
    if (!download.lastModified.isEmpty()) {
        request.setRawHeader("If-Modified-Since", download.lastModified.toUtf8());
    }
    for (const auto &[name, value] : std::as_const(download.options.headers)) {
        request.setRawHeader(name, value);
    }
    if (sameOrigin) {
        for (const auto &[name, value] : std::as_const(download.options.authHeaders)) {
            request.setRawHeader(name, value);
        }
    }
    if (download.options.post && !download.options.contentType.isEmpty()) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, download.options.contentType);
    }

    QNetworkReply *const reply = download.options.post ? _networkManager->post(request, download.options.body) : _networkManager->get(request);
    GeoZoneEnvironment::prepareDownload(*reply);
    _downloads.insert(reply, download);
    emit progressChanged();

    (void) connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        const auto it = _downloads.find(reply);
        if ((it == _downloads.end()) || !it->writeError.isEmpty() || it->tooLarge) {
            return;
        }
        // Redirect and error responses have bodies which aren't the feed
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if ((statusCode < 200) || (statusCode >= 300)) {
            (void) reply->readAll();
            return;
        }
        const QByteArray chunk = reply->readAll();
        it->hash->addData(chunk);
        if (it->file->write(chunk) != chunk.size()) {
            it->writeError = it->file->errorString();
            reply->abort();
        }
    });

#if QT_CONFIG(ssl)
    // The reply only reports that the handshake failed, these tell why
    (void) connect(reply, &QNetworkReply::sslErrors, this, [this, reply](const QList<QSslError> &errors) {
        const auto it = _downloads.find(reply);
        if (it != _downloads.end()) {
            for (const QSslError &error : errors) {
                it->sslErrors.append(error.errorString());
            }
        }
    });
#endif

    (void) connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64 total) {
        const auto it = _downloads.find(reply);
        if (it == _downloads.end()) {
            return;
        }
        it->received = received;
        it->total = total;
        // Untrusted servers could otherwise exhaust the memory
        if (!it->tooLarge && ((received > _maxBytes) || (total > _maxBytes))) {
            it->tooLarge = true;
            reply->abort();
            return;
        }
        emit progressChanged();
    });
    (void) connect(reply, &QNetworkReply::finished, this, [this, reply]() { _finished(reply); });
}

bool GeoZoneDownloader::_redirect(QNetworkReply *reply, Download &download, QString &errorString)
{
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QUrl target = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if ((statusCode < 300) || (statusCode >= 400) || (statusCode == 304) || !target.isValid() || download.aborted) {
        return false;
    }

    const QUrl from = reply->url();
    const QUrl next = from.resolved(target);
    if (download.redirects.count() >= maxRedirects) {
        errorString = tr("The server redirected more than %1 times.").arg(maxRedirects);
        return true;
    }
    if (((next.scheme() != QStringLiteral("http")) && (next.scheme() != QStringLiteral("https"))) ||
        ((from.scheme() == QStringLiteral("https")) && (next.scheme() == QStringLiteral("http")))) {
        errorString = tr("The server redirected to the less secure address %1.").arg(redacted(next.toString(), download.options.secrets));
        return true;
    }

    qCDebug(GeoZoneSourceLog) << "Redirected" << redacted(from.toString(), download.options.secrets) << "to" << redacted(next.toString(), download.options.secrets);
    download.redirects.append(redacted(from.toString(), download.options.secrets));
    // Like browsers, the request after a 303, or a POST redirected by a 301 or 302, is a GET
    if ((statusCode == 303) || (download.options.post && ((statusCode == 301) || (statusCode == 302)))) {
        download.options.post = false;
        download.options.body.clear();
    }
    download.received = 0;
    download.total = -1;
    download.hash->reset();
    // The redirect isn't followed without the file to write its response to
    if (!download.writeError.isEmpty() || !download.file->resize(0)) {
        errorString = tr("The download could not be saved: %1").arg(download.writeError.isEmpty() ? download.file->errorString() : download.writeError);
        return true;
    }
    _send(download, next);
    return true;
}

void GeoZoneDownloader::_finished(QNetworkReply *reply)
{
    reply->deleteLater();
    Download download = _downloads.take(reply);
    emit progressChanged();
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    QString redirectError;
    if ((reply->error() == QNetworkReply::NoError) && _redirect(reply, download, redirectError) && redirectError.isEmpty()) {
        return;
    }

    if (download.writeError.isEmpty() && (reply->error() == QNetworkReply::NoError) && (statusCode >= 200) && (statusCode < 300)) {
        const QByteArray rest = reply->readAll();
        download.hash->addData(rest);
        if (download.file->write(rest) != rest.size()) {
            download.writeError = download.file->errorString();
        }
    }
    download.file->close();

    const QStringList &secrets = download.options.secrets;
    Result result;
    result.statusCode = statusCode;
    result.finalUrl = redacted(reply->url().toString(), secrets);
    result.redirects = download.redirects;
    for (const auto &[name, value] : reply->rawHeaderPairs()) {
        result.headers.insert(name.toLower(), value);
    }
    const bool conditional = !download.etag.isEmpty() || !download.lastModified.isEmpty();
    if (download.aborted) {
        result.errorString = tr("The download was canceled.");
    } else if (!redirectError.isEmpty()) {
        result.errorString = redirectError;
    } else if (!download.writeError.isEmpty()) {
        result.errorString = tr("The download could not be saved: %1").arg(download.writeError);
    } else if (download.tooLarge) {
        result.errorString = tr("The download is larger than %1.").arg(QLocale().formattedDataSize(_maxBytes));
    } else if ((reply->error() == QNetworkReply::SslHandshakeFailedError) && !download.sslErrors.isEmpty()) {
        result.errorString = tr("The secure connection to %1 failed: %2").arg(reply->url().host(), download.sslErrors.join(QStringLiteral("; ")));
    } else if (reply->error() == QNetworkReply::OperationCanceledError) {
        // Transfer timeouts cancel the reply
        result.errorString = tr("The server didn't respond within %n second(s).", nullptr, static_cast<int>(std::chrono::ceil<std::chrono::seconds>(_timeout).count()));
    } else if (reply->error() != QNetworkReply::NoError) {
        result.errorString = redacted(reply->errorString(), secrets);
    } else if ((statusCode == 304) && conditional) {
        result.status = Result::Status::NotModified;
    } else if ((statusCode < 200) || (statusCode >= 300)) {
        result.errorString = tr("The server responded with status %1.").arg(statusCode);
    } else {
        if (download.file->size() <= 0) {
            result.errorString = tr("No data was received.");
        } else {
            result.status = Result::Status::Ok;
            result.filePath = download.file->fileName();
            result.etag = QString::fromUtf8(reply->rawHeader("ETag"));
            result.lastModified = QString::fromUtf8(reply->rawHeader("Last-Modified"));
            result.sha256 = download.hash->result();
        }
    }
    // Network and proxy errors have codes below 200, those of content, protocol and server errors come with a status
    const bool networkError = (reply->error() != QNetworkReply::NoError) && (static_cast<int>(reply->error()) < 200);
    result.unreachable = (result.status == Result::Status::Failed) && (statusCode == 0) && networkError && !download.aborted && redirectError.isEmpty() &&
                         download.writeError.isEmpty() && !download.tooLarge;
    if (result.status != Result::Status::Ok) {
        (void) download.file->remove();
    }
    download.callback(result);
}

double GeoZoneDownloader::progress() const
{
    qint64 received = 0;
    qint64 total = 0;
    for (const Download &download : _downloads) {
        if (download.total <= 0) {
            return -1;
        }
        received += download.received;
        total += download.total;
    }
    return (total > 0) ? (static_cast<double>(received) / total) : -1;
}

QString GeoZoneDownloader::progressText() const
{
    if (_downloads.isEmpty()) {
        return QString();
    }

    qint64 received = 0;
    qint64 total = 0;
    bool totalKnown = true;
    for (const Download &download : _downloads) {
        received += download.received;
        total += download.total;
        totalKnown = totalKnown && (download.total > 0);
    }
    const QLocale locale;
    const QString size = totalKnown ?
        tr("%1 of %2").arg(locale.formattedDataSize(received), locale.formattedDataSize(total)) :
        locale.formattedDataSize(received);
    const QString name = (_downloads.count() == 1) ? QUrl(_downloads.cbegin()->url).host() : tr("%n feeds", nullptr, static_cast<int>(_downloads.count()));
    return tr("Downloading %1: %2").arg(name, size);
}
