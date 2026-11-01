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
#include <QtCore/QSharedPointer>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <chrono>
#include <functional>

class QFile;
class QNetworkAccessManager;

/// Downloads geozone feeds over http or https with conditional requests and reports the combined progress. Downloads
/// are written to files as they arrive, so large feeds aren't held in memory.
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
        QString filePath;       ///< Downloaded file in directory(), which the receiver adopts or removes. Empty unless Ok.
        QString etag;
        QString lastModified;
        QString errorString;
    };
    using Callback = std::function<void(const Result &result)>;

    static constexpr std::chrono::seconds defaultTimeout{ 120 };   ///< Without data received, a download fails
    static constexpr qint64 defaultMaxBytes = 100 * 1024 * 1024;   ///< Several times the largest national file

    explicit GeoZoneDownloader(QObject *parent = nullptr);

    /// Starts a download. The callback is called once, unless the downloader is destroyed first.
    /// @param etag, lastModified Validators of the previous download, empty to download unconditionally
    /// @param tag Identifies the downloads abort() cancels
    void get(const QString &url, const QString &etag, const QString &lastModified, Callback callback, const QString &tag = QString());

    /// Cancels the downloads started with the tag. Their callbacks report a failure.
    void abort(const QString &tag);

    /// Directory in which downloads are written, the temporary directory by default
    QString directory() const { return _directory; }
    void setDirectory(const QString &directory) { _directory = directory; }

    /// @return Content of a downloaded file, empty when it is larger than maxBytes or can't be read
    static QByteArray readFile(const QString &filePath, qint64 maxBytes);

    /// Downloads larger than this fail without being read
    void setMaxBytes(qint64 maxBytes) { _maxBytes = maxBytes; }
    void setTimeout(std::chrono::milliseconds timeout) { _timeout = timeout; }

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
        QString tag;
        QSharedPointer<QFile> file;
        qint64 received = 0;
        qint64 total = -1;      ///< Negative when the size is unknown
        bool tooLarge = false;
        bool aborted = false;
        QString writeError;     ///< The file couldn't be written
        QStringList sslErrors;  ///< Why the server's certificate wasn't trusted
    };

    QNetworkAccessManager *_networkManager = nullptr;
    QHash<const QObject*, Download> _downloads;  ///< Keyed by reply
    qint64 _maxBytes = defaultMaxBytes;
    std::chrono::milliseconds _timeout = defaultTimeout;
    QString _directory;
};
