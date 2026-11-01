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
#include <QtCore/QDateTime>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>

/// Keeps copies of the geozone sources in a directory with an index of their origin, dates and HTTP validators.
/// Methods only access the files, so a copy of the store can be used from worker threads.
class GeoZoneStore
{
public:
    struct Entry {
        QString key;            ///< File name or URL of the source
        QString url;            ///< Feed URL, empty for files
        QString fileName;       ///< Stored copy in the directory
        QDateTime loadedAt;
        QDateTime fileDate;
        QString etag;
        QString lastModified;
    };

    enum class IndexStatus : quint8 {
        Ok,
        Missing,        ///< No index, entries has the single file kept by earlier versions if there is one
        Unreadable,     ///< The index exists but isn't valid
        Newer,          ///< Written by a newer version which may store sources differently
    };

    struct Index {
        IndexStatus status = IndexStatus::Missing;
        QList<Entry> entries;
        QString errorString;
    };

    explicit GeoZoneStore(const QString &dir);

    QString dir() const { return _dir; }
    QString path(const QString &fileName) const;
    static QString newFileName();

    /// @return Index entries in source order. Without an index, the single file kept by earlier versions is
    ///         returned and its name setting is removed.
    Index readIndex() const;
    /// Writes the index, removing it when there are no entries
    bool writeIndex(const QList<Entry> &entries, QString &errorString) const;
    /// Moves the index and the stored copies to a new subdirectory, keeping them for the user when the index is unreadable
    /// @return Path of the subdirectory, empty if nothing could be moved
    QString moveToBackup() const;

    bool writeFile(const QString &fileName, const QByteArray &bytes, QString &errorString) const;
    /// Copies a file into the directory as fileName
    bool copyFile(const QString &sourcePath, const QString &fileName, QString &errorString) const;
    /// Moves a file, like a finished download, into the directory as fileName
    bool adoptFile(const QString &sourcePath, const QString &fileName, QString &errorString) const;

    /// Directory in which downloads are written until they are adopted
    QString downloadDir() const;
    /// Removes downloads left by a previous session
    void clearDownloads() const;
    void removeFile(const QString &fileName) const;
    /// Removes stored copies which aren't referenced
    void removeOrphanFiles(const QSet<QString> &referencedFileNames) const;

    static constexpr const char *indexFileName = "sources.json";
    static constexpr const char *legacyFileName = "geozones.json";
    static constexpr const char *legacySettingsGroup = "GeoZoneManager";
    static constexpr const char *legacySourceNameKey = "sourceName";

private:
    QString _dir;
};
