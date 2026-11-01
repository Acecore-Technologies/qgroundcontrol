/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneProvenance.h"

#include <QtCore/QJsonArray>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>

namespace {

constexpr const char *providerKey = "provider";
constexpr const char *requestUrlKey = "requestUrl";
constexpr const char *finalUrlKey = "finalUrl";
constexpr const char *redirectsKey = "redirects";
constexpr const char *etagKey = "etag";
constexpr const char *lastModifiedKey = "lastModified";
constexpr const char *fetchedAtKey = "fetchedAt";
constexpr const char *sha256Key = "sha256";
constexpr const char *contentHashKey = "blake2b256";
constexpr const char *sizeKey = "size";
constexpr const char *areaKey = "area";
constexpr const char *integrityKey = "integrity";
constexpr const char *integrityTextKey = "integrityCheck";
constexpr const char *importedKey = "imported";
constexpr const char *packKey = "pack";
constexpr const char *importedAtKey = "importedAt";
constexpr const char *packSignatureKey = "packSignature";
constexpr const char *packSignerKey = "packSigner";
constexpr const char *packPublicKeyKey = "packKey";

QString shortHash(const QByteArray &hash)
{
    return QString::fromLatin1(hash.toHex().left(12));
}

} // namespace

QString GeoZoneProvenance::integrityToString(Integrity integrity)
{
    switch (integrity) {
    case Integrity::Verified: return QStringLiteral("verified");
    case Integrity::Failed: return QStringLiteral("failed");
    case Integrity::Unchecked: break;
    }
    return QStringLiteral("unchecked");
}

QJsonObject GeoZoneProvenance::toJson() const
{
    QJsonObject object;
    const auto insert = [&object](const char *key, const QString &value) {
        if (!value.isEmpty()) {
            object.insert(QLatin1String(key), value);
        }
    };
    insert(providerKey, providerId);
    insert(requestUrlKey, requestUrl);
    insert(finalUrlKey, finalUrl);
    if (!redirects.isEmpty()) {
        object.insert(QLatin1String(redirectsKey), QJsonArray::fromStringList(redirects));
    }
    insert(etagKey, etag);
    insert(lastModifiedKey, lastModified);
    if (fetchedAt.isValid()) {
        object.insert(QLatin1String(fetchedAtKey), fetchedAt.toUTC().toString(Qt::ISODate));
    }
    insert(sha256Key, QString::fromLatin1(sha256.toHex()));
    insert(contentHashKey, QString::fromLatin1(contentHash.toHex()));
    if (size >= 0) {
        object.insert(QLatin1String(sizeKey), size);
    }
    if (area.isValid()) {
        object.insert(QLatin1String(areaKey), QJsonArray{ area.topLeft().longitude(), area.bottomRight().latitude(),
                                                         area.bottomRight().longitude(), area.topLeft().latitude() });
    }
    object.insert(QLatin1String(integrityKey), integrityToString(integrity));
    insert(integrityTextKey, integrityText);
    if (imported) {
        object.insert(QLatin1String(importedKey), true);
        insert(packKey, packName);
        if (importedAt.isValid()) {
            object.insert(QLatin1String(importedAtKey), importedAt.toUTC().toString(Qt::ISODate));
        }
        if (packSignature != PackSignature::None) {
            object.insert(QLatin1String(packSignatureKey), packSignatureToString(packSignature));
            insert(packSignerKey, packSigner);
            insert(packPublicKeyKey, QString::fromLatin1(packPublicKey.toBase64()));
        }
    }
    return object;
}

GeoZoneProvenance GeoZoneProvenance::fromJson(const QJsonObject &object)
{
    GeoZoneProvenance provenance;
    provenance.providerId = object.value(QLatin1String(providerKey)).toString();
    provenance.requestUrl = object.value(QLatin1String(requestUrlKey)).toString();
    provenance.finalUrl = object.value(QLatin1String(finalUrlKey)).toString();
    for (const QJsonValue &redirect : object.value(QLatin1String(redirectsKey)).toArray()) {
        provenance.redirects.append(redirect.toString());
    }
    provenance.etag = object.value(QLatin1String(etagKey)).toString();
    provenance.lastModified = object.value(QLatin1String(lastModifiedKey)).toString();
    provenance.fetchedAt = QDateTime::fromString(object.value(QLatin1String(fetchedAtKey)).toString(), Qt::ISODate);
    provenance.sha256 = QByteArray::fromHex(object.value(QLatin1String(sha256Key)).toString().toLatin1());
    provenance.contentHash = QByteArray::fromHex(object.value(QLatin1String(contentHashKey)).toString().toLatin1());
    provenance.size = object.value(QLatin1String(sizeKey)).toInteger(-1);
    const QJsonArray area = object.value(QLatin1String(areaKey)).toArray();
    if (area.count() == 4) {
        provenance.area = QGeoRectangle(QGeoCoordinate(area[3].toDouble(), area[0].toDouble()), QGeoCoordinate(area[1].toDouble(), area[2].toDouble()));
    }
    const QString integrity = object.value(QLatin1String(integrityKey)).toString();
    for (const Integrity candidate : { Integrity::Verified, Integrity::Failed }) {
        if (integrity == integrityToString(candidate)) {
            provenance.integrity = candidate;
        }
    }
    provenance.integrityText = object.value(QLatin1String(integrityTextKey)).toString();
    provenance.imported = object.value(QLatin1String(importedKey)).toBool();
    provenance.packName = object.value(QLatin1String(packKey)).toString();
    provenance.importedAt = QDateTime::fromString(object.value(QLatin1String(importedAtKey)).toString(), Qt::ISODate);
    const QString signature = object.value(QLatin1String(packSignatureKey)).toString();
    for (const PackSignature candidate : { PackSignature::Unsigned, PackSignature::Unverified, PackSignature::Untrusted, PackSignature::Signed }) {
        if (signature == packSignatureToString(candidate)) {
            provenance.packSignature = candidate;
        }
    }
    provenance.packSigner = object.value(QLatin1String(packSignerKey)).toString();
    provenance.packPublicKey = QByteArray::fromBase64(object.value(QLatin1String(packPublicKeyKey)).toString().toLatin1());
    return provenance;
}

