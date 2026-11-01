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

Q_DECLARE_LOGGING_CATEGORY(GeoZoneParserLog)

/// Parses UAS geographical zone files in the EUROCAE ED-269 (JSON) and ED-318 (GeoJSON) formats, and the FAA
/// temporary flight restrictions GeoJSON.
class GeoZoneParser
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneParser)

public:
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
        int skippedFeatures = 0;    ///< Features ignored because they have no usable horizontal geometry
        QByteArray contentHash;     ///< Hash of the features, which stays the same when only metadata changes
        int missingFeatures = 0;    ///< Features of a WFS response the server left out because of its limit
        QMap<QString, int> issues;  ///< Problems found in the zones, with the number of zones having each
        Companions companions;      ///< Of a composite document, see composite()
    };

    /// @return A document with its companion documents, which parse() reads like the document with the companions
    static QByteArray composite(const QJsonObject &document, const Companions &companions);

    static bool parseFile(const QString &fileName, Result &result, QString &errorString);
    /// @param formatHint WFS formats like FaaTfr accept an empty feature list from their server as a valid list
    /// @param language Code of the preferred language of localized texts, English is used otherwise
    static bool parse(const QByteArray &bytes, Result &result, QString &errorString, Format formatHint = Format::Unknown,
                      const QString &language = QString());

    static QString formatToString(Format format);

private:
    /// Parses with the XML format recognizing the root element, the hinted one first
    static bool _parseXml(const QByteArray &xml, Result &result, QString &errorString, Format formatHint);
};
