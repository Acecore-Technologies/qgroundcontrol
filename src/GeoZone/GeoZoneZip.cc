/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneZip.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QtEndian>

#include <zlib.h>

namespace GeoZoneZip {

namespace {

constexpr quint32 kEndOfCentralDirectory = 0x06054b50;
constexpr quint32 kCentralDirectoryEntry = 0x02014b50;
constexpr quint32 kLocalHeader = 0x04034b50;
constexpr int kEndRecordSize = 22;
constexpr int kCentralEntrySize = 46;
constexpr int kLocalHeaderSize = 30;
constexpr quint16 kStored = 0;
constexpr quint16 kDeflated = 8;

QString tr(const char *text)
{
    return QCoreApplication::translate("GeoZoneZip", text);
}

struct Entry {
    QString name;
    quint16 flags = 0;
    quint16 method = 0;
    quint32 crc = 0;
    quint32 compressedSize = 0;
    quint32 size = 0;
    quint32 localHeaderOffset = 0;
};

/// Reads little endian fields with bounds checks
class Reader
{
public:
    explicit Reader(const QByteArray &data) : _data(data) {}

    bool has(qint64 offset, qint64 length) const { return (offset >= 0) && (length >= 0) && ((offset + length) <= _data.size()); }
    quint16 u16(qint64 offset) const { return qFromLittleEndian<quint16>(_data.constData() + offset); }
    quint32 u32(qint64 offset) const { return qFromLittleEndian<quint32>(_data.constData() + offset); }
    QByteArray bytes(qint64 offset, qint64 length) const { return _data.mid(offset, length); }

private:
    const QByteArray &_data;
};

/// @return Inflated or stored data of the entry, empty on errors
QByteArray entryData(const QByteArray &archive, const Entry &entry, qint64 maxBytes, QString &errorString)
{
    const Reader reader(archive);
    const qint64 offset = entry.localHeaderOffset;
    if (!reader.has(offset, kLocalHeaderSize) || (reader.u32(offset) != kLocalHeader)) {
        errorString = tr("The zip archive is damaged.");
        return QByteArray();
    }
    const qint64 dataOffset = offset + kLocalHeaderSize + reader.u16(offset + 26) + reader.u16(offset + 28);
    if (!reader.has(dataOffset, entry.compressedSize)) {
        errorString = tr("The zip archive is damaged.");
        return QByteArray();
    }

    QByteArray data;
    if (entry.method == kStored) {
        data = reader.bytes(dataOffset, entry.compressedSize);
    } else {
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            errorString = tr("Unable to extract the zip archive.");
            return QByteArray();
        }
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(archive.constData() + dataOffset));
        stream.avail_in = entry.compressedSize;
        constexpr qint64 chunk = 1 << 20;
        int result = Z_OK;
        while (result == Z_OK) {
            // The declared size isn't trusted, the output is limited as it grows
            if ((data.size() + chunk) > (maxBytes + chunk)) {
                break;
            }
            const qsizetype written = data.size();
            data.resize(written + chunk);
            stream.next_out = reinterpret_cast<Bytef*>(data.data() + written);
            stream.avail_out = chunk;
            result = inflate(&stream, Z_NO_FLUSH);
            data.resize(written + (chunk - stream.avail_out));
        }
        inflateEnd(&stream);
        if (result != Z_STREAM_END) {
            errorString = (data.size() > maxBytes) ? tr("The file in the zip archive is too large.") : tr("The zip archive is damaged.");
            return QByteArray();
        }
    }

    if ((data.size() > maxBytes) || (data.size() != entry.size) ||
        (crc32(0L, reinterpret_cast<const Bytef*>(data.constData()), static_cast<uInt>(data.size())) != entry.crc)) {
        errorString = (data.size() > maxBytes) ? tr("The file in the zip archive is too large.") : tr("The zip archive is damaged.");
        return QByteArray();
    }
    return data;
}

} // namespace

QByteArray largestFile(const QByteArray &archive, const QStringList &suffixes, qint64 maxBytes, QString &errorString)
{
    errorString.clear();
    const Reader reader(archive);

    // The end record is followed by a comment of up to 64 KiB
    qint64 endOffset = -1;
    for (qint64 offset = archive.size() - kEndRecordSize; (offset >= 0) && (offset >= (archive.size() - kEndRecordSize - 0xFFFF)); offset--) {
        if (reader.u32(offset) == kEndOfCentralDirectory) {
            endOffset = offset;
            break;
        }
    }
    if (endOffset < 0) {
        errorString = tr("The zip archive is damaged.");
        return QByteArray();
    }
    const int entryCount = reader.u16(endOffset + 10);
    const qint64 directoryOffset = reader.u32(endOffset + 16);
    if (entryCount > maxEntries) {
        errorString = tr("The zip archive has too many files.");
        return QByteArray();
    }

    Entry best;
    bool found = false;
    qint64 offset = directoryOffset;
    for (int i = 0; i < entryCount; i++) {
        if (!reader.has(offset, kCentralEntrySize) || (reader.u32(offset) != kCentralDirectoryEntry)) {
            errorString = tr("The zip archive is damaged.");
            return QByteArray();
        }
        Entry entry;
        entry.flags = reader.u16(offset + 8);
        entry.method = reader.u16(offset + 10);
        entry.crc = reader.u32(offset + 16);
        entry.compressedSize = reader.u32(offset + 20);
        entry.size = reader.u32(offset + 24);
        const int nameLength = reader.u16(offset + 28);
        const int extraLength = reader.u16(offset + 30);
        const int commentLength = reader.u16(offset + 32);
        entry.localHeaderOffset = reader.u32(offset + 42);
        if (!reader.has(offset + kCentralEntrySize, nameLength)) {
            errorString = tr("The zip archive is damaged.");
            return QByteArray();
        }
        entry.name = QString::fromUtf8(reader.bytes(offset + kCentralEntrySize, nameLength));
        offset += kCentralEntrySize + nameLength + extraLength + commentLength;

        const bool wanted = std::any_of(suffixes.cbegin(), suffixes.cend(), [&entry](const QString &suffix) { return entry.name.endsWith(suffix, Qt::CaseInsensitive); });
        if (!wanted || entry.name.endsWith(QLatin1Char('/'))) {
            continue;
        }
        // Metadata files of the archive are small, the data is the largest file
        if (!found || (entry.size > best.size)) {
            best = entry;
            found = true;
        }
    }

    if (!found) {
        errorString = tr("The zip archive has no geozone file.");
        return QByteArray();
    }
    if ((best.flags & 0x1) || ((best.method != kStored) && (best.method != kDeflated))) {
        errorString = tr("The zip archive is encrypted or compressed in an unsupported way.");
        return QByteArray();
    }
    if (best.size > maxBytes) {
        errorString = tr("The file in the zip archive is too large.");
        return QByteArray();
    }
    return entryData(archive, best, maxBytes, errorString);
}

} // namespace GeoZoneZip
