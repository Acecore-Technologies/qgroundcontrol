/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZone.h"
#include "GeoZoneDownloader.h"
#include "GeoZoneFeedCatalogue.h"
#include "GeoZoneInputFiles.h"
#include "GeoZoneLoader.h"
#include "GeoZoneParser.h"
#include "GeoZoneText.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QLoggingCategory>
#include <QtCore/QMap>
#include <QtCore/QTextStream>

#include <cmath>

// geozone-validate: checks geozone files and zip packages like QGroundControl loads them, without the application,
// and prints their format, their zones, the problems found in them and a summary of their altitude limits and
// schedules. See the Testing section of docs/en/qgc-dev-guide/geozones.md.

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

/// "3 polygons", "1 polygon"
QString count(qsizetype n, const QString &singular, const QString &plural)
{
    return QStringLiteral("%1 %2").arg(n).arg((n == 1) ? singular : plural);
}

/// The counts which aren't 0, joined with commas, or none
QString counts(const QList<std::pair<qsizetype, QString>> &items)
{
    QStringList parts;
    for (const auto &[n, text] : items) {
        if (n > 0) {
            parts.append(QStringLiteral("%1 %2").arg(n).arg(text));
        }
    }
    return parts.isEmpty() ? QStringLiteral("none") : parts.join(QStringLiteral(", "));
}

QString referenceText(GeoZone::VerticalReference reference)
{
    switch (reference) {
    case GeoZone::VerticalReference::Ground:
        return QStringLiteral("above ground");
    case GeoZone::VerticalReference::MeanSeaLevel:
        return QStringLiteral("above mean sea level");
    case GeoZone::VerticalReference::Ellipsoid:
        return QStringLiteral("above the WGS84 ellipsoid");
    case GeoZone::VerticalReference::FlightLevel:
        return QStringLiteral("flight level");
    case GeoZone::VerticalReference::Unknown:
        break;
    }
    return QStringLiteral("in an unknown reference");
}

struct Options {
    GeoZoneParser::Format formatHint = GeoZoneParser::Format::Unknown;
    QString language;
    bool listZones = false;
};

/// Problems of a file which aren't errors
struct Findings {
    int warnings = 0;
};

void printGeometry(const QList<GeoZone::Data> &zones)
{
    qsizetype polygons = 0;
    qsizetype holes = 0;
    qsizetype circles = 0;
    qsizetype vertices = 0;
    for (const GeoZone::Data &zone : zones) {
        for (const GeoZone::Volume &volume : zone.volumes) {
            if (volume.isCircle) {
                circles++;
                continue;
            }
            polygons++;
            holes += qMax<qsizetype>(0, volume.rings.count() - 1);
            for (const GeoZone::Ring &ring : volume.rings) {
                vertices += ring.count();
            }
        }
    }
    out() << "  Geometry: " << counts({ { polygons, (polygons == 1) ? QStringLiteral("polygon") : QStringLiteral("polygons") },
                                        { holes, (holes == 1) ? QStringLiteral("hole") : QStringLiteral("holes") },
                                        { circles, (circles == 1) ? QStringLiteral("circle") : QStringLiteral("circles") } })
          << ", " << count(vertices, QStringLiteral("vertex"), QStringLiteral("vertices")) << Qt::endl;
}

void printAltitudes(const QList<GeoZone::Data> &zones)
{
    QMap<GeoZone::VerticalReference, qsizetype> limits;
    qsizetype unlimited = 0;
    qsizetype feet = 0;
    qsizetype volumes = 0;
    for (const GeoZone::Data &zone : zones) {
        for (const GeoZone::Volume &volume : zone.volumes) {
            volumes++;
            const bool lower = !std::isnan(volume.lowerLimit);
            const bool upper = !std::isnan(volume.upperLimit);
            if (lower) {
                limits[volume.lowerReference]++;
            }
            if (upper) {
                limits[volume.upperReference]++;
            }
            if (!lower && !upper) {
                unlimited++;
            }
            if ((lower || upper) && volume.feet) {
                feet++;
            }
        }
    }
    QList<std::pair<qsizetype, QString>> items;
    for (const GeoZone::VerticalReference reference : { GeoZone::VerticalReference::Ground, GeoZone::VerticalReference::MeanSeaLevel,
                                                        GeoZone::VerticalReference::Ellipsoid, GeoZone::VerticalReference::FlightLevel,
                                                        GeoZone::VerticalReference::Unknown }) {
        items.append({ limits.value(reference), referenceText(reference) });
    }
    out() << "  Altitude limits: " << counts(items) << " (" << count(volumes, QStringLiteral("volume"), QStringLiteral("volumes")) << ": "
          << unlimited << " without limits, " << feet << " in feet)" << Qt::endl;
}

