/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

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
        FaaTfr      ///< FAA temporary flight restrictions GeoJSON from tfr.faa.gov
    };

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
    };

    static bool parseFile(const QString &fileName, Result &result, QString &errorString);
    /// @param formatHint FaaTfr accepts an empty feature list from the FAA as a valid TFR list
    static bool parse(const QByteArray &bytes, Result &result, QString &errorString, Format formatHint = Format::Unknown);

    static QString formatToString(Format format);
};
