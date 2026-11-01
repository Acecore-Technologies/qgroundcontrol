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
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtPositioning/QGeoRectangle>

#include "GeoZoneCancelToken.h"
#include "GeoZoneDownloader.h"
#include "GeoZoneFeedFetch.h"
#include "GeoZoneParser.h"
#include "GeoZoneProvenance.h"
#include "GeoZoneSourceIndex.h"

class GeoZoneStore;

/// Reads and parses a geozone source and stores a copy of it with its parse cache, see GeoZoneParseCache. Jobs are
/// self-contained and run on worker threads.
class GeoZoneLoader
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneLoader)

public:
    enum class Request : quint8 {
        Automatic,  ///< Stored source or periodic refresh, failures are only shown on the source
        User,       ///< Refresh requested by the user, failures are reported
        Add,        ///< Source added by the user, failures are reported and maps move to the source
    };

    struct Job {
        QString key;                ///< File name, or the URL of a feed
        QString url;                ///< Empty for files
        QString sourcePath;         ///< File to parse
        bool fromStore = false;     ///< sourcePath is the stored copy of a source restored at startup, read from its parse cache when current
        bool downloaded = false;    ///< sourcePath is a download, which becomes the stored copy or is removed
        QString storedFileName;     ///< Name of the copy to write in the storage directory, empty to not copy
        QDateTime loadedAt;
        QDateTime fileDate;
        QDateTime checkedAt;        ///< When the server last confirmed a stored copy, loadedAt when invalid
        GeoZoneParser::Format formatHint = GeoZoneParser::Format::Unknown;
        QString language;           ///< Preferred language of localized texts
        QString etag;               ///< HTTP validators of a downloaded feed
        QString lastModified;
        QString datasetUrl;         ///< Dataset the feed's page linked to, which was downloaded
        qint64 maxAgeSecs = -1;     ///< How long the server said a downloaded feed stays current, negative when it didn't
        QStringList pagePaths;      ///< Further pages of a WFS response the server limited, merged into sourcePath and removed
        QString responseMember;     ///< Member of a downloaded JSON response holding the zones, see GeoZoneService::responseMember
        QGeoRectangle area;         ///< Area of interest the feed was downloaded for, invalid when it doesn't take one
        GeoZoneProvenance provenance;   ///< Of the data, completed with the hash of the stored copy
        Request request = Request::Automatic;
        GeoZoneCancelToken cancelToken;     ///< Canceled when the result is no longer wanted, run() then stops parsing or before storing
    };

    struct Result {
        GeoZoneParser::Result result;
        QString errorString;        ///< Why reading or parsing failed, empty on success
        bool stored = false;        ///< The copy was written
        QString storeError;
        bool cancelled = false;     ///< The job was canceled, nothing was stored
        bool fromCache = false;     ///< The result was read from the parse cache of the stored copy
        QByteArray contentHash;     ///< GeoZoneParseCache::hash of the content parsed, empty when read from the cache
        qint64 contentSize = -1;
    };

    /// @return Job loading a local file or file: URL, keyed by fileKey
    static Job fileJob(const QString &fileName);
    /// @return Key of the source of a file: its canonical path, so that files with the same name in different
    ///         folders are separate sources
    static QString fileKey(const QString &fileName);
    /// @return Job loading a feed, from downloaded data or, for file: URLs, from the file in place
    static Job feedJob(const QString &url);
    /// @return Job loading a feed downloaded by a fetch, with its validators, area and provenance
    static Job downloadJob(const GeoZoneFeedFetch::Request &request, const GeoZoneFeedFetch::Result &result);
    /// @return Job restoring the stored copy of a source of the index at sourcePath
    static Job storedJob(const GeoZoneSourceIndex::Entry &entry, const QString &sourcePath);
    /// @return Job loading a source of an offline pack, its copy extracted at filePath becoming the stored copy
    static Job importedJob(const GeoZoneSourceIndex::Entry &entry, const QString &filePath, const QString &packName, const QDateTime &importedAt);
    /// @return Index entry of the stored copy a job writes, updating the previous entry of the source
    static GeoZoneSourceIndex::Entry indexEntry(const Job &job, const GeoZoneSourceIndex::Entry &previous);


    /// @return The largest XML file of a zip archive, or the data itself when it isn't a zip archive. Empty when the
    ///         archive can't be extracted.
    static QByteArray extractZip(const QByteArray &data, QString &errorString);
    /// @return The largest XML or JSON file of a zip archive, empty when it can't be extracted
    static QByteArray extractZipFile(const QString &archivePath, QString &errorString);

    /// Parses the content of a source: a geozone document, or a zip package whose XML and JSON files are parsed
    /// together, the files of the format of the largest one which parses
    /// @param maxBytes Limit of the total size of the files of a zip package
    static bool parse(const QByteArray &content, GeoZoneParser::Result &result, QString &errorString, const GeoZoneParser::Options &options,
                      qint64 maxBytes = GeoZoneDownloader::defaultMaxBytes);
    static bool parse(const QByteArray &content, GeoZoneParser::Result &result, QString &errorString,
                      GeoZoneParser::Format formatHint = GeoZoneParser::Format::Unknown, const QString &language = QString(),
                      qint64 maxBytes = GeoZoneDownloader::defaultMaxBytes);

    static Result run(const Job &job, const GeoZoneStore &store);

private:
    /// Sets the dates, validators, area and provenance of a source of the index or a pack
    static void _setEntry(Job &job, const GeoZoneSourceIndex::Entry &entry);
};
