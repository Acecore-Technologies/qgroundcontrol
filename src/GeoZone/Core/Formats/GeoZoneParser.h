/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QLoggingCategory>

#include "GeoZone.h"
#include "GeoZoneCancelToken.h"

Q_DECLARE_LOGGING_CATEGORY(GeoZoneParserLog)

class GeoZoneFormat;

/// Parses UAS geographical zone files in the EUROCAE ED-269 (JSON) and ED-318 (GeoJSON) formats, and the FAA
/// temporary flight restrictions GeoJSON.
class GeoZoneParser
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneParser)

public:
    /// Version of what the parser produces from a document. Bump it with any change of the parser, the formats or
    /// GeoZoneParserUtils which changes the result for the same bytes: the zones and their fields, texts, issues and
    /// hashes, or the metadata. Stored sources are restored from parse caches of the same version only, the others
    /// are parsed again, see GeoZoneParseCache.
    static constexpr quint16 version = 4;

    enum class Format {
        Unknown,
        ED269,
        ED318,
        FaaTfr,     ///< FAA temporary flight restrictions GeoJSON from tfr.faa.gov
        Dipul,      ///< German geographical zones GeoJSON from the DiPUL WFS
        FranceUas,  ///< French UAS restrictions GeoJSON of the IGN Géoplateforme WFS
        Aixm,       ///< AIXM 5.1 airspaces, like the UK flight restriction zones from NATS
    };

    /// Document a feed's features refer to, like the details of an FAA NOTAM, with its version
    struct Companion {
        QString version;
        QByteArray data;
    };
    using Companions = QHash<QString, Companion>;   ///< By key

    struct Result {
        Format format = Format::Unknown;
        QString title;
        QString provider;       ///< ED-318 dataset metadata
        QDateTime issued;
        QDateTime validFrom;
        QDateTime validTo;
        QList<GeoZone::Data> zones;
        int skippedFeatures = 0;    ///< Features ignored because they have no usable horizontal geometry, see issues
        QByteArray contentHash;     ///< Hash of the features, which stays the same when only metadata changes
        int missingFeatures = 0;    ///< Features of a WFS response the server left out because of its limit
        QMap<QString, int> issues;  ///< Problems found in the zones, with the number of zones having each, including those skipped for their geometry
        Companions companions;      ///< Of a composite document, see composite()
    };  // Fields added here must be serialized, see operator<<

    /// How parse() reads a document
    struct Options {
        Format formatHint = Format::Unknown;    ///< WFS formats like FaaTfr accept an empty feature list from their server as a valid list
        QString language;                       ///< Code of the preferred language of localized texts, English is used otherwise
        Format onlyFormat = Format::Unknown;    ///< Documents in another format fail before their features are read, Unknown for any
        GeoZoneCancelToken cancelToken;         ///< Checked between features, the parse fails once it is canceled
    };

    /// @return A document with its companion documents, which parse() reads like the document with the companions
    static QByteArray composite(const QJsonObject &document, const Companions &companions);

    static bool parseFile(const QString &fileName, Result &result, QString &errorString);
    static bool parse(const QByteArray &bytes, Result &result, QString &errorString, const Options &options);
    /// See Options
    static bool parse(const QByteArray &bytes, Result &result, QString &errorString, Format formatHint = Format::Unknown,
                      const QString &language = QString());

    static QString formatToString(Format format);

private:
    /// Parses with the XML format recognizing the root element, the hinted one first
    static bool _parseXml(const QByteArray &xml, Result &result, QString &errorString, const Options &options);
    /// @return true if the options accept documents in the format
    static bool _acceptsFormat(const GeoZoneFormat *format, const Options &options, QString &errorString);
    /// Checks the geometry of a parsed zone, see GeoZoneParserUtils::_normalizeVolumes, and adds it to the result with
    /// its issues. The issues of a zone left without volumes are counted too.
    /// @return false if the zone has no usable volume
    static bool _addZone(GeoZone::Data &zone, Result &result);
};

/// Binary serialization of parse results, see GeoZoneParseCache
QDataStream &operator<<(QDataStream &stream, const GeoZoneParser::Result &result);
QDataStream &operator>>(QDataStream &stream, GeoZoneParser::Result &result);
