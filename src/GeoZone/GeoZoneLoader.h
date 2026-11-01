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

#include "GeoZoneParser.h"

class GeoZoneStore;

/// Reads and parses a geozone source and stores a copy of it. Jobs are self-contained and run on worker threads.
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
        QString sourcePath;         ///< File to parse when there is no data
        QByteArray data;            ///< Downloaded content to parse
        QString storedFileName;     ///< Name of the copy to write in the storage directory, empty to not copy
        QDateTime loadedAt;
        QDateTime fileDate;
        GeoZoneParser::Format formatHint = GeoZoneParser::Format::Unknown;
        QString language;           ///< Preferred language of localized texts
        QString etag;               ///< HTTP validators of a downloaded feed
        QString lastModified;
        Request request = Request::Automatic;
        int generation = 0;
    };

    struct Result {
        GeoZoneParser::Result result;
        QString errorString;        ///< Why reading or parsing failed, empty on success
        bool stored = false;        ///< The copy was written
        QString storeError;
    };

    /// @return Job loading a local file or file: URL, keyed by its file name
    static Job fileJob(const QString &fileName);
    /// @return Job loading a feed, from downloaded data or, for file: URLs, from the file in place
    static Job feedJob(const QString &url);
    /// @return The format to expect from a feed URL
    static GeoZoneParser::Format formatHint(const QString &url);

    /// @return URL of the current dataset linked from a download page, like the AIRAC datasets of NATS, or an empty
    ///         string when the data isn't such a page. The latest dataset effective today or earlier is chosen.
    static QString datasetLink(const QString &url, const QByteArray &data, const QDate &today);

    /// @return The largest XML file of a zip archive, or the data itself when it isn't a zip archive. Empty when the
    ///         archive can't be extracted.
    static QByteArray extractZip(const QByteArray &data, QString &errorString);

    static Result run(const Job &job, const GeoZoneStore &store);
};
