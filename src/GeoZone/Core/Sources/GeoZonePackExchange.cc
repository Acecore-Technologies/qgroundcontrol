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
#include "GeoZoneHash.h"
#include "GeoZonePack.h"
#include "GeoZoneSource.h"
#include "GeoZoneZip.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QUrl>

#include <algorithm>
#include <QtCore/QSet>
#include <QtCore/QRegularExpression>

namespace {

/// @return true for the name of a query parameter whose value authenticates: a key, token, password or signature
bool secretParameter(const QString &name)
{
    // The whole name without its separators, and the words of names like "subscription-key" or "accessToken"
    static const QSet<QString> names = {
        QStringLiteral("apikey"), QStringLiteral("appkey"), QStringLiteral("appid"), QStringLiteral("accesstoken"), QStringLiteral("clientsecret"),
        QStringLiteral("code"), QStringLiteral("pass"), QStringLiteral("sas"), QStringLiteral("session"), QStringLiteral("sessionid"),
    };
    static const QSet<QString> words = {
        QStringLiteral("key"), QStringLiteral("apikey"), QStringLiteral("token"), QStringLiteral("secret"), QStringLiteral("password"), QStringLiteral("passwd"),
        QStringLiteral("pwd"), QStringLiteral("auth"), QStringLiteral("authorization"), QStringLiteral("signature"), QStringLiteral("sig"),
        QStringLiteral("credential"), QStringLiteral("credentials"), QStringLiteral("jwt"), QStringLiteral("bearer"),
    };
    static const QRegularExpression separators(QStringLiteral("[^A-Za-z0-9]+|(?<=[a-z0-9])(?=[A-Z])"));
    const QStringList nameWords = QUrl::fromPercentEncoding(name.toUtf8()).split(separators, Qt::SkipEmptyParts);
    if (names.contains(nameWords.join(QString()).toLower())) {
        return true;
    }
    return std::any_of(nameWords.cbegin(), nameWords.cend(), [](const QString &word) { return words.contains(word.toLower()); });
}

} // namespace

QString GeoZonePackExchange::shareableUrl(const QString &url)
{
    const QString hidden = QStringLiteral("***");
    if (!url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) && !url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        return url;
    }
    QString result = url;
    // user:password@host
    const qsizetype authorityStart = result.indexOf(QStringLiteral("://")) + 3;
    static const QRegularExpression authorityEndRegExp(QStringLiteral("[/?#]"));
    qsizetype authorityEnd = result.indexOf(authorityEndRegExp, authorityStart);
    if (authorityEnd < 0) {
        authorityEnd = result.size();
    }
    const QStringView authority = QStringView(result).mid(authorityStart, authorityEnd - authorityStart);
    if (const qsizetype at = authority.lastIndexOf(QLatin1Char('@')); at >= 0) {
        if (const qsizetype colon = authority.indexOf(QLatin1Char(':')); (colon >= 0) && (colon < at - 1)) {
            (void) result.replace(authorityStart + colon + 1, at - colon - 1, hidden);
        }
    }

    const qsizetype queryStart = result.indexOf(QLatin1Char('?'));
    if (queryStart < 0) {
        return result;
    }
    qsizetype queryEnd = result.indexOf(QLatin1Char('#'), queryStart);
    if (queryEnd < 0) {
        queryEnd = result.size();
    }
    // As written: the address may be a template, which parsing and writing it again would change
    QStringList parameters = result.mid(queryStart + 1, queryEnd - queryStart - 1).split(QLatin1Char('&'));
    for (QString &parameter : parameters) {
        const qsizetype equals = parameter.indexOf(QLatin1Char('='));
        if ((equals > 0) && (equals < parameter.size() - 1) && secretParameter(parameter.left(equals))) {
            parameter = parameter.left(equals + 1) + hidden;
        }
    }
    return result.left(queryStart + 1) + parameters.join(QLatin1Char('&')) + result.mid(queryEnd);
}

QString GeoZonePackExchange::shareableText(const QString &text)
{
    static const QRegularExpression urlRegExp(QStringLiteral("https?://[^\\s\"'<>]+"), QRegularExpression::CaseInsensitiveOption);
    QString result;
    qsizetype copied = 0;
    QRegularExpressionMatchIterator matches = urlRegExp.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result.append(QStringView(text).mid(copied, match.capturedStart() - copied));
        result.append(shareableUrl(match.captured()));
        copied = match.capturedEnd();
    }
    result.append(QStringView(text).mid(copied));
    return result;
}

