/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "GeoZoneDownloader.h"
#include "GeoZoneFeedCatalogue.h"

#include <QtCore/QDateTime>
#include <QtCore/QObject>
#include <QtCore/QPointer>

/// Keeps the feed catalogue of GeoZoneProviders up to date: the built-in catalogue with the catalogue downloaded from
/// the feed catalogue URL applied, then the catalogue file of the geozone data folder (GeoZoneFeedCatalogue::fileName).
/// The file is read again when it changes, the URL is downloaded again after the feed refresh interval like a feed,
/// with conditional requests, and its last valid copy is kept for offline starts. Catalogues which can't be read are
/// ignored with a warning, the built-in catalogue is always used.
class GeoZoneFeedCatalogueLoader : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString warning      READ warning    NOTIFY changed FINAL)   ///< Catalogues or entries ignored and why, one per line
    Q_PROPERTY(QString statusText   READ statusText NOTIFY changed FINAL)   ///< The catalogues in use
    Q_PROPERTY(QString filePath     READ filePath   CONSTANT FINAL)

public:
    /// @param directory Of the catalogue file and the copy of the downloaded catalogue
    /// @param downloader Downloads the catalogue URL
    GeoZoneFeedCatalogueLoader(const QString &directory, GeoZoneDownloader *downloader, QObject *parent = nullptr);
    /// Goes back to the built-in catalogue when this loader changed it
    ~GeoZoneFeedCatalogueLoader() override;

    /// Reads the catalogue file and the copy of the downloaded catalogue, applies them, then downloads the catalogue
    /// URL when due
    void init();
    /// Sets the URL of the catalogue to download, empty for none. A new URL is downloaded at once after init.
    void setUrl(const QString &url);
    QString url() const { return _url; }
    /// Reads the catalogue file again when it changed and downloads the catalogue URL when due
    /// @param force Downloads the catalogue URL now, also after a failure
    void check(bool force = false);
    /// Downloads the catalogue URL now when its last download failed because no server answered, for when the network
    /// is back
    void retryUnreachable();

    QString warning() const;
    QString statusText() const;
    QString filePath() const;
    /// @return When the catalogue URL was last downloaded or confirmed current, invalid when it never was
    QDateTime checkedAt() const { return _cache.checkedAt; }

    static constexpr const char *cacheFileName = "feeds-download.json";    ///< Copy of the downloaded catalogue
    static constexpr qint64 maxBytes = 1024 * 1024;

signals:
    /// The catalogue in use or the warnings changed
    void changed();

private:
    /// @return true when the file changed since it was last read, and reads it
    bool _readFile();
    void _readCache();
    void _writeCache();
    void _download();
    void _downloaded(const GeoZoneDownloader::Result &result);
    /// Reads the catalogue of a file URL, which is checked like the catalogue file
    void _readUrlFile();
    void _apply();

    QString _directory;
    QPointer<GeoZoneDownloader> _downloader;
    QString _url;
    QString _tag;

    QDateTime _fileModified;
    qint64 _fileSize = -1;
    GeoZoneFeedCatalogue::Catalogue _fileCatalogue;
    QString _fileError;

    struct Cache {
        QString url;
        QString etag;
        QString lastModified;
        QDateTime checkedAt;
        QByteArray data;
    };
    Cache _cache;
    GeoZoneFeedCatalogue::Catalogue _urlCatalogue;
    QString _urlError;          ///< Why the downloaded catalogue is ignored
    QString _downloadError;     ///< Why the last download failed
    bool _downloading = false;
    bool _unreachable = false;  ///< The last download failed because no server answered
    int _failures = 0;
    QDateTime _retryAt;
    bool _initialized = false;
    bool _applied = false;     ///< The catalogue of GeoZoneProviders was set
};
