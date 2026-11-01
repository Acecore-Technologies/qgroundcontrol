/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneParseCache.h"
#include "GeoZoneSource.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QtEndian>

#include <zlib.h>

#include <algorithm>
#include <limits>

namespace {

constexpr QLatin1StringView kSuffix(".gzc");
constexpr quint32 kMagic = 0x475A5043;  // "GZPC"
// The header layout is fixed, its fields are read before the version of the payload is known
constexpr QDataStream::Version kHeaderStreamVersion = QDataStream::Qt_6_0;
constexpr qint64 kModifiedOffset = 20;  ///< Of the modification time of the file, after the magic, versions and size

quint32 checksum(QByteArrayView bytes)
{
    uLong crc = crc32(0L, Z_NULL, 0);
    for (qsizetype offset = 0; offset < bytes.size(); offset += std::numeric_limits<uInt>::max()) {
        const qsizetype length = (std::min)(bytes.size() - offset, static_cast<qsizetype>(std::numeric_limits<uInt>::max()));
        crc = crc32(crc, reinterpret_cast<const Bytef*>(bytes.data() + offset), static_cast<uInt>(length));
    }
    return static_cast<quint32>(crc);
}

/// @return Hash of the bytes of the file, empty when it can't be read
QByteArray hashFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    uchar *const mapped = (file.size() > 0) ? file.map(0, file.size()) : nullptr;
    if (!mapped) {
        return GeoZoneParseCache::hash(file.readAll());
    }
    const QByteArray hash = GeoZoneParseCache::hash(QByteArrayView(reinterpret_cast<const char*>(mapped), file.size()));
    (void) file.unmap(mapped);
    return hash;
}

} // namespace

QString GeoZoneParseCache::path(const QString &filePath)
{
    return filePath + kSuffix;
}

bool GeoZoneParseCache::isCacheFileName(const QString &fileName)
{
    return fileName.endsWith(kSuffix);
}

QByteArray GeoZoneParseCache::hash(QByteArrayView bytes)
{
    // Several times faster than SHA-256
    return QCryptographicHash::hash(bytes, QCryptographicHash::Blake2b_256);
}

bool GeoZoneParseCache::read(const QString &filePath, const Key &key, GeoZoneParser::Result &result)
{
    result = GeoZoneParser::Result();
    QFile cache(path(filePath));
    const QFileInfo fileInfo(filePath);
    if (!cache.exists() || !fileInfo.exists() || !cache.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto unused = [&filePath](const char *reason) {
        qCDebug(GeoZoneSourceLog) << "Parse cache of" << filePath << "not used:" << reason;
        return false;
    };

    // Read in place like the stored copies
    const qint64 cacheSize = cache.size();
    uchar *mapped = (cacheSize > 0) ? cache.map(0, cacheSize) : nullptr;
    const auto unmap = [&cache, &mapped]() {
        if (mapped) {
            (void) cache.unmap(mapped);
            mapped = nullptr;
        }
    };
    const auto unmapOnReturn = qScopeGuard(unmap);
    const QByteArray bytes = mapped ? QByteArray::fromRawData(reinterpret_cast<const char*>(mapped), cacheSize) : cache.readAll();

    QDataStream header(bytes);
    header.setVersion(kHeaderStreamVersion);
    quint32 magic = 0;
    quint16 cacheVersion = 0;
    quint16 parserVersion = 0;
    qint32 payloadStreamVersion = 0;
    qint64 fileSize = -1;
    qint64 fileModified = 0;
    QByteArray fileHash;
    QString language;
    quint8 formatHint = 0;
    quint32 payloadChecksum = 0;
    qint64 payloadSize = -1;
    header >> magic >> cacheVersion >> parserVersion >> payloadStreamVersion >> fileSize >> fileModified >> fileHash >> language >> formatHint
           >> payloadChecksum >> payloadSize;
    if ((header.status() != QDataStream::Ok) || (magic != kMagic)) {
        return unused("damaged");
    }
    if ((cacheVersion != formatVersion) || (parserVersion != GeoZoneParser::version) || (payloadStreamVersion != streamVersion)) {
        return unused("other version");
    }
    if ((language != key.language) || (formatHint != static_cast<quint8>(key.formatHint))) {
        return unused("other language or format hint");
    }
    if (fileSize != fileInfo.size()) {
        return unused("file changed");
    }
    const qint64 payloadOffset = header.device()->pos();
    if (payloadSize != (bytes.size() - payloadOffset)) {
        return unused("truncated");
    }
    const QByteArray payload = QByteArray::fromRawData(bytes.constData() + payloadOffset, payloadSize);
    if (checksum(payload) != payloadChecksum) {
        return unused("damaged");
    }

    // Copying the directory changes the time but not the bytes
    const qint64 modified = fileInfo.lastModified().toMSecsSinceEpoch();
    if ((modified != fileModified) && (hashFile(filePath) != fileHash)) {
        return unused("file changed");
    }

    QDataStream stream(payload);
    stream.setVersion(payloadStreamVersion);
    stream >> result;
    if ((stream.status() != QDataStream::Ok) || !stream.atEnd()) {
        result = GeoZoneParser::Result();
        return unused("damaged");
    }

    if (modified != fileModified) {
        // The next start doesn't hash the file again
        unmap();
        cache.close();
        if (cache.open(QIODevice::ReadWrite) && cache.seek(kModifiedOffset)) {
            char time[sizeof(qint64)];
            qToBigEndian<qint64>(modified, time);
            (void) cache.write(time, sizeof(time));
        }
    }
    return true;
}

bool GeoZoneParseCache::write(const QString &filePath, qint64 fileSize, const QByteArray &fileHash, const Key &key,
                              const GeoZoneParser::Result &result)
{
    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || (fileInfo.size() != fileSize)) {
        return false;
    }

    QByteArray payload;
    {
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream.setVersion(streamVersion);
        stream << result;
        if (stream.status() != QDataStream::Ok) {
            return false;
        }
    }

    // Written under a temporary name first, an interrupted write leaves no cache
    QSaveFile cache(path(filePath));
    if (!cache.open(QIODevice::WriteOnly)) {
        qCWarning(GeoZoneSourceLog) << "Unable to write the parse cache of" << filePath << cache.errorString();
        return false;
    }
    QDataStream header(&cache);
    header.setVersion(kHeaderStreamVersion);
    header << kMagic << formatVersion << GeoZoneParser::version << static_cast<qint32>(streamVersion) << fileSize
           << fileInfo.lastModified().toMSecsSinceEpoch();
    Q_ASSERT(cache.pos() == (kModifiedOffset + static_cast<qint64>(sizeof(qint64))));
    header << fileHash << key.language << static_cast<quint8>(key.formatHint) << checksum(payload) << static_cast<qint64>(payload.size());
    if ((header.status() != QDataStream::Ok) || (cache.write(payload) != payload.size()) || !cache.commit()) {
        qCWarning(GeoZoneSourceLog) << "Unable to write the parse cache of" << filePath << cache.errorString();
        return false;
    }
    return true;
}

void GeoZoneParseCache::remove(const QString &filePath)
{
    (void) QFile::remove(path(filePath));
}