QJsonValue GeoZonePackExchange::shareable(const QJsonValue &value)
{
    if (value.isString()) {
        return shareableText(value.toString());
    }
    if (value.isArray()) {
        QJsonArray array;
        for (const QJsonValue &element : value.toArray()) {
            array.append(shareable(element));
        }
        return array;
    }
    if (value.isObject()) {
        QJsonObject object;
        const QJsonObject members = value.toObject();
        for (auto it = members.constBegin(); it != members.constEnd(); ++it) {
            object.insert(it.key(), shareable(it.value()));
        }
        return object;
    }
    return value;
}

bool GeoZonePackExchange::write(const QList<ExportSource> &sources, const QString &packPath, QString &errorString, const Signer &signer)
{
    QSaveFile file(packPath);
    if (!file.open(QIODevice::WriteOnly)) {
        errorString = file.errorString();
        return false;
    }
    GeoZoneZip::Writer writer(&file);
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
        object.insert(QStringLiteral("fileSha256"), QString::fromLatin1(GeoZoneHash::sha256(data).toHex()));
        // The report has them as lastChanges
        (void) object.remove(QStringLiteral("changes"));
        if (source.service.isEmpty()) {
            (void) object.remove(QStringLiteral("service"));
        } else {
            // The service with its user, which identifies but doesn't authenticate, without its headers, which may hold keys
            object.insert(QStringLiteral("service"), source.service.shareableJson());
        }
        // A key typed into the address of a feed is in its key, its URLs and its provenance
        manifestSources.append(shareable(object));
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
                                                      const std::function<QString(const QString &key)> &localKey, const QDateTime &importedAt,
                                                      const QList<GeoZonePackSignature::Publisher> &publishers)
{
    Import import;
    const GeoZonePack::Contents contents = GeoZonePack::read(packPath, directory);
    if (!contents.errorString.isEmpty()) {
        import.errorString = tr("The geozone pack could not be read. %1").arg(contents.errorString);
        return import;
    }

    const auto removeCopies = [&contents]() {
        for (const GeoZonePack::Source &packSource : contents.sources) {
            (void) QFile::remove(packSource.filePath);
        }
    };
    if (!contents.signatureData.isEmpty()) {
        // A signature file which isn't one is neither a valid signature nor none: the pack is refused in every build
        QJsonParseError parseError;
        const QJsonDocument signatureDocument = QJsonDocument::fromJson(contents.signatureData, &parseError);
        if ((parseError.error != QJsonParseError::NoError) || !signatureDocument.isObject()) {
            removeCopies();
            import.errorString = tr("The geozone pack could not be read. Its signature file is damaged.");
            return import;
        }
        const QJsonObject signature = signatureDocument.object();
        import.publicKey = QByteArray::fromBase64(signature.value(QStringLiteral("publicKey")).toString().toLatin1());
        import.signer = signature.value(QStringLiteral("publisher")).toString().trimmed();
        if (!GeoZonePackSignature::available()) {
            // Taken as unsigned: the publisher and key it gives aren't recorded, and are never trusted
            import.signature = GeoZoneProvenance::PackSignature::Unverified;
            import.signer.clear();
            import.publicKey.clear();
        } else if ((signature.value(QStringLiteral("algorithm")).toString() != QLatin1String(GeoZonePackSignature::algorithm)) ||
                   !GeoZonePackSignature::verify(import.publicKey, contents.manifestData,
                                                 QByteArray::fromBase64(signature.value(QStringLiteral("signature")).toString().toLatin1()))) {
            // The pack was changed after it was signed
            removeCopies();
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
        GeoZoneSourceIndex::Entry entry = GeoZoneSourceIndex::entryFromJson(packSource.manifest);
        // Feeds are requested at their key, which the service was checked against
        if (!entry.url.isEmpty() && (entry.url != entry.key)) {
            (void) QFile::remove(packSource.filePath);
            import.errors.append(tr("%1: its address doesn't match the source.").arg(packSource.manifest.value(QStringLiteral("name")).toString()));
            continue;
        }
        ImportSource source;
        const QString known = localKey(entry.key);
        source.known = !known.isEmpty();
        if (source.known && (known != entry.key)) {
            // The feed here whose address has the secret the pack left out
            entry.key = known;
            entry.url = known;
        }
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
        // The manifest may say that the data was verified against its publisher's hash
        source.job.provenance.distrustPackIntegrity();
        import.sources.append(source);
    }
    return import;
}
