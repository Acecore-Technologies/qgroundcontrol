/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePack.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneParseCache.h"
#include "GeoZoneZip.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QScopeGuard>
#include <QtCore/QtEndian>
#include <QtCore/QUuid>

#include <zlib.h>

#include <algorithm>

namespace GeoZonePack {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("GeoZonePack", text);
}

void appendU16(QByteArray &bytes, quint16 value)
{
    char buffer[2];
    qToLittleEndian(value, buffer);
    bytes.append(buffer, 2);
}

void appendU32(QByteArray &bytes, quint32 value)
{
    char buffer[4];
    qToLittleEndian(value, buffer);
    bytes.append(buffer, 4);
}

/// @return The data deflated without zlib header, as zip archives store it, empty on errors
QByteArray deflateRaw(const QByteArray &data)
{
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return QByteArray();
    }
    QByteArray output(static_cast<qsizetype>(deflateBound(&stream, static_cast<uLong>(data.size()))), Qt::Uninitialized);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    const int result = deflate(&stream, Z_FINISH);
    output.resize(static_cast<qsizetype>(stream.total_out));
    deflateEnd(&stream);
    return (result == Z_STREAM_END) ? output : QByteArray();
}

} // namespace

ZipWriter::ZipWriter(QIODevice *device)
    : _device(device)
{
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    _dosTime = static_cast<quint16>((now.time().hour() << 11) | (now.time().minute() << 5) | (now.time().second() / 2));
    _dosDate = static_cast<quint16>((((std::max)(now.date().year(), 1980) - 1980) << 9) | (now.date().month() << 5) | now.date().day());
}

bool ZipWriter::_write(const QByteArray &bytes)
{
    if (_device->write(bytes) != bytes.size()) {
        _errorString = _device->errorString();
        return false;
    }
    return true;
}

bool ZipWriter::add(const QString &name, const QByteArray &data)
{
    constexpr qint64 maxSize = 0xFFFFFFFFLL;
    if ((data.size() >= maxSize) || (_device->pos() >= maxSize) || (_entries.count() >= GeoZoneZip::maxEntries)) {
        _errorString = tr("The pack is too large.");
        return false;
    }

    Entry entry;
    entry.name = name.toUtf8();
    entry.crc = static_cast<quint32>(crc32(0L, reinterpret_cast<const Bytef*>(data.constData()), static_cast<uInt>(data.size())));
    entry.size = static_cast<quint32>(data.size());
    entry.offset = static_cast<quint32>(_device->pos());
    QByteArray compressed = deflateRaw(data);
    if (!compressed.isEmpty() && (compressed.size() < data.size())) {
        entry.method = 8;
    } else {
        compressed = data;
        entry.method = 0;
    }
    entry.compressedSize = static_cast<quint32>(compressed.size());

    QByteArray header;
    appendU32(header, 0x04034b50);
    appendU16(header, 20);              // Version needed to extract
    appendU16(header, 0x0800);          // UTF-8 names
    appendU16(header, entry.method);
    appendU16(header, _dosTime);
    appendU16(header, _dosDate);
    appendU32(header, entry.crc);
    appendU32(header, entry.compressedSize);
    appendU32(header, entry.size);
    appendU16(header, static_cast<quint16>(entry.name.size()));
    appendU16(header, 0);               // Extra field length
    header.append(entry.name);
    if (!_write(header) || !_write(compressed)) {
        return false;
    }
    _entries.append(entry);
    return true;
}

bool ZipWriter::finish()
{
    const qint64 directoryOffset = _device->pos();
    QByteArray directory;
    for (const Entry &entry : std::as_const(_entries)) {
        appendU32(directory, 0x02014b50);
        appendU16(directory, 20);       // Version made by
        appendU16(directory, 20);       // Version needed to extract
        appendU16(directory, 0x0800);
        appendU16(directory, entry.method);
        appendU16(directory, _dosTime);
        appendU16(directory, _dosDate);
        appendU32(directory, entry.crc);
        appendU32(directory, entry.compressedSize);
        appendU32(directory, entry.size);
        appendU16(directory, static_cast<quint16>(entry.name.size()));
        appendU16(directory, 0);        // Extra field length
        appendU16(directory, 0);        // Comment length
        appendU16(directory, 0);        // Disk number
        appendU16(directory, 0);        // Internal attributes
        appendU32(directory, 0);        // External attributes
        appendU32(directory, entry.offset);
        directory.append(entry.name);
    }
    QByteArray end;
    appendU32(end, 0x06054b50);
    appendU16(end, 0);
    appendU16(end, 0);
    appendU16(end, static_cast<quint16>(_entries.count()));
    appendU16(end, static_cast<quint16>(_entries.count()));
    appendU32(end, static_cast<quint32>(directory.size()));
    appendU32(end, static_cast<quint32>(directoryOffset));
    appendU16(end, 0);                  // Comment length
    return _write(directory) && _write(end);
}