void printSchedules(const QList<GeoZone::Data> &zones)
{
    qsizetype always = 0;
    qsizetype permanent = 0;
    qsizetype dated = 0;
    qsizetype daily = 0;
    qsizetype events = 0;
    qsizetype timeZoneAssumed = 0;
    qsizetype assumed = 0;
    for (const GeoZone::Data &data : zones) {
        if (data.applicability.isEmpty()) {
            always++;
        }
        bool isPermanent = false;
        bool isDated = false;
        bool isDaily = false;
        bool hasEvents = false;
        bool hasAssumedTimeZone = false;
        for (const GeoZone::TimePeriod &period : data.applicability) {
            isPermanent = isPermanent || period.permanent;
            isDated = isDated || period.start.isValid() || period.end.isValid();
            isDaily = isDaily || !period.schedule.isEmpty();
            for (const GeoZone::DailyPeriod &dailyPeriod : period.schedule) {
                hasEvents = hasEvents || !dailyPeriod.startEvent.isEmpty() || !dailyPeriod.endEvent.isEmpty();
                hasAssumedTimeZone = hasAssumedTimeZone || dailyPeriod.timeZoneAssumed;
            }
        }
        permanent += isPermanent ? 1 : 0;
        dated += isDated ? 1 : 0;
        daily += isDaily ? 1 : 0;
        events += hasEvents ? 1 : 0;
        timeZoneAssumed += hasAssumedTimeZone ? 1 : 0;
        assumed += GeoZone(data).scheduleAssumed() ? 1 : 0;
    }
    out() << "  Schedules: " << counts({ { always, QStringLiteral("always active") }, { permanent, QStringLiteral("permanent") },
                                         { dated, QStringLiteral("with dates") }, { daily, QStringLiteral("with daily periods") },
                                         { events, QStringLiteral("with daylight events") },
                                         { timeZoneAssumed, QStringLiteral("with the time zone assumed") },
                                         { assumed, QStringLiteral("whose activity can't be computed, taken as always active") } })
          << Qt::endl;
}

/// Prints the problems found in the zones, with the first zones having each
void printIssues(const GeoZoneParser::Result &result, Findings &findings)
{
    if (result.issues.isEmpty()) {
        return;
    }
    QMap<QString, QStringList> zonesByIssue;
    for (const GeoZone::Data &zone : result.zones) {
        for (const QString &issue : zone.issues) {
            zonesByIssue[issue].append(zone.identifier.isEmpty() ? zone.name : zone.identifier);
        }
    }
    out() << "  Warnings:" << Qt::endl;
    for (auto it = result.issues.cbegin(); it != result.issues.cend(); ++it) {
        findings.warnings++;
        out() << "    " << it.key() << " (" << count(it.value(), QStringLiteral("zone"), QStringLiteral("zones"));
        const QStringList identifiers = zonesByIssue.value(it.key());
        if (!identifiers.isEmpty()) {
            constexpr qsizetype maxListed = 5;
            out() << ": " << identifiers.mid(0, maxListed).join(QStringLiteral(", ")) << ((identifiers.count() > maxListed) ? QStringLiteral(", ...") : QString());
        }
        out() << ")" << Qt::endl;
    }
}

/// The lines of a text of a zone on one line
QString oneLine(const QString &text)
{
    QStringList lines;
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        lines.append(line.trimmed());
    }
    return lines.join(QStringLiteral("; "));
}

void printZones(const QList<GeoZone::Data> &zones)
{
    out() << "  Zones:" << Qt::endl;
    for (const GeoZone::Data &data : zones) {
        const GeoZone zone(data);
        out() << "    " << zone.identifier() << ((zone.name().isEmpty() || (zone.name() == zone.identifier())) ? QString() : QStringLiteral(" (%1)").arg(zone.name()))
              << ": " << GeoZoneText::restriction(zone.restriction());
        for (const QString &text : { GeoZoneText::verticalLimits(zone.data()), GeoZoneSchedule::text(zone.data().applicability) }) {
            if (!oneLine(text).isEmpty()) {
                out() << ", " << oneLine(text);
            }
        }
        out() << Qt::endl;
        for (const QString &issue : data.issues) {
            out() << "      " << issue << Qt::endl;
        }
    }
}

