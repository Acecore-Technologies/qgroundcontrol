/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>

#include "GeoZoneLoader.h"
#include "GeoZonePackSignature.h"
#include "GeoZoneProvenance.h"
#include "GeoZoneService.h"
#include "GeoZoneSourceIndex.h"

/// Writes offline packs (GeoZonePack) of the stored copies of sources and reads them for import, with the trust
/// policy of imported services: a pack never changes the service of a source already known, and the service of a new
/// source loses its custom headers and is refused when it sends requests to another server than its feed. Packs are
/// signed with the private key of their publisher, and the signatures of imported packs are checked against the
/// publishers the user trusts, see GeoZonePackSignature. That the data of a source was verified against the hash of
/// its publisher is only taken from a pack a trusted publisher signed, see GeoZoneProvenance::distrustPackIntegrity.
class GeoZonePackExchange
{
    Q_DECLARE_TR_FUNCTIONS(GeoZonePackExchange)

public:
    struct ExportSource {
        QJsonObject report;     ///< See GeoZoneSourceRepository::sourceReport
        GeoZoneSourceIndex::Entry entry;
        GeoZoneService service;
        QString storedPath;     ///< The stored copy
    };

    /// Who signs a pack
    struct Signer {
        QString publisher;          ///< Name the pack gives for its publisher
        QByteArray privateKeyPem;   ///< Ed25519 private key, empty to not sign
    };

    /// Writes a pack of the sources, without secrets and custom headers, signed when the signer has a key. Secrets
    /// typed into the address of a feed are left out of every address of the manifest, see shareableText.
    /// @return false when it can't be written, errorString tells why
    static bool write(const QList<ExportSource> &sources, const QString &packPath, QString &errorString, const Signer &signer = Signer());

    /// @return The web address without what authenticates its user: the values of the query parameters named like
    ///         keys, tokens, passwords or signatures (like "apikey", "access_token" or "subscription-key"), and the
    ///         password of its user info, are replaced by "***". The rest is kept as written, also the placeholders
    ///         of a template. Other texts are returned as they are.
    static QString shareableUrl(const QString &url);
    /// @return The text with shareableUrl applied to the web addresses in it
    static QString shareableText(const QString &text);
    /// @return The value with shareableText applied to all its texts
    static QJsonValue shareable(const QJsonValue &value);

    struct ImportSource {
        GeoZoneLoader::Job job;     ///< Loads the copy extracted from the pack
        bool known = false;         ///< A source with the key is known, it keeps its service and secret
        GeoZoneService service;     ///< Of a new source, the pack's without custom headers
    };

    struct Import {
        QList<ImportSource> sources;
        QStringList errors;         ///< Of the sources left out
        QString errorString;        ///< Why the pack can't be read, empty when it can
        GeoZoneProvenance::PackSignature signature = GeoZoneProvenance::PackSignature::Unsigned;
        QString signer;             ///< See GeoZoneProvenance::packSigner
        QByteArray publicKey;       ///< Of the signature
    };

    /// Extracts the sources of a pack into the directory. A pack whose signature doesn't match its manifest can't be
    /// read; its signature is recorded in the provenance of its sources otherwise. Builds which can't check
    /// signatures import signed packs as unverified, like unsigned ones.
    /// @param localKey Gives the key of the source here which a source of the pack is, nothing when it isn't known:
    ///                 the key itself, or the key of the feed whose address has the secrets shareableUrl left out of
    ///                 the pack, which then takes the data
    /// @param importedAt Time of the import, recorded in the provenance of the sources
    /// @param publishers Whose signatures are trusted
    static Import read(const QString &packPath, const QString &directory, const std::function<QString(const QString &key)> &localKey,
                       const QDateTime &importedAt, const QList<GeoZonePackSignature::Publisher> &publishers = {});

    /// @return The service of a new source imported with the URL: the pack's without its custom headers. Empty with
    ///         errorString set when it sends requests to another server than the feed.
    static GeoZoneService importedService(const QString &url, const QJsonObject &service, QString &errorString);
};
