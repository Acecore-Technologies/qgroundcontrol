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
#include <QtCore/QCryptographicHash>
#include <QtCore/QHash>
#include <QtCore/QSharedPointer>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <chrono>
#include <functional>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

/// Downloads geozone feeds over http or https with conditional requests and reports the combined progress. Downloads
/// are written to files as they arrive, so large feeds aren't held in memory. Redirects are followed here rather than
/// by Qt, so that the URLs are recorded and credentials aren't sent to another server.
class GeoZoneDownloader : public QObject
{
    Q_OBJECT

public:
    using Header = std::pair<QByteArray, QByteArray>;

    /// How a request is sent besides its URL and validators
    struct Options {
        QList<Header> headers;      ///< Further request headers
        QList<Header> authHeaders;  ///< Credentials, only sent to the scheme, host and port of the URL, not after redirects elsewhere
        QList<std::pair<QString, QString>> authQuery;   ///< Credentials added to the query like authHeaders
        bool post = false;          ///< POST the body rather than GET
        QByteArray body;
        QByteArray contentType;
        QStringList secrets;        ///< Replaced by *** in the URLs and errors of the result
    };

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
        qint64 maxAgeSecs = -1; ///< How long the server said the data stays current, negative when it didn't
        int statusCode = 0;     ///< HTTP status of the last response, 0 without
        QString finalUrl;       ///< URL which answered after the redirects, without secrets
        QStringList redirects;  ///< URLs which redirected, in order, without secrets
        QByteArray sha256;      ///< SHA-256 of the body received when Ok
        QHash<QByteArray, QByteArray> headers;  ///< Response headers by lower case name
    };
    using Callback = std::function<void(const Result &result)>;

    static constexpr std::chrono::seconds defaultTimeout{ 120 };   ///< Without data received, a download fails
    static constexpr qint64 defaultMaxBytes = 100 * 1024 * 1024;   ///< Several times the largest national file
    static constexpr int maxRedirects = 10;

    explicit GeoZoneDownloader(QObject *parent = nullptr);

    /// Starts a download. The callback is called once, unless the downloader is destroyed first.
    /// @param etag, lastModified Validators of the previous download, empty to download unconditionally
    /// @param tag Identifies the downloads abort() cancels
    void get(const QString &url, const QString &etag, const QString &lastModified, Callback callback, const QString &tag = QString());
    void get(const QString &url, const QString &etag, const QString &lastModified, Callback callback, const QString &tag, const Options &options);

    /// @return The text with the secrets, also percent-encoded, replaced by ***
    static QString redacted(const QString &text, const QStringList &secrets);

    /// Cancels the downloads started with the tag. Their callbacks report a failure.
    void abort(const QString &tag);

    /// Directory in which downloads are written, the temporary directory by default
    QString directory() const { return _directory; }
    void setDirectory(const QString &directory) { _directory = directory; }

    /// @return Seconds the response stays current from its headers: Cache-Control max-age, or Expires after Date,
    ///         less its Age. Negative when the response doesn't tell, or tells to revalidate it at once.
    static qint64 maxAgeSecs(const QByteArray &cacheControl, const QByteArray &expires, const QByteArray &date, const QByteArray &age);

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
        QString etag;
        QString lastModified;
        Options options;
        Callback callback;
        QStringList redirects;      ///< Without secrets
        QSharedPointer<QFile> file;
        QSharedPointer<QCryptographicHash> hash;
        qint64 received = 0;
        qint64 total = -1;      ///< Negative when the size is unknown
        bool tooLarge = false;
        bool aborted = false;
        QString writeError;     ///< The file couldn't be written
        QStringList sslErrors;  ///< Why the server's certificate wasn't trusted
    };

    /// Sends the request of the download to the URL, the first or the target of a redirect
    void _send(Download download, const QUrl &url);
    void _finished(QNetworkReply *reply);
    /// Follows the redirect the reply answered with
    /// @return false when the reply isn't a redirect
    bool _redirect(QNetworkReply *reply, Download &download, QString &errorString);

    QNetworkAccessManager *_networkManager = nullptr;
    QHash<const QObject*, Download> _downloads;  ///< Keyed by reply
    qint64 _maxBytes = defaultMaxBytes;
    std::chrono::milliseconds _timeout = defaultTimeout;
    QString _directory;
};
