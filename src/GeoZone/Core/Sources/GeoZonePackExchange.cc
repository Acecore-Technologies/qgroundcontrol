/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePackExchange.h"
#include "GeoZoneEnvironment.h"
#include "GeoZonePack.h"
#include "GeoZoneSource.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>

bool GeoZonePackExchange::write(const QList<ExportSource> &sources, const QString &packPath, QString &errorString, const Signer &signer)
{
    QSaveFile file(packPath);
    if (!file.open(QIODevice::WriteOnly)) {
        errorString = file.errorString();
        return false;
    }
    GeoZonePack::ZipWriter writer(&file);
    QJsonArray manifestSources;
    for (const ExportSource &source : sources) {
        QFile stored(source.storedPath);
        if (!stored.open(QIODevice::ReadOnly)) {
            errorString = tr("The stored copy of %1 could not be read: %2").arg(source.report.value(QStringLiteral("name")).toString(), stored.errorString());
            return false;
        }
        const QString name = QLatin1String(GeoZonePack::sourcesDir) + source.entry.fileName;
        const QByteArray data = stored.readAll();
        if (!writer.add(name, data)) {
            errorString = writer.errorString();
            return false;
        }

        // The fields of the index restore the source, the report describes it
        QJsonObject object = GeoZoneSourceIndex::entryToJson(source.entry);
        for (auto it = source.report.constBegin(); it != source.report.constEnd(); ++it) {
            object.insert(it.key(), it.value());
        }
        object.insert(QStringLiteral("file"), name);
        // In the manifest the signature covers
        object.insert(QStringLiteral("fileSha256"), QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));
        // The report has them as lastChanges
        (void) object.remove(QStringLiteral("changes"));
        if (source.service.isEmpty()) {
            (void) object.remove(QStringLiteral("service"));
        } else {
            // The service with its user, which identifies but doesn't authenticate, without its headers, which may hold keys
            object.insert(QStringLiteral("service"), source.service.shareableJson());
        }
        manifestSources.append(object);
    }
    if (manifestSources.isEmpty()) {
        errorString = tr("None of the sources has stored data to export.");
        return false;
    }

    const QJsonObject manifest{
        { QStringLiteral("format"), QLatin1String(GeoZonePack::formatName) },
        { QStringLiteral("version"), GeoZonePack::version },
        { QStringLiteral("createdAt"), GeoZoneEnvironment::currentDateTimeUtc().toString(Qt::ISODate) },
        { QStringLiteral("generator"), QStringLiteral("%1 %2").arg(QCoreApplication::applicationName(), QCoreApplication::applicationVersion()).trimmed() },
        { QStringLiteral("sources"), manifestSources },
    };
    const QByteArray manifestData = QJsonDocument(manifest).toJson();
    if (!writer.add(QLatin1String(GeoZonePack::manifestName), manifestData)) {
        errorString = writer.errorString();
        return false;
    }
    if (!signer.privateKeyPem.isEmpty()) {
        const QByteArray publicKey = GeoZonePackSignature::publicKey(signer.privateKeyPem, errorString);
        const QByteArray signature = publicKey.isEmpty() ? QByteArray() : GeoZonePackSignature::sign(signer.privateKeyPem, manifestData, errorString);
        if (signature.isEmpty()) {
            return false;
        }
        const QJsonObject signatureObject{
            { QStringLiteral("algorithm"), QLatin1String(GeoZonePackSignature::algorithm) },
            { QStringLiteral("publisher"), signer.publisher },
            { QStringLiteral("publicKey"), QString::fromLatin1(publicKey.toBase64()) },
            { QStringLiteral("signature"), QString::fromLatin1(signature.toBase64()) },
        };
        if (!writer.add(QLatin1String(GeoZonePack::signatureName), QJsonDocument(signatureObject).toJson())) {
            errorString = writer.errorString();
            return false;
        }
    }
    if (!writer.finish()) {
        errorString = writer.errorString();
        return false;
    }
    if (!file.commit()) {
        errorString = file.errorString();
        return false;
    }
    qCDebug(GeoZoneSourceLog) << "Exported" << manifestSources.count() << "sources to" << packPath;
    return true;
}

