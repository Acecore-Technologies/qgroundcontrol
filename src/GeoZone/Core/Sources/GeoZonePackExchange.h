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
#include "GeoZoneService.h"
#include "GeoZoneSourceIndex.h"

/// Writes offline packs (GeoZonePack) of the stored copies of sources and reads them for import, with the trust
/// policy of imported services: a pack never changes the service of a source already known, and the service of a new
/// source loses its custom headers and is refused when it sends requests to another server than its feed.
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

    /// Writes a pack of the sources, without secrets and custom headers
    /// @return false when it can't be written, errorString tells why
    static bool write(const QList<ExportSource> &sources, const QString &packPath, QString &errorString);

    struct ImportSource {
        GeoZoneLoader::Job job;     ///< Loads the copy extracted from the pack
        bool known = false;         ///< A source with the key is known, it keeps its service and secret
        GeoZoneService service;     ///< Of a new source, the pack's without custom headers
    };

    struct Import {
        QList<ImportSource> sources;
        QStringList errors;         ///< Of the sources left out
        QString errorString;        ///< Why the pack can't be read, empty when it can
    };

    /// Extracts the sources of a pack into the directory
    /// @param isKnown Tells whether a source with the key is known
    /// @param importedAt Time of the import, recorded in the provenance of the sources
    static Import read(const QString &packPath, const QString &directory, const std::function<bool(const QString &key)> &isKnown,
                       const QDateTime &importedAt);

    /// @return The service of a new source imported with the URL: the pack's without its custom headers. Empty with
    ///         errorString set when it sends requests to another server than the feed.
    static GeoZoneService importedService(const QString &url, const QJsonObject &service, QString &errorString);
};
