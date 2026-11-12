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
    // No hint and that of every JSON format registered, so that a new format is fuzzed too
    static const QList<GeoZoneParser::Format> hints = []() {
        QList<GeoZoneParser::Format> formats = { GeoZoneParser::Format::Unknown };
        for (const GeoZoneFormat *format : GeoZoneFormat::formats()) {
            if (!format->isXml()) {
                formats.append(format->format());
            }
        }
        return formats;
    }();
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
