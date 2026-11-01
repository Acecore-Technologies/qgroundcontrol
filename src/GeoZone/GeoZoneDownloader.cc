/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneDownloader.h"
#include "QGCFileDownload.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QLocale>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
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
    if (!_networkManager) {
        _networkManager = new QNetworkAccessManager(this);
    }

    QNetworkRequest request{ QUrl(url) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(_timeout);
    // Servers answer 304 Not Modified instead of sending unchanged data again
    if (!etag.isEmpty()) {
        request.setRawHeader("If-None-Match", etag.toUtf8());
    }
    if (!lastModified.isEmpty()) {
        request.setRawHeader("If-Modified-Since", lastModified.toUtf8());
    }

    QNetworkReply *const reply = _networkManager->get(request);
    QGCFileDownload::setIgnoreSSLErrorsIfNeeded(*reply);
    Download download;
    download.url = url;
    download.tag = tag;
    (void) QDir().mkpath(_directory);
    download.file = QSharedPointer<QFile>::create(QDir(_directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".part")));
    if (!download.file->open(QIODevice::WriteOnly)) {
        download.writeError = download.file->errorString();
    }
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
    const bool conditional = !etag.isEmpty() || !lastModified.isEmpty();
    (void) connect(reply, &QNetworkReply::finished, this, [this, reply, conditional, callback = std::move(callback)]() {
        reply->deleteLater();
        Download download = _downloads.take(reply);
        emit progressChanged();
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (download.writeError.isEmpty() && (reply->error() == QNetworkReply::NoError) && (statusCode >= 200) && (statusCode < 300)) {
            const QByteArray rest = reply->readAll();
            if (download.file->write(rest) != rest.size()) {
                download.writeError = download.file->errorString();
            }
        }
        download.file->close();

        Result result;
        if (download.aborted) {
            result.errorString = tr("The download was canceled.");
        } else if (!download.writeError.isEmpty()) {
            result.errorString = tr("The download could not be saved: %1").arg(download.writeError);
        } else if (download.tooLarge) {
            result.errorString = tr("The download is larger than %1.").arg(QLocale().formattedDataSize(_maxBytes));
        } else if ((reply->error() == QNetworkReply::SslHandshakeFailedError) && !download.sslErrors.isEmpty()) {
            result.errorString = tr("The secure connection to %1 failed: %2").arg(QUrl(download.url).host(), download.sslErrors.join(QStringLiteral("; ")));
        } else if (reply->error() == QNetworkReply::OperationCanceledError) {
            // Transfer timeouts cancel the reply
            result.errorString = tr("The server didn't respond within %n second(s).", nullptr, static_cast<int>(std::chrono::ceil<std::chrono::seconds>(_timeout).count()));
        } else if (reply->error() != QNetworkReply::NoError) {
            result.errorString = reply->errorString();
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
            }
        }
        if (result.status != Result::Status::Ok) {
            (void) download.file->remove();
        }
        callback(result);
    });
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
    const QString name = (_downloads.count() == 1) ? QUrl(_downloads.cbegin()->url).host() : tr("%1 feeds").arg(_downloads.count());
    return tr("Downloading %1: %2").arg(name, size);
}
