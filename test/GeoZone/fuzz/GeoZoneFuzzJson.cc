/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFuzz.h"
#include "GeoZoneFormat.h"
#include "GeoZoneParser.h"

/// Parses the input as a JSON document with each format hint, the formats then detect it in different orders and
/// accept empty web service responses, and in another language for the localized texts
extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    GeoZoneFuzz::initialize(argc, argv);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArray input = GeoZoneFuzz::bytes(data, size);
    static const GeoZoneParser::Format hints[] = {
        GeoZoneParser::Format::Unknown,
        GeoZoneParser::Format::ED269,
        GeoZoneParser::Format::ED318,
        GeoZoneParser::Format::FaaTfr,
        GeoZoneParser::Format::Dipul,
        GeoZoneParser::Format::FranceUas,
    };
    for (const GeoZoneParser::Format hint : hints) {
        GeoZoneParser::Result result;
        QString errorString;
        (void) GeoZoneParser::parse(input, result, errorString, hint);
    }

    GeoZoneParser::Result result;
    QString errorString;
    (void) GeoZoneParser::parse(input, result, errorString, GeoZoneParser::Format::Unknown, QStringLiteral("de"));
    return 0;
}
