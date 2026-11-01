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

/// Parses the input as an XML document, detected from its root element like web service responses and AIXM files,
/// and with each XML format directly, so that inputs whose root element changed still reach the format's parser
extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    GeoZoneFuzz::initialize(argc, argv);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArray input = GeoZoneFuzz::bytes(data, size);
    for (const GeoZoneParser::Format hint : { GeoZoneParser::Format::Unknown, GeoZoneParser::Format::Aixm }) {
        GeoZoneParser::Result result;
        QString errorString;
        (void) GeoZoneParser::parse(input, result, errorString, hint);
    }

    for (const GeoZoneFormat *format : GeoZoneFormat::formats()) {
        if (format->isXml()) {
            GeoZoneParser::Result result;
            QString errorString;
            (void) format->parseXml(input, result, errorString);
        }
    }
    return 0;
}
