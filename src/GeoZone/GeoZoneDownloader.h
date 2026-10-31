/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <functional>

class QNetworkAccessManager;

/// Downloads geozone feeds over http or https with conditional requests and reports the combined progress
class GeoZoneDownloader : public QObject
{
    Q_OBJECT

public:
    struct Result {
        enum class Status {
            Ok,
            NotModified,    ///< The server confirmed the validators of the previous download
            Failed,
        };
        Status status = Status::Failed;
        QByteArray data;
        QString etag;
        QString lastModified;
        QString errorString;
    };
    using Callback = std::function<void(const Result &result)>;

    static constexpr int timeoutSecs = 120;
    static constexpr qint64 defaultMaxBytes = 100 * 1024 * 1024;   ///< Several times the largest national file

    explicit GeoZoneDownloader(QObject *parent = nullptr);

    /// Starts a download. The callback is called once, unless the downloader is destroyed first.
    /// @param etag, lastModified Validators of the previous download, empty to download unconditionally
    void get(const QString &url, const QString &etag, const QString &lastModified, Callback callback);

    /// Downloads larger than this fail without being read
    void setMaxBytes(qint64 maxBytes) { _maxBytes = maxBytes; }

    int count() const { return static_cast<int>(_downloads.count()); }
    /// @return Fraction of the downloads received, negative when a size is unknown
    double progress() const;
    /// @return Description of the downloads in progress, empty when there are none
    QString progressText() const;

signals:
    void progressChanged();

private:
    struct Download {
        QString url;
        qint64 received = 0;
        qint64 total = -1;      ///< Negative when the size is unknown
        bool tooLarge = false;
    };

    QNetworkAccessManager *_networkManager = nullptr;
    QHash<const QObject*, Download> _downloads;  ///< Keyed by reply
    qint64 _maxBytes = defaultMaxBytes;
};
