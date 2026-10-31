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

#include <QtCore/QLocale>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

GeoZoneDownloader::GeoZoneDownloader(QObject *parent)
    : QObject(parent)
{
}

void GeoZoneDownloader::get(const QString &url, const QString &etag, const QString &lastModified, Callback callback)
{
    if (!_networkManager) {
        _networkManager = new QNetworkAccessManager(this);
    }

    QNetworkRequest request{ QUrl(url) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(timeoutSecs * 1000);
    // Servers answer 304 Not Modified instead of sending unchanged data again
    if (!etag.isEmpty()) {
        request.setRawHeader("If-None-Match", etag.toUtf8());
    }
    if (!lastModified.isEmpty()) {
        request.setRawHeader("If-Modified-Since", lastModified.toUtf8());
    }

    QNetworkReply *const reply = _networkManager->get(request);
    QGCFileDownload::setIgnoreSSLErrorsIfNeeded(*reply);
    _downloads.insert(reply, Download{ url });
    emit progressChanged();

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
        const bool tooLarge = _downloads.value(reply).tooLarge;
        (void) _downloads.remove(reply);
        emit progressChanged();

        Result result;
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (tooLarge) {
            result.errorString = tr("The download is larger than %1.").arg(QLocale().formattedDataSize(_maxBytes));
        } else if (reply->error() != QNetworkReply::NoError) {
            result.errorString = reply->errorString();
        } else if ((statusCode == 304) && conditional) {
            result.status = Result::Status::NotModified;
        } else if ((statusCode < 200) || (statusCode >= 300)) {
            result.errorString = tr("The server responded with status %1.").arg(statusCode);
        } else {
            result.data = reply->readAll();
            if (result.data.isEmpty()) {
                result.errorString = tr("No data was received.");
            } else {
                result.status = Result::Status::Ok;
                result.etag = QString::fromUtf8(reply->rawHeader("ETag"));
                result.lastModified = QString::fromUtf8(reply->rawHeader("Last-Modified"));
            }
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
