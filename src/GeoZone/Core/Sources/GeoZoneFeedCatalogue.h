/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "GeoZoneProviders.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>

/// Reads feed catalogues: the lists of the feeds, area services and service templates QGroundControl offers. The
/// built-in catalogue is the GeoZoneFeeds.json resource, which custom builds can replace (GEOZONE_FEEDS_FILE). A
/// manufacturer or operator extends or changes it without rebuilding with a catalogue file in the geozone data folder
/// (fileName) or a catalogue downloaded from the feed catalogue URL of the settings: their entries replace the entries
/// with the same id, or turn them off ("enabled": false). The format is described in the user guide, Adding or
/// Changing Feeds.
class GeoZoneFeedCatalogue
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneFeedCatalogue)

public:
    static constexpr int version = 1;      ///< Catalogues of a later version are refused
    static constexpr const char *fileType = "GeoZoneFeeds";
    static constexpr const char *builtInPath = ":/geozone/GeoZoneFeeds.json";
    static constexpr const char *fileName = "feeds.json";   ///< Catalogue file in the geozone data folder
    /// Context of the translations of the built-in names and notes, the file name for translations/qgc-lupdate-json.py
    static constexpr const char *translationContext = "GeoZoneFeeds.json";

    struct Catalogue {
        QList<GeoZoneProviders::Service> feeds;
        QList<GeoZoneProviders::ServiceTemplate> templates;
        QStringList disabled;   ///< Ids of the feeds and templates the catalogue turns off
        QStringList warnings;   ///< The entries left out and why, translated

        int entryCount() const { return static_cast<int>(feeds.count() + templates.count() + disabled.count()); }
    };

    /// Reads a catalogue. Entries which can't be used are left out with a warning.
    /// @param origin Catalogue named in the warnings and set as the entries' origin, empty for the built-in one,
    ///               whose names and notes are translated
    /// @return false with the reason when the document isn't a catalogue of a supported version
    static bool parse(const QByteArray &json, const QString &origin, Catalogue &catalogue, QString &errorString);

    /// @return The built-in catalogue
    static Catalogue builtIn();

    /// Applies an overlay: its entries replace those of the catalogue with the same id, or are appended, and the
    /// entries it disables are removed
    static void apply(Catalogue &catalogue, const Catalogue &overlay);

    /// @return The format of a name of the catalogue (ed269, ed318, aixm, dipul, france-uas, faa-tfr, faa-airspace,
    ///         openaip), Unknown with ok false for other names
    static GeoZoneParser::Format formatFromName(const QString &name, bool &ok);
};