Contents read(const QString &packPath, const QString &directory)
{
    Contents contents;
    QFile pack(packPath);
    if (!pack.open(QIODevice::ReadOnly)) {
        contents.errorString = pack.errorString();
        return contents;
    }
    if (pack.size() > maxBytes) {
        contents.errorString = tr("The pack is too large.");
        return contents;
    }
    // Read from the mapped file, the files are extracted one at a time
    uchar *const mapped = (pack.size() > 0) ? pack.map(0, pack.size()) : nullptr;
    const QByteArray archive = mapped ? QByteArray::fromRawData(reinterpret_cast<const char*>(mapped), pack.size()) : pack.readAll();

    QByteArray manifestData;
    QHash<QString, QString> extracted;      // By name in the pack
    const auto removeExtracted = qScopeGuard([&extracted]() {
        for (const QString &path : std::as_const(extracted)) {
            (void) QFile::remove(path);
        }
    });
    (void) QDir().mkpath(directory);
    QString zipError;
    // Packs of earlier versions name all copies .json, whatever their content
    const QStringList suffixes = { QStringLiteral(".json"), QStringLiteral(".xml"), QStringLiteral(".zip") };
    const bool readOk = GeoZoneZip::forEachFile(archive, suffixes, maxBytes, zipError, [&](const GeoZoneZip::File &file) {
        if (file.name == QLatin1String(manifestName)) {
            manifestData = file.data;
        } else if (file.name.startsWith(QLatin1String(sourcesDir)) && !file.name.contains(QStringLiteral(".."))) {
            QFile copy(QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".part")));
            if (!copy.open(QIODevice::WriteOnly) || (copy.write(file.data) != file.data.size())) {
                zipError = copy.errorString();
                return false;
            }
            extracted.insert(file.name, copy.fileName());
        }
        return true;
    });
    if (mapped) {
        (void) pack.unmap(mapped);
    }
    if (!readOk) {
        contents.errorString = zipError;
        return contents;
    }

    const QJsonObject manifest = QJsonDocument::fromJson(manifestData).object();
    if (manifest.value(QStringLiteral("format")).toString() != QLatin1String(formatName)) {
        contents.errorString = tr("The file is not a geozone pack.");
        return contents;
    }
    if (manifest.value(QStringLiteral("version")).toInt() > version) {
        contents.errorString = tr("The geozone pack was made by a newer version of QGroundControl.");
        return contents;
    }

    for (const QJsonValue &value : manifest.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject source = value.toObject();
        const QString name = source.value(QStringLiteral("name")).toString();
        const QString filePath = extracted.take(source.value(QStringLiteral("file")).toString());
        if (source.value(QStringLiteral("key")).toString().isEmpty() || filePath.isEmpty()) {
            contents.errors.append(tr("%1: the pack has no data of the source.").arg(name));
            continue;
        }
        // The copy is the one whose provenance the pack records
        const QByteArray expectedHash = QByteArray::fromHex(source.value(QStringLiteral("provenance")).toObject().value(QStringLiteral("blake2b256")).toString().toLatin1());
        QFile file(filePath);
        const bool readable = file.open(QIODevice::ReadOnly);
        const QByteArray hash = readable ? GeoZoneParseCache::hash(file.readAll()) : QByteArray();
        file.close();
        if (!expectedHash.isEmpty() && (hash != expectedHash)) {
            (void) QFile::remove(filePath);
            contents.errors.append(tr("%1: the data in the pack is damaged.").arg(name));
            continue;
        }
        contents.sources.append(Source{ source, filePath });
    }
    return contents;
}

} // namespace GeoZonePack