QString GeoZoneProvenance::packSignatureToString(PackSignature signature)
{
    switch (signature) {
    case PackSignature::None: return QString();
    case PackSignature::Unsigned: return QStringLiteral("unsigned");
    case PackSignature::Unverified: return QStringLiteral("unverified");
    case PackSignature::Untrusted: return QStringLiteral("untrusted");
    case PackSignature::Signed: return QStringLiteral("signed");
    }
    return QString();
}

QString GeoZoneProvenance::packSignatureText() const
{
    switch (packSignature) {
    case PackSignature::None:
        return QString();
    case PackSignature::Unsigned:
        return tr("unsigned");
    case PackSignature::Unverified:
        return tr("signature not checked");
    case PackSignature::Untrusted:
        return packSigner.isEmpty() ? tr("untrusted signature") : tr("untrusted signature of \"%1\"").arg(packSigner);
    case PackSignature::Signed:
        return tr("signed by %1").arg(packSigner);
    }
    return QString();
}

QString GeoZoneProvenance::summary() const
{
    if (isEmpty()) {
        return QString();
    }
    const QLocale locale;
    QStringList parts;
    if (imported) {
        parts.append(tr("imported from %1 on %2").arg(packName, locale.toString(importedAt.toLocalTime(), QLocale::ShortFormat)));
        if (const QString signature = packSignatureText(); !signature.isEmpty()) {
            parts.append(signature);
        }
    }
    const QUrl url(finalUrl.isEmpty() ? requestUrl : finalUrl);
    if (url.isLocalFile() || url.scheme().isEmpty()) {
        parts.append(tr("loaded %1").arg(locale.toString(fetchedAt.toLocalTime(), QLocale::ShortFormat)));
    } else {
        parts.append(tr("downloaded %1 from %2").arg(locale.toString(fetchedAt.toLocalTime(), QLocale::ShortFormat), url.host()));
    }
    if (!redirects.isEmpty()) {
        parts.append(tr("redirected from %1").arg(QUrl(redirects.first()).host()));
    }
    if (!etag.isEmpty()) {
        parts.append(tr("ETag %1").arg(etag));
    } else if (!lastModified.isEmpty()) {
        parts.append(tr("modified %1").arg(lastModified));
    }
    if (!sha256.isEmpty()) {
        parts.append(tr("SHA-256 %1…").arg(shortHash(sha256)));
    } else if (!contentHash.isEmpty()) {
        parts.append(tr("BLAKE2b %1…").arg(shortHash(contentHash)));
    }
    if (integrity == Integrity::Verified) {
        parts.append(tr("verified"));
    } else if (integrity == Integrity::Failed) {
        parts.append(tr("integrity check failed"));
    }
    return parts.join(QStringLiteral(", "));
}

QByteArray GeoZoneProvenance::digestHeaderSha256(const QHash<QByteArray, QByteArray> &headers, QString &headerName)
{
    // The digests are of the content as encoded, which Qt decodes, so encoded responses can't be checked
    const QByteArray encoding = headers.value("content-encoding").trimmed().toLower();
    if (!encoding.isEmpty() && (encoding != "identity")) {
        return QByteArray();
    }
    // RFC 9530 dictionaries: sha-256=:base64:
    for (const QByteArray &name : { QByteArray("repr-digest"), QByteArray("content-digest") }) {
        for (const QByteArray &member : headers.value(name).split(',')) {
            const QByteArray trimmed = member.trimmed();
            if (trimmed.toLower().startsWith("sha-256=:") && trimmed.endsWith(':') && (trimmed.size() > 10)) {
                headerName = QString::fromLatin1(name);
                return QByteArray::fromBase64(trimmed.mid(9, trimmed.size() - 10));
            }
        }
    }
    // RFC 3230 Digest: SHA-256=base64
    for (const QByteArray &member : headers.value("digest").split(',')) {
        const QByteArray trimmed = member.trimmed();
        if (trimmed.toLower().startsWith("sha-256=")) {
            headerName = QStringLiteral("digest");
            return QByteArray::fromBase64(trimmed.mid(8));
        }
    }
    return QByteArray();
}

QByteArray GeoZoneProvenance::parseSha256File(const QByteArray &content)
{
    static const QRegularExpression pattern(QStringLiteral("^\\s*([0-9a-fA-F]{64})(\\s|$)"));
    const QRegularExpressionMatch match = pattern.match(QString::fromLatin1(content.left(1024)));
    return match.hasMatch() ? QByteArray::fromHex(match.captured(1).toLatin1()) : QByteArray();
}
