/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneParser.h"
#include "GeoZoneFormat.h"
#include "GeoZoneParserUtils.h"
#include "QGCLoggingCategory.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>
#include <cctype>
#include <cmath>

QGC_LOGGING_CATEGORY(GeoZoneParserLog, "qgc.geozone.geozoneparser")

namespace {

constexpr const char *kCompositeKey = "geozoneComposite";

} // namespace

using namespace GeoZoneParserUtils;

bool GeoZoneParser::parseFile(const QString &fileName, Result &result, QString &errorString)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        errorString = tr("Unable to open file %1: %2").arg(fileName, file.errorString());
        return false;
    }
    return parse(file.readAll(), result, errorString);
}

bool GeoZoneParser::parse(const QByteArray &bytes, Result &result, QString &errorString, Format formatHint, const QString &language)
{
    result = Result();
    errorString.clear();

    const LanguageScope languageScope(language);

    // Several national feeds are published with a UTF-8 byte order mark which QJsonDocument rejects. The bytes of
    // large files are used in place, without copies.
    const qsizetype bomSize = bytes.startsWith("\xEF\xBB\xBF") ? 3 : 0;
    const QByteArray json = QByteArray::fromRawData(bytes.constData() + bomSize, bytes.size() - bomSize);
    const auto firstCharacter = std::find_if_not(json.cbegin(), json.cend(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
    if ((firstCharacter != json.cend()) && (*firstCharacter == '<')) {
        for (const GeoZoneFormat *format : GeoZoneFormat::formats()) {
            if (format->isXml()) {
                return format->parseXml(json, result, errorString);
            }
        }
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        errorString = tr("Invalid JSON: %1 (offset %2)").arg(parseError.errorString()).arg(parseError.offset);
        return false;
    }

    QJsonObject root = document.object();
    // Composite documents carry the companions of the feed's document
    if (root.contains(QLatin1String(kCompositeKey))) {
        const QJsonObject companions = root.value(QStringLiteral("companions")).toObject();
        for (auto it = companions.constBegin(); it != companions.constEnd(); ++it) {
            const QJsonObject companion = it.value().toObject();
            result.companions.insert(it.key(), Companion{ companion.value(QStringLiteral("version")).toString(), companion.value(QStringLiteral("data")).toString().toUtf8() });
        }
        root = root.value(QStringLiteral("document")).toObject();
    }
    const QJsonValue featuresValue = root.value(QStringLiteral("features"));
    if (!document.isObject() || !featuresValue.isArray()) {
        errorString = tr("Not a geozone file. EUROCAE ED-269 and ED-318 files, AIXM 5.1 airspaces and the FAA, DiPUL and IGN GeoJSON are supported.");
        return false;
    }
    const QJsonArray features = featuresValue.toArray();

    // Hashed one feature at a time, serializing all of them at once would take as much memory as the file
    QCryptographicHash contentHash(QCryptographicHash::Sha1);
    for (const QJsonValue &feature : features) {
        contentHash.addData(QJsonDocument(feature.toObject()).toJson(QJsonDocument::Compact));
    }
    for (auto it = result.companions.cbegin(); it != result.companions.cend(); ++it) {
        contentHash.addData(it.key().toUtf8());
        contentHash.addData(it.value().version.toUtf8());
    }
    result.contentHash = contentHash.result();

    // A WFS response the server limited, which misses zones
    const double matched = _number(root.value(QStringLiteral("numberMatched")));
    const double returned = _number(root.value(QStringLiteral("numberReturned")));
    if (!std::isnan(matched) && !std::isnan(returned) && (matched > returned)) {
        result.missingFeatures = static_cast<int>(matched - returned);
    }

    const GeoZoneFormat *format = nullptr;
    for (const GeoZoneFormat *candidate : GeoZoneFormat::formats()) {
        if (!candidate->isXml() && candidate->detect(root, features, formatHint)) {
            format = candidate;
            break;
        }
    }
    if (!format) {
        errorString = tr("The file is not in a supported geozone format. EUROCAE ED-269 and ED-318 files, AIXM 5.1 airspaces and the FAA, DiPUL and IGN GeoJSON are supported.");
        return false;
    }
    result.format = format->format();
    format->readMetadata(root, result);

    // ED-318 names the dataset metadata "metadata", some publishers use "datasetMetadata"
    QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    if (metadata.isEmpty()) {
        metadata = root.value(QStringLiteral("datasetMetadata")).toObject();
    }
    if (!metadata.isEmpty()) {
        result.provider = _text(metadata.value(QStringLiteral("provider")));
        result.issued = _dateTime(metadata.value(QStringLiteral("issued")));
        result.validFrom = _dateTime(metadata.value(QStringLiteral("validFrom")));
        result.validTo = _dateTime(metadata.value(QStringLiteral("validTo")));
    }

    for (const QJsonValue &featureValue : features) {
        GeoZone::Data zone;
        if (format->parseFeatureWithCompanions(featureValue.toObject(), result.companions, zone)) {
            zone.issues.removeDuplicates();
            for (const QString &issue : std::as_const(zone.issues)) {
                result.issues[issue]++;
            }
            result.zones.append(zone);
        } else {
            qCDebug(GeoZoneParserLog) << "Skipping feature without usable geometry" << zone.identifier;
            result.skippedFeatures++;
        }
    }

    // An empty list from a web service means no zones are in the area or in effect
    if (result.zones.isEmpty() && !format->acceptsEmpty()) {
        errorString = tr("No geozones with a supported geometry were found in the %1 file.").arg(formatToString(result.format));
        return false;
    }

    qCDebug(GeoZoneParserLog) << formatToString(result.format) << "zones:" << result.zones.count() << "skipped:" << result.skippedFeatures;

    return true;
}

QByteArray GeoZoneParser::composite(const QJsonObject &document, const Companions &companions)
{
    QJsonObject companionObjects;
    for (auto it = companions.cbegin(); it != companions.cend(); ++it) {
        companionObjects.insert(it.key(), QJsonObject{ { QStringLiteral("version"), it.value().version }, { QStringLiteral("data"), QString::fromUtf8(it.value().data) } });
    }
    return QJsonDocument(QJsonObject{ { QLatin1String(kCompositeKey), 1 }, { QStringLiteral("document"), document }, { QStringLiteral("companions"), companionObjects } }).toJson(QJsonDocument::Compact);
}

QString GeoZoneParser::formatToString(Format format)
{
    const GeoZoneFormat *const found = GeoZoneFormat::find(format);
    return found ? found->name() : tr("Unknown");
}
