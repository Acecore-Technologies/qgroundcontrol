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
#include <QtCore/QDataStream>
#include <QtCore/QString>

#include "GeoZoneParser.h"

/// Binary cache of the parse result of a stored geozone source, kept next to the stored copy as "<file>.gzc", so that
/// sources are restored at startup without parsing them again. Written by GeoZoneLoader when it parses a stored copy,
/// removed with it by GeoZoneStore.
///
/// The header holds a magic number, the cache format version, GeoZoneParser::version, the QDataStream version of the
/// result, the size, modification time and hash of the stored copy, and the language and format hint of the parse.
/// A cache is only used when all of them match, anything else, like a damaged or truncated cache, makes the source
/// parse again and rewrite its cache. The stored copy is recognized by its size and modification time, and only hashed
/// when its time changed, like when the directory was copied, so that it isn't read at all on most starts.
class GeoZoneParseCache
{
public:
    /// Version of the cache file and the serialization of the parse result, see the QDataStream operators of
    /// GeoZoneParser::Result, GeoZone::Data and their parts. Bump it with any change to them.
    static constexpr quint16 formatVersion = 3;
    /// Of the parse result, caches written by another version of Qt are parsed again
    static constexpr QDataStream::Version streamVersion = QDataStream::Qt_DefaultCompiledVersion;

    /// What the parse result depends on besides the bytes of the file
    struct Key {
        QString language;
        GeoZoneParser::Format formatHint = GeoZoneParser::Format::Unknown;
    };

    /// @return Path of the cache of the file
    static QString path(const QString &filePath);
    /// @return true if the file name is the name of a cache
    static bool isCacheFileName(const QString &fileName);
    /// @return Hash of the bytes of a file, as written in its cache
    static QByteArray hash(QByteArrayView bytes);

    /// Reads the cached parse result of the file, see the class description for when a cache is used
    /// @return false when there is no current cache, result is then empty
    static bool read(const QString &filePath, const Key &key, GeoZoneParser::Result &result);
    /// Writes the cache of the file, whose bytes have the size and hash
    /// @return false when the file has another size or the cache can't be written
    static bool write(const QString &filePath, qint64 fileSize, const QByteArray &fileHash, const Key &key, const GeoZoneParser::Result &result);
    static void remove(const QString &filePath);
};
