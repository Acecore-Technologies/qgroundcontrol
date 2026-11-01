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

#include <QtCore/QCborValue>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QXmlStreamReader>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

Q_LOGGING_CATEGORY(GeoZoneParserLog, "qgc.geozone.geozoneparser")

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

bool GeoZoneParser::_parseXml(const QByteArray &xml, Result &result, QString &errorString, const Options &options)
{
    QXmlStreamReader reader(xml);
    while (!reader.atEnd() && !reader.isStartElement()) {
        (void) reader.readNext();
    }
    if (!reader.isStartElement()) {
        errorString = tr("Invalid XML: %1 (line %2)").arg(reader.errorString()).arg(reader.lineNumber());
        return false;
    }
    const QString rootElement = reader.name().toString();
    const QString namespaceUri = reader.namespaceUri().toString();

    // Web services report errors as XML, web servers as HTML pages
    if (rootElement.endsWith(QLatin1String("ExceptionReport"))) {
        QStringList texts;
        while (!reader.atEnd()) {
            if (reader.readNext() == QXmlStreamReader::StartElement) {
                if ((reader.name() == QLatin1String("ExceptionText")) || (reader.name() == QLatin1String("ServiceException"))) {
                    texts.append(reader.readElementText(QXmlStreamReader::IncludeChildElements).simplified());
                }
            }
        }
        errorString = tr("The server reported an error: %1").arg(texts.isEmpty() ? rootElement : texts.join(QStringLiteral(" ")));
        return false;
    }
    if (rootElement.compare(QLatin1String("html"), Qt::CaseInsensitive) == 0) {
        errorString = tr("The address returned a web page instead of geozone data.");
        return false;
    }

    // The format expected from the address first
    const GeoZoneFormat *hinted = GeoZoneFormat::find(options.formatHint);
    const GeoZoneFormat *format = (hinted && hinted->isXml() && hinted->detectXml(rootElement, namespaceUri)) ? hinted : nullptr;
    for (const GeoZoneFormat *candidate : GeoZoneFormat::formats()) {
        if (!format && candidate->isXml() && candidate->detectXml(rootElement, namespaceUri)) {
            format = candidate;
        }
    }
    if (!format) {
        errorString = tr("Unsupported XML document (%1).").arg(rootElement);
        return false;
    }
    if (!_acceptsFormat(format, options, errorString)) {
        return false;
    }
    if (!format->parseXml(xml, result, errorString)) {
        if (cancelled()) {
            result = Result();
            errorString = tr("The parse was canceled.");
        }
        return false;
    }
    return true;
}

bool GeoZoneParser::_acceptsFormat(const GeoZoneFormat *format, const Options &options, QString &errorString)
{
    if ((options.onlyFormat == Format::Unknown) || (format->format() == options.onlyFormat)) {
        return true;
    }
    errorString = tr("The file is in the %1 format instead of %2.").arg(format->name(), formatToString(options.onlyFormat));
    return false;
}

bool GeoZoneParser::parse(const QByteArray &bytes, Result &result, QString &errorString, Format formatHint, const QString &language)
{
    Options options;
    options.formatHint = formatHint;
    options.language = language;
    return parse(bytes, result, errorString, options);
}

bool GeoZoneParser::parse(const QByteArray &bytes, Result &result, QString &errorString, const Options &options)
{
    result = Result();
    errorString.clear();

    const LanguageScope languageScope(options.language);
    const CancelScope cancelScope(options.cancelToken);

    // Several national feeds are published with a UTF-8 byte order mark which QJsonDocument rejects. The bytes of
    // large files are used in place, without copies.
    const qsizetype bomSize = bytes.startsWith("\xEF\xBB\xBF") ? 3 : 0;
    const QByteArray json = QByteArray::fromRawData(bytes.constData() + bomSize, bytes.size() - bomSize);
    const auto firstCharacter = std::find_if_not(json.cbegin(), json.cend(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
    if ((firstCharacter != json.cend()) && (*firstCharacter == '<')) {
        return _parseXml(json, result, errorString, options);
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

    // A WFS response the server limited, which misses zones
    const double matched = _number(root.value(QStringLiteral("numberMatched")));
    const double returned = _number(root.value(QStringLiteral("numberReturned")));
    if (!std::isnan(matched) && !std::isnan(returned) && (matched > returned)) {
        result.missingFeatures = static_cast<int>((std::min)(matched - returned, static_cast<double>(std::numeric_limits<int>::max())));
    }

    const GeoZoneFormat *const format = GeoZoneFormat::detectJson(root, features, options.formatHint);
    if (!format) {
        errorString = tr("The file is not in a supported geozone format. EUROCAE ED-269 and ED-318 files, AIXM 5.1 airspaces and the FAA, DiPUL and IGN GeoJSON are supported.");
        return false;
    }
    if (!_acceptsFormat(format, options, errorString)) {
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

    // Features are hashed in their binary CBOR form, which is much faster to write than JSON text and like it doesn't
    // depend on the order of the keys and the whitespace of the document. The hashes are only compared within a
    // session and with parse caches of the same parser version, they aren't persisted otherwise.
    QCryptographicHash contentHash(QCryptographicHash::Sha1);
    for (const QJsonValue &featureValue : features) {
        if (cancelled()) {
            result = Result();
            errorString = tr("The parse was canceled.");
            return false;
        }
        GeoZone::Data zone;
        zone.contentHash = QCryptographicHash::hash(QCborValue::fromJsonValue(featureValue).toCbor(), QCryptographicHash::Sha1);
        contentHash.addData(zone.contentHash);
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

    // In key order, the order of a hash differs between runs
    QStringList companionKeys = result.companions.keys();
    companionKeys.sort();
    for (const QString &key : std::as_const(companionKeys)) {
        contentHash.addData(key.toUtf8());
        contentHash.addData(result.companions.value(key).version.toUtf8());
    }
    result.contentHash = contentHash.result();

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
