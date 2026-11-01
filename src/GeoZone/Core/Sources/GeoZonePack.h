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
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

class QIODevice;

/// Offline packs of geozone sources, to take their zones to a computer without network access: a zip archive with
/// the stored copy of each source in "sources/", and "manifest.json" describing them. The manifest has the format
/// name and version, when the pack was made, and per source GeoZoneSourceRepository::sourceReport with the file of
/// its copy, the SHA-256 of that file and the index entry fields needed to restore it, like its validators and area.
/// "signature.json", when the pack is signed, has the Ed25519 signature of the manifest, see GeoZonePackSignature.
/// Packs hold no secrets. GeoZonePackExchange writes and imports them.
namespace GeoZonePack {

constexpr const char *manifestName = "manifest.json";
constexpr const char *signatureName = "signature.json";
constexpr const char *sourcesDir = "sources/";
constexpr const char *formatName = "qgc-geozone-pack";
constexpr int version = 1;
constexpr qint64 maxBytes = 1024LL * 1024 * 1024;   ///< Of the pack and of its files together

/// Writes a zip archive, the files deflated unless that doesn't make them smaller. Sizes are limited to 4 GiB.
class ZipWriter
{
public:
    explicit ZipWriter(QIODevice *device);

    bool add(const QString &name, const QByteArray &data);
    /// Writes the central directory
    bool finish();
    QString errorString() const { return _errorString; }

private:
    struct Entry {
        QByteArray name;
        quint16 method = 0;
        quint32 crc = 0;
        quint32 compressedSize = 0;
        quint32 size = 0;
        quint32 offset = 0;
    };

    bool _write(const QByteArray &bytes);

    QIODevice *_device = nullptr;
    QList<Entry> _entries;
    quint16 _dosTime = 0;
    quint16 _dosDate = 0;
    QString _errorString;
};

struct Source {
    QJsonObject manifest;   ///< Of the source in the manifest
    QString filePath;       ///< The stored copy extracted, which the caller adopts or removes
};

struct Contents {
    QByteArray manifestData;    ///< The manifest as in the pack, which the signature signs
    QByteArray signatureData;   ///< The signature file, empty when the pack isn't signed
    QList<Source> sources;
    QStringList errors;     ///< Of the sources which were left out, like when their copy is damaged
    QString errorString;    ///< Why the pack can't be read, empty when it can
};

/// Extracts the stored copies of a pack into the directory, checking them against their SHA-256 in the manifest and
/// the hashes of their provenance
Contents read(const QString &packPath, const QString &directory);

} // namespace GeoZonePack
