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
#include <QtCore/QSet>
#include <QtCore/QString>

/// Keeps copies of the geozone sources in a directory, see GeoZoneSourceIndex for the index of their origin, dates
/// and HTTP validators. Methods only access the files, so a copy of the store can be used from worker threads.
class GeoZoneStore
{
public:
    explicit GeoZoneStore(const QString &dir);

    QString dir() const { return _dir; }
    QString path(const QString &fileName) const;
    static QString newFileName();

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

private:
    QString _dir;
};
