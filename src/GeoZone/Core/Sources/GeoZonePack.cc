/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePack.h"
#include "GeoZoneHash.h"
#include "GeoZoneZip.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QScopeGuard>
#include <QtCore/QUuid>

namespace GeoZonePack {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("GeoZonePack", text);
}

} // namespace

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
    // By name in the pack. Those which don't become the copy of a source are removed.
    QHash<QString, QString> extracted;
    const auto removeExtracted = qScopeGuard([&extracted]() {
        for (const QString &path : std::as_const(extracted)) {
            (void) QFile::remove(path);
        }
    });
    (void) QDir().mkpath(directory);
    QString zipError;
    // Of the callback, which stops the reading without making it fail
    QString extractError;
    // Packs of earlier versions name all copies .json, whatever their content
    const QStringList suffixes = { QStringLiteral(".json"), QStringLiteral(".xml"), QStringLiteral(".zip") };
    const bool readOk = GeoZoneZip::forEachFile(archive, suffixes, maxBytes, zipError, [&](const GeoZoneZip::File &file) {
        if (file.name == QLatin1String(manifestName)) {
            manifestData = file.data;
        } else if (file.name == QLatin1String(signatureName)) {
            contents.signatureData = file.data;
        } else if (file.name.startsWith(QLatin1String(sourcesDir)) && !file.name.contains(QStringLiteral(".."))) {
            // Which of two files of one name the manifest means can't be told
            if (extracted.contains(file.name)) {
                extractError = tr("The pack has several files named %1.").arg(file.name);
                return false;
            }
            QFile copy(QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".part")));
            if (!copy.open(QIODevice::WriteOnly)) {
                extractError = tr("The files of the pack could not be extracted to %1: %2").arg(directory, copy.errorString());
                return false;
            }
            extracted.insert(file.name, copy.fileName());
            if ((copy.write(file.data) != file.data.size()) || !copy.flush()) {
                extractError = tr("The files of the pack could not be extracted to %1: %2").arg(directory, copy.errorString());
                return false;
            }
        }
        return true;
    });
    if (mapped) {
        (void) pack.unmap(mapped);
    }
    if (!readOk || !extractError.isEmpty()) {
        contents.errorString = readOk ? extractError : zipError;
        return contents;
    }

    contents.manifestData = manifestData;
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
        // Kept with the extracted files, which are removed, until the caller takes it
        const QString fileName = source.value(QStringLiteral("file")).toString();
        const QString filePath = extracted.value(fileName);
        if (source.value(QStringLiteral("key")).toString().isEmpty() || filePath.isEmpty()) {
            contents.errors.append(tr("%1: the pack has no data of the source.").arg(name));
            continue;
        }
        // The copy is the one the manifest, which the signature covers, and its provenance record
        const QByteArray expectedHash = QByteArray::fromHex(source.value(QStringLiteral("provenance")).toObject().value(QStringLiteral("blake2b256")).toString().toLatin1());
        const QByteArray expectedSha256 = QByteArray::fromHex(source.value(QStringLiteral("fileSha256")).toString().toLatin1());
        QFile file(filePath);
        const QByteArray data = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        file.close();
        if ((!expectedHash.isEmpty() && (GeoZoneHash::contentHash(data) != expectedHash)) ||
            (!expectedSha256.isEmpty() && (GeoZoneHash::sha256(data) != expectedSha256))) {
            contents.errors.append(tr("%1: the data in the pack is damaged.").arg(name));
            continue;
        }
        (void) extracted.remove(fileName);
        contents.sources.append(Source{ source, filePath });
    }
    return contents;
}

} // namespace GeoZonePack