/// @return false on errors
bool validate(const QString &fileName, const Options &options, Findings &findings)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        out() << fileName << ": error: " << file.errorString() << Qt::endl;
        return false;
    }
    if (file.size() > GeoZoneDownloader::defaultMaxBytes) {
        out() << fileName << ": error: larger than " << (GeoZoneDownloader::defaultMaxBytes / (1024 * 1024)) << " MB, which QGroundControl doesn't load" << Qt::endl;
        return false;
    }
    GeoZoneParser::Options parseOptions;
    parseOptions.formatHint = options.formatHint;
    parseOptions.language = options.language;
    GeoZoneParser::Result result;
    QString errorString;
    if (!GeoZoneLoader::parse(file.readAll(), result, errorString, parseOptions)) {
        out() << fileName << ": error: " << errorString << Qt::endl;
        return false;
    }

    out() << fileName << ": " << GeoZoneParser::formatToString(result.format) << ", " << count(result.zones.count(), QStringLiteral("zone"), QStringLiteral("zones"))
          << Qt::endl;
    if (!result.title.isEmpty()) {
        out() << "  Title: " << result.title << Qt::endl;
    }
    if (!result.provider.isEmpty()) {
        out() << "  Provider: " << result.provider << Qt::endl;
    }
    if (result.issued.isValid()) {
        out() << "  Issued: " << result.issued.toString(Qt::ISODate) << Qt::endl;
    }
    if (result.validFrom.isValid() || result.validTo.isValid()) {
        out() << "  Valid: " << (result.validFrom.isValid() ? result.validFrom.toString(Qt::ISODate) : QStringLiteral("...")) << " to "
              << (result.validTo.isValid() ? result.validTo.toString(Qt::ISODate) : QStringLiteral("...")) << Qt::endl;
    }

    QMap<GeoZone::Restriction, qsizetype> restrictions;
    for (const GeoZone::Data &zone : std::as_const(result.zones)) {
        restrictions[zone.restriction]++;
    }
    QList<std::pair<qsizetype, QString>> restrictionCounts;
    for (auto it = restrictions.cbegin(); it != restrictions.cend(); ++it) {
        restrictionCounts.append({ it.value(), GeoZoneText::restriction(it.key()) });
    }
    out() << "  Restrictions: " << counts(restrictionCounts) << Qt::endl;
    printGeometry(result.zones);
    printAltitudes(result.zones);
    printSchedules(result.zones);

    if (result.skippedFeatures > 0) {
        findings.warnings++;
        out() << "  Skipped: " << count(result.skippedFeatures, QStringLiteral("feature"), QStringLiteral("features")) << " without a usable geometry" << Qt::endl;
    }
    if (result.missingFeatures > 0) {
        findings.warnings++;
        out() << "  Missing: " << count(result.missingFeatures, QStringLiteral("feature"), QStringLiteral("features"))
              << " the server left out because of its limit" << Qt::endl;
    }
    printIssues(result, findings);
    if (options.listZones) {
        printZones(result.zones);
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("geozone-validate"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Checks geozone files and zip packages like QGroundControl loads them, and prints their format, zones, problems,\n"
        "altitude limits and schedules. Exits with 1 when a file can't be loaded, or with --strict has warnings."));
    parser.addHelpOption();
    const QCommandLineOption formatOption(QStringLiteral("format"),
                                          QStringLiteral("Format expected, like for the URL of a feed: a format name of the feed catalogue, like ed269, ed318, aixm, "
                                                         "dipul, france-uas, faa-tfr, faa-airspace or openaip."),
                                          QStringLiteral("format"));
    const QCommandLineOption languageOption(QStringLiteral("language"), QStringLiteral("Preferred language of localized texts, like de."), QStringLiteral("code"));
    const QCommandLineOption zonesOption(QStringLiteral("zones"), QStringLiteral("List the zones with their limits, schedules and problems."));
    const QCommandLineOption strictOption(QStringLiteral("strict"), QStringLiteral("Fail on warnings too: problems found in zones, skipped or missing features."));
    const QCommandLineOption verboseOption(QStringLiteral("verbose"), QStringLiteral("Show the log of the parsers."));
    parser.addOptions({ formatOption, languageOption, zonesOption, strictOption, verboseOption });
    parser.addPositionalArgument(QStringLiteral("paths"), QStringLiteral("Files, or directories whose files are checked."), QStringLiteral("<path>..."));
    parser.process(app);

    if (!parser.isSet(verboseOption)) {
        QLoggingCategory::setFilterRules(QStringLiteral("qgc.*=false"));
    }

    Options options;
    options.language = parser.value(languageOption);
    options.listZones = parser.isSet(zonesOption);
    if (parser.isSet(formatOption)) {
        // The names feeds and services have in the feed catalogue, so that a format is never missing here
        bool known = false;
        options.formatHint = GeoZoneFeedCatalogue::formatFromName(parser.value(formatOption), known);
        if (!known) {
            out() << "Unknown format " << parser.value(formatOption) << ": the formats are those of the feed catalogue, see --help" << Qt::endl;
            return 2;
        }
    }

    const QStringList files = GeoZoneInputFiles::collect(parser.positionalArguments());
    if (files.isEmpty()) {
        parser.showHelp(2);
    }

    int errors = 0;
    Findings findings;
    for (const QString &fileName : files) {
        errors += validate(fileName, options, findings) ? 0 : 1;
    }
    const bool failed = (errors > 0) || (parser.isSet(strictOption) && (findings.warnings > 0));
    out() << count(files.count(), QStringLiteral("file"), QStringLiteral("files")) << ", " << count(errors, QStringLiteral("error"), QStringLiteral("errors"))
          << ", " << count(findings.warnings, QStringLiteral("warning"), QStringLiteral("warnings"))
          << ((parser.isSet(strictOption) && (findings.warnings > 0)) ? QStringLiteral(", which fail with --strict") : QString()) << Qt::endl;
    return failed ? 1 : 0;
}
