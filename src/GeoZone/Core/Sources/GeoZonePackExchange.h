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
/// publishers the user trusts, see GeoZonePackSignature.
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

    /// Writes a pack of the sources, without secrets and custom headers, signed when the signer has a key
    /// @return false when it can't be written, errorString tells why
    static bool write(const QList<ExportSource> &sources, const QString &packPath, QString &errorString, const Signer &signer = Signer());

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
    /// @param isKnown Tells whether a source with the key is known
    /// @param importedAt Time of the import, recorded in the provenance of the sources
    /// @param publishers Whose signatures are trusted
    static Import read(const QString &packPath, const QString &directory, const std::function<bool(const QString &key)> &isKnown,
                       const QDateTime &importedAt, const QList<GeoZonePackSignature::Publisher> &publishers = {});

    /// @return The service of a new source imported with the URL: the pack's without its custom headers. Empty with
    ///         errorString set when it sends requests to another server than the feed.
    static GeoZoneService importedService(const QString &url, const QJsonObject &service, QString &errorString);
};
