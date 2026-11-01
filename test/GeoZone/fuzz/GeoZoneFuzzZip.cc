/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFuzz.h"
#include "GeoZoneLoader.h"
#include "GeoZoneZip.h"

namespace {

// Smaller than the limit of downloads, so that archives inflating to it don't slow the fuzzing down
constexpr qint64 kMaxBytes = 4 * 1024 * 1024;

} // namespace

/// Reads the input as a zip archive like the loader: the largest file, all the files, and the package parsed
/// together
extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    GeoZoneFuzz::initialize(argc, argv);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArray input = GeoZoneFuzz::bytes(data, size);
    const QStringList suffixes = { QStringLiteral(".xml"), QStringLiteral(".json"), QStringLiteral(".geojson") };
    QString errorString;
    (void) GeoZoneZip::largestFile(input, suffixes, kMaxBytes, errorString);
    (void) GeoZoneZip::files(input, suffixes, kMaxBytes, errorString);

    GeoZoneParser::Result result;
    (void) GeoZoneLoader::parse(input, result, errorString, GeoZoneParser::Format::Unknown, QString(), kMaxBytes);
    return 0;
}