GeoZoneService GeoZonePackExchange::importedService(const QString &url, const QJsonObject &service, QString &errorString)
{
    errorString.clear();
    if (url.isEmpty() || service.isEmpty()) {
        return GeoZoneService();
    }
    GeoZoneService imported = GeoZoneService::fromJson(service);
    imported.headers.clear();
    if (const QStringList otherOrigins = imported.otherOrigins(url); !otherOrigins.isEmpty()) {
        errorString = tr("its service sends requests to %1, another server than the feed's. Add the service again to trust it.")
                          .arg(otherOrigins.join(QStringLiteral(", ")));
        return GeoZoneService();
    }
    return imported;
}

GeoZonePackExchange::Import GeoZonePackExchange::read(const QString &packPath, const QString &directory,
                                                      const std::function<bool(const QString &key)> &isKnown, const QDateTime &importedAt,
                                                      const QList<GeoZonePackSignature::Publisher> &publishers)
{
    Import import;
    const GeoZonePack::Contents contents = GeoZonePack::read(packPath, directory);
    if (!contents.errorString.isEmpty()) {
        import.errorString = tr("The geozone pack could not be read. %1").arg(contents.errorString);
        return import;
    }

    if (!contents.signatureData.isEmpty()) {
        const QJsonObject signature = QJsonDocument::fromJson(contents.signatureData).object();
        import.publicKey = QByteArray::fromBase64(signature.value(QStringLiteral("publicKey")).toString().toLatin1());
        import.signer = signature.value(QStringLiteral("publisher")).toString().trimmed();
        if (!GeoZonePackSignature::available()) {
            import.signature = GeoZoneProvenance::PackSignature::Unverified;
        } else if ((signature.value(QStringLiteral("algorithm")).toString() != QLatin1String(GeoZonePackSignature::algorithm)) ||
                   !GeoZonePackSignature::verify(import.publicKey, contents.manifestData,
                                                 QByteArray::fromBase64(signature.value(QStringLiteral("signature")).toString().toLatin1()))) {
            // The pack was changed after it was signed
            for (const GeoZonePack::Source &packSource : contents.sources) {
                (void) QFile::remove(packSource.filePath);
            }
            import.errorString = tr("The geozone pack could not be read. Its signature doesn't match its content, it was changed after it was signed.");
            return import;
        } else if (const GeoZonePackSignature::Publisher *const publisher = GeoZonePackSignature::find(publishers, import.publicKey)) {
            import.signature = GeoZoneProvenance::PackSignature::Signed;
            import.signer = publisher->name;
        } else {
            import.signature = GeoZoneProvenance::PackSignature::Untrusted;
        }
    }

    import.errors = contents.errors;
    const QString packName = QFileInfo(packPath).fileName();
    for (const GeoZonePack::Source &packSource : contents.sources) {
        const GeoZoneSourceIndex::Entry entry = GeoZoneSourceIndex::entryFromJson(packSource.manifest);
        ImportSource source;
        source.known = isKnown(entry.key);
        if (!source.known) {
            QString serviceError;
            source.service = importedService(entry.url, entry.service, serviceError);
            if (!serviceError.isEmpty()) {
                (void) QFile::remove(packSource.filePath);
                import.errors.append(QStringLiteral("%1: %2").arg(packSource.manifest.value(QStringLiteral("name")).toString(), serviceError));
                continue;
            }
        }
        source.job = GeoZoneLoader::importedJob(entry, packSource.filePath, packName, importedAt);
        source.job.provenance.packSignature = import.signature;
        source.job.provenance.packSigner = (import.signature == GeoZoneProvenance::PackSignature::Unsigned) ? QString() : import.signer;
        source.job.provenance.packPublicKey = import.publicKey;
        import.sources.append(source);
    }
    return import;
}
