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
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>

class QIODevice;

/// Reads the files of a zip archive, like the dataset of a feed, and writes archives, like offline packs. Sizes are
/// limited while inflating, whatever the archive declares, so a small archive can't expand to fill the memory.
namespace GeoZoneZip {

constexpr int maxEntries = 1000;

/// @return The largest file of the archive with one of the suffixes, inflated, empty on errors
/// @param maxBytes Larger files, or archives declaring them, are rejected
QByteArray largestFile(const QByteArray &archive, const QStringList &suffixes, qint64 maxBytes, QString &errorString);

struct File {
    QString name;
    QByteArray data;
};
/// @return The files of the archive with one of the suffixes, inflated, the largest first, none on errors
/// @param maxBytes Limit of the total size of the files
QList<File> files(const QByteArray &archive, const QStringList &suffixes, qint64 maxBytes, QString &errorString);

/// Inflates the files of the archive with one of the suffixes one at a time, the largest first, so that only one of
/// them is in memory. The archive is checked before the first file is inflated, like files() does.
/// @param maxBytes Limit of the total size of the files
/// @param callback Called with each file, returns false to stop
/// @return false on errors, true when the files were read or the callback stopped
bool forEachFile(const QByteArray &archive, const QStringList &suffixes, qint64 maxBytes, QString &errorString,
                 const std::function<bool(const File &file)> &callback);

/// Writes a zip archive, the files deflated unless that doesn't make them smaller. The archive is limited to 4 GiB,
/// the range of its 32 bit sizes and offsets: add() and finish() fail rather than write one which can't be read.
class Writer
{
public:
    explicit Writer(QIODevice *device);

    /// @return false, with errorString(), when the file can't be written or the archive would get too large with it
    bool add(const QString &name, const QByteArray &data);
    /// Writes the central directory
    /// @return false, with errorString(), when it can't be written or its offset or size don't fit the archive
    bool finish();
    QString errorString() const { return _errorString; }

private:
    bool _write(const QByteArray &bytes);
    /// @return true if the size or offset can be written in a 32 bit field
    bool _fits(qint64 size) const;

    QIODevice *_device = nullptr;
    QByteArray _directory;      ///< Central directory of the files added
    int _count = 0;
    quint16 _dosTime = 0;
    quint16 _dosDate = 0;
    QString _errorString;
};

} // namespace GeoZoneZip
