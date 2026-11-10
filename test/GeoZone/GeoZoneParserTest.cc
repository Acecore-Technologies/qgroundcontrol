/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneParserTest.h"
#include "GeoZoneTestHelpers.h"
#include "GeoZoneEd318Writer.h"
#include "GeoZoneFeedCatalogue.h"
#include "GeoZoneFormat.h"
#include "GeoZoneParserUtils.h"
#include "GeoZoneText.h"
#include "GeoZoneZip.h"

#include <zlib.h>

using namespace GeoZoneTestHelpers;

namespace {

/// @return The first difference between the results, compared field by field with NaN equal to NaN, empty without
QString _resultDifference(const GeoZoneParser::Result &a, const GeoZoneParser::Result &b)
{
    const auto sameDouble = [](double x, double y) { return (qIsNaN(x) && qIsNaN(y)) || (x == y); };
    const auto sameDateTime = [](const QDateTime &x, const QDateTime &y) {
        return (x == y) && (x.isValid() == y.isValid()) && (x.timeRepresentation() == y.timeRepresentation());
    };
    const auto sameCoordinate = [&sameDouble](const QGeoCoordinate &x, const QGeoCoordinate &y) {
        return sameDouble(x.latitude(), y.latitude()) && sameDouble(x.longitude(), y.longitude()) && sameDouble(x.altitude(), y.altitude());
    };
    const auto sameRing = [](const GeoZone::Ring &x, const GeoZone::Ring &y) {
        return std::equal(x.cbegin(), x.cend(), y.cbegin(), y.cend(), [](const QPointF &p, const QPointF &q) { return (p.x() == q.x()) && (p.y() == q.y()); });
    };
    const auto sameVolume = [&](const GeoZone::Volume &x, const GeoZone::Volume &y) {
        return (x.isCircle == y.isCircle) && std::equal(x.rings.cbegin(), x.rings.cend(), y.rings.cbegin(), y.rings.cend(), sameRing) &&
               sameCoordinate(x.center, y.center) && sameDouble(x.radius, y.radius) && sameDouble(x.lowerLimit, y.lowerLimit) &&
               sameDouble(x.upperLimit, y.upperLimit) && (x.lowerReference == y.lowerReference) && (x.upperReference == y.upperReference) &&
               (x.lowerReferenceCode == y.lowerReferenceCode) && (x.upperReferenceCode == y.upperReferenceCode) && (x.feet == y.feet);
    };
    const auto sameDailyPeriod = [](const GeoZone::DailyPeriod &x, const GeoZone::DailyPeriod &y) {
        return (x.days == y.days) && (x.startTime == y.startTime) && (x.endTime == y.endTime) && (x.utcOffsetSecs == y.utcOffsetSecs) &&
               (x.timeZone == y.timeZone) && (x.timeZoneAssumed == y.timeZoneAssumed) && (x.startEvent == y.startEvent) && (x.endEvent == y.endEvent);
    };
    const auto sameTimePeriod = [&](const GeoZone::TimePeriod &x, const GeoZone::TimePeriod &y) {
        return (x.permanent == y.permanent) && sameDateTime(x.start, y.start) && sameDateTime(x.end, y.end) &&
               std::equal(x.schedule.cbegin(), x.schedule.cend(), y.schedule.cbegin(), y.schedule.cend(), sameDailyPeriod);
    };

    if ((a.format != b.format) || (a.title != b.title) || (a.provider != b.provider)) {
        return QStringLiteral("format, title or provider");
    }
    if (!sameDateTime(a.issued, b.issued) || !sameDateTime(a.validFrom, b.validFrom) || !sameDateTime(a.validTo, b.validTo)) {
        return QStringLiteral("dates");
    }
    if ((a.skippedFeatures != b.skippedFeatures) || (a.missingFeatures != b.missingFeatures) || (a.contentHash != b.contentHash) || (a.issues != b.issues)) {
        return QStringLiteral("counts, hash or issues");
    }
    QStringList companionKeys = a.companions.keys();
    QStringList otherCompanionKeys = b.companions.keys();
    companionKeys.sort();
    otherCompanionKeys.sort();
    if (companionKeys != otherCompanionKeys) {
        return QStringLiteral("companions");
    }
    for (auto it = a.companions.cbegin(); it != a.companions.cend(); ++it) {
        if ((it->version != b.companions[it.key()].version) || (it->data != b.companions[it.key()].data)) {
            return QStringLiteral("companion %1").arg(it.key());
        }
    }
    if (a.zones.count() != b.zones.count()) {
        return QStringLiteral("zone count");
    }
    for (qsizetype i = 0; i < a.zones.count(); i++) {
        const GeoZone::Data &x = a.zones[i];
        const GeoZone::Data &y = b.zones[i];
        if ((x.identifier != y.identifier) || (x.country != y.country) || (x.name != y.name) || (x.restrictionCode != y.restrictionCode) ||
            (x.restriction != y.restriction) || (x.reasons != y.reasons) || (x.otherReasonInfo != y.otherReasonInfo) ||
            (x.restrictionConditions != y.restrictionConditions) || (x.message != y.message) || (x.authorities != y.authorities) ||
            (x.uSpaceClass != y.uSpaceClass) || (x.regulationExemption != y.regulationExemption) || (x.extendedProperties != y.extendedProperties) ||
            (x.issues != y.issues) || (x.contentHash != y.contentHash) || (x.notificationAuthority != y.notificationAuthority) ||
            (x.notificationLeadSecs != y.notificationLeadSecs)) {
            return QStringLiteral("texts of zone %1").arg(i);
        }
        if (!std::equal(x.applicability.cbegin(), x.applicability.cend(), y.applicability.cbegin(), y.applicability.cend(), sameTimePeriod)) {
            return QStringLiteral("applicability of zone %1").arg(i);
        }
        if (!std::equal(x.volumes.cbegin(), x.volumes.cend(), y.volumes.cbegin(), y.volumes.cend(), sameVolume)) {
            return QStringLiteral("volumes of zone %1").arg(i);
        }
    }
    return QString();
}

/// Result with every field set, including values the fixtures don't have
GeoZoneParser::Result _syntheticResult()
{
    GeoZoneParser::Result result;
    result.format = GeoZoneParser::Format::FaaTfr;
    result.title = QStringLiteral("Synthetic");
    result.provider = QStringLiteral("Provider \u00e9\u00df");
    result.issued = _utc(QDate(2026, 8, 13), QTime(6, 30));
    result.validFrom = QDateTime(QDate(2026, 8, 14), QTime(0, 0), QTimeZone::fromSecondsAheadOfUtc(7200));
    result.validTo = _utc(QDate(2027, 8, 14), QTime(0, 0));
    result.skippedFeatures = 3;
    result.missingFeatures = 7;
    result.contentHash = QByteArray(20, '\x5a');
    result.issues.insert(QStringLiteral("Unknown unit FL"), 2);
    result.issues.insert(QStringLiteral("Unknown day XYZ"), 1);
    result.companions.insert(QStringLiteral("6/1234"), GeoZoneParser::Companion{ QStringLiteral("v2"), QByteArray("<xml>\0binary</xml>", 18) });
    result.companions.insert(QStringLiteral("6/0001"), GeoZoneParser::Companion{ QString(), QByteArray() });

    GeoZone::Data zone;
    zone.identifier = QStringLiteral("SYN-1");
    zone.country = QStringLiteral("CHE");
    zone.name = QStringLiteral("Synthetic zone");
    zone.restrictionCode = QStringLiteral("REQ_AUTHORISATION");
    zone.restriction = GeoZone::ReqAuthorisation;
    zone.reasons = { QStringLiteral("AIR_TRAFFIC"), QStringLiteral("OTHER") };
    zone.otherReasonInfo = QStringLiteral("Other");
    zone.restrictionConditions = QStringLiteral("Conditions");
    zone.message = QStringLiteral("Message\nwith lines");
    zone.authorities = { QStringLiteral("Authority, +41 0000") };
    zone.notificationAuthority = QStringLiteral("Tower");
    zone.notificationLeadSecs = 2 * 24 * 3600;
    GeoZone::DailyPeriod daily;
    daily.days = { Qt::Monday, Qt::Saturday };
    daily.startTime = QTime(6, 0);
    daily.utcOffsetSecs = 3600;
    daily.endEvent = QStringLiteral("SS");
    GeoZone::DailyPeriod events;
    events.startEvent = QStringLiteral("BMCT");
    events.endEvent = QStringLiteral("EECT");
    GeoZone::DailyPeriod local;
    local.startTime = QTime(8, 0);
    local.endTime = QTime(18, 0);
    local.timeZone = QTimeZone("Europe/Zurich");
    local.timeZoneAssumed = true;
    zone.applicability = { GeoZone::TimePeriod{ false, _utc(QDate(2026, 9, 1), QTime(0, 0)), _utc(QDate(2026, 10, 1), QTime(0, 0)), { daily, events, local } },
                           GeoZone::TimePeriod{ true, QDateTime(), QDateTime(), {} } };
    GeoZone::Volume polygon;
    polygon.rings = { { QPointF(8.1, 47.1), QPointF(8.2, 47.1), QPointF(8.2, 47.2) }, { QPointF(8.15, 47.12), QPointF(8.16, 47.12), QPointF(8.16, 47.13) } };
    polygon.lowerLimit = 0;
    polygon.upperLimit = 400;
    polygon.feet = true;
    polygon.setReferences(QStringLiteral("AGL"), QStringLiteral("FL"));
    GeoZone::Volume circle;
    circle.isCircle = true;
    // Continuous across the antimeridian
    circle.center.setLatitude(47.5);
    circle.center.setLongitude(181.25);
    circle.center.setAltitude(432.5);
    circle.radius = 1234.5;
    circle.setReferences(QStringLiteral("WGS84"), QStringLiteral("AMSL"));
    zone.volumes = { polygon, circle };
    zone.uSpaceClass = QStringLiteral("EUROCONTROL");
    zone.regulationExemption = QStringLiteral("YES");
    zone.extendedProperties = { QStringLiteral("a: 1"), QStringLiteral("b: 2") };
    zone.issues = { QStringLiteral("Unknown unit FL") };
    zone.contentHash = QByteArray(20, '\x01');
    result.zones = { zone, GeoZone::Data() };
    return result;
}

/// Converts to the type of any field, see _fieldCount
struct AnyField {
    template <typename T>
    operator T &() const;
};

/// @return Number of fields of an aggregate: the most initializers it takes
template <typename T, typename... Fields>
constexpr int _fieldCount()
{
    if constexpr (requires { T{ Fields{}..., AnyField{} }; }) {
        return _fieldCount<T, Fields..., AnyField>();
    } else {
        return sizeof...(Fields);
    }
}

/// GeoZoneParser::version the hashes of the fixtures' results in _testParseFixtures_data were recorded for
constexpr quint16 kResultHashVersion = 12;

/// @return Hash of a serialized result, with the vertices rounded to 1e-7 degrees, which the differences of the math
///         libraries computing arcs and geodesics don't change
QString _resultHash(GeoZoneParser::Result result)
{
    const auto round = [](double value) { return (std::round(value * 1e7) / 1e7) + 0.0; };
    for (GeoZone::Data &zone : result.zones) {
        for (GeoZone::Volume &volume : zone.volumes) {
            for (GeoZone::Ring &ring : volume.rings) {
                for (QPointF &vertex : ring) {
                    vertex = QPointF(round(vertex.x()), round(vertex.y()));
                }
            }
        }
    }
    QByteArray serialized;
    QDataStream stream(&serialized, QIODevice::WriteOnly);
    stream.setVersion(GeoZoneParseCache::streamVersion);
    stream << result;
    return QString::fromLatin1(QCryptographicHash::hash(serialized, QCryptographicHash::Sha256).toHex().left(16));
}

/// Canceled by cancelOnSkippedFeature
const GeoZoneCancelToken *s_cancelOnSkip = nullptr;

/// Cancels s_cancelOnSkip while the parser logs a feature it skips, which happens between features
void cancelOnSkippedFeature(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (s_cancelOnSkip && message.contains(QStringLiteral("Skipping feature"))) {
        s_cancelOnSkip->cancel();
    }
}


/// Replaces a random value in the JSON tree with one of another type or an extreme value
QJsonValue _mutateJson(const QJsonValue &value, QRandomGenerator &random, int depth = 0)
{
    const auto randomValue = [&random]() -> QJsonValue {
        switch (random.bounded(14)) {
        case 0:  return QJsonValue();
        case 1:  return QJsonValue(QJsonValue::Undefined);
        case 2:  return QString();
        case 3:  return QStringLiteral("NaN");
        case 4:  return -1;
        case 5:  return 0;
        case 6:  return 1e308;
        case 7:  return -1e308;
        case 8:  return QJsonArray();
        case 9:  return QJsonObject();
        case 10: return QJsonArray{ QJsonArray{ 1e300, -1e300 }, QJsonArray{ 0, 0 } };
        case 11: return QJsonArray{ QJsonArray{ QJsonArray{ 8, 47 }, QJsonArray{ 8, 47 }, QJsonArray{ 8, 47 } } };
        case 12: return QStringLiteral("2026-13-45T99:99:99Z");
        default: return QString(1000, QLatin1Char('x'));
        }
    };

    if (value.isObject() && (depth < 12)) {
        QJsonObject object = value.toObject();
        if (object.isEmpty() || (random.bounded(8) == 0)) {
            return randomValue();
        }
        const QStringList keys = object.keys();
        const QString key = keys[random.bounded(keys.count())];
        if (random.bounded(6) == 0) {
            object.remove(key);
        } else {
            object[key] = _mutateJson(object[key], random, depth + 1);
        }
        return object;
    }
    if (value.isArray() && (depth < 12)) {
        QJsonArray array = value.toArray();
        if (array.isEmpty() || (random.bounded(8) == 0)) {
            return randomValue();
        }
        const int index = random.bounded(static_cast<int>(array.count()));
        switch (random.bounded(6)) {
        case 0:
            array.removeAt(index);
            break;
        case 1:
            array.append(array[index]);
            break;
        default:
            array[index] = _mutateJson(array[index], random, depth + 1);
            break;
        }
        return array;
    }
    return randomValue();
}


} // namespace

void GeoZoneParserTest::_testParseFixtures_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<int>("formatHint");
    QTest::addColumn<int>("format");
    QTest::addColumn<int>("zones");
    QTest::addColumn<int>("skippedFeatures");
    QTest::addColumn<int>("missingFeatures");
    QTest::addColumn<QString>("resultHash");

    using Format = GeoZoneParser::Format;
    const auto row = [](const char *fileName, Format formatHint, Format format, int zones, int skippedFeatures, int missingFeatures, const char *resultHash) {
        QTest::newRow(fileName) << QString::fromLatin1(fileName) << static_cast<int>(formatHint) << static_cast<int>(format) << zones << skippedFeatures
                                << missingFeatures << QString::fromLatin1(resultHash);
    };
    // The ED-269 and ED-318 test files each have a feature without a supported geometry, the IGN response was limited
    row("ch_zurich_ed269.json", Format::Unknown, Format::ED269, 8, 0, 0, "9229fa5a8ab6ffc6");
    row("dipul_test.json", Format::Dipul, Format::Dipul, 4, 0, 0, "e2d4b0d264592f37");
    row("ed269_test.json", Format::Unknown, Format::ED269, 3, 1, 0, "3c0d3f716763a061");
    row("ed318_test.json", Format::Unknown, Format::ED318, 3, 1, 0, "bb11ebfc2907b643");
    row("es_enaire_ed318_test.json", Format::ED318, Format::ED318, 3, 0, 0, "1ba7cfe75dd69cce");
    row("es_servais_ed318_test.json", Format::ED318, Format::ED318, 2, 0, 0, "4ca8ded0a0427a73");
    row("at_ed269_test.json", Format::ED269, Format::ED269, 3, 0, 0, "5a0999bafe784557");
    row("faa_airspace_test.json", Format::Unknown, Format::FaaAirspace, 6, 1, 0, "60bb56a4e18bf1b8");
    row("faa_tfr_test.json", Format::Unknown, Format::FaaTfr, 2, 0, 0, "f3ce5eb69ad863e7");
    row("france_uas_test.json", Format::Unknown, Format::FranceUas, 2, 0, 8, "ad45dfe3891b4d6c");
    row("openaip_test.json", Format::Unknown, Format::OpenAip, 6, 2, 0, "7c019bd11e66e495");
    row("uk_aixm_test.xml", Format::Unknown, Format::Aixm, 3, 0, 0, "1cc47a3d61d437ed");
}

void GeoZoneParserTest::_testParseFixtures()
{
    QFETCH(QString, fileName);
    QFETCH(int, formatHint);
    QFETCH(int, format);
    QFETCH(int, zones);
    QFETCH(int, skippedFeatures);
    QFETCH(int, missingFeatures);
    QFETCH(QString, resultHash);

    // As text, Windows checkouts have other line endings, which change the hash of AIXM documents
    QFile file(QStringLiteral(":/unittest/") + fileName);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray bytes = file.readAll();
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(bytes, result, errorString, static_cast<GeoZoneParser::Format>(formatHint)), qPrintable(errorString));
    QCOMPARE(static_cast<int>(result.format), format);
    QCOMPARE(result.zones.count(), zones);
    QCOMPARE(result.skippedFeatures, skippedFeatures);
    QCOMPARE(result.missingFeatures, missingFeatures);
    QVERIFY(!result.contentHash.isEmpty());
    for (const GeoZone::Data &zone : std::as_const(result.zones)) {
        QVERIFY(!zone.volumes.isEmpty());
    }

    // Parser version guard: stored sources are restored from parse caches of the same GeoZoneParser::version, so a
    // change of the results must bump it
    const QString hash = _resultHash(result);
    QVERIFY2(GeoZoneParser::version == kResultHashVersion,
             qPrintable(QStringLiteral("GeoZoneParser::version changed: set kResultHashVersion to it and record the result hash %1 of %2 in _testParseFixtures_data").arg(hash, fileName)));
    QVERIFY2(hash == resultHash,
             qPrintable(QStringLiteral("The parse result of %1 changed (hash %2): bump GeoZoneParser::version if the parser changed it, and record the new hash in _testParseFixtures_data").arg(fileName, hash)));

    // The format hint of the detected format and the loader give the same result
    GeoZoneParser::Result hinted;
    QVERIFY2(GeoZoneParser::parse(bytes, hinted, errorString, result.format), qPrintable(errorString));
    QCOMPARE(hinted.format, result.format);
    QCOMPARE(hinted.contentHash, result.contentHash);
    GeoZoneParser::Result loaded;
    QVERIFY2(GeoZoneLoader::parse(bytes, loaded, errorString, static_cast<GeoZoneParser::Format>(formatHint)), qPrintable(errorString));
    QCOMPARE(loaded.zones.count(), zones);
    QCOMPARE(loaded.contentHash, result.contentHash);

    // Features are hashed in a canonical form, independent of the order of their keys and the indentation
    if (!fileName.endsWith(QStringLiteral(".xml"))) {
        const QByteArray reserialized = QJsonDocument::fromJson(bytes.startsWith("\xEF\xBB\xBF") ? bytes.mid(3) : bytes).toJson(QJsonDocument::Indented);
        QVERIFY(reserialized.size() > bytes.size() / 2);
        GeoZoneParser::Result indented;
        QVERIFY2(GeoZoneParser::parse(reserialized, indented, errorString, static_cast<GeoZoneParser::Format>(formatHint)), qPrintable(errorString));
        QCOMPARE(indented.contentHash, result.contentHash);
        QCOMPARE(indented.zones.count(), zones);
        for (qsizetype i = 0; i < result.zones.count(); i++) {
            QCOMPARE(result.zones[i].contentHash.size(), 20);
            QCOMPARE(indented.zones[i].contentHash, result.zones[i].contentHash);
        }
    }
}

void GeoZoneParserTest::_testEmptyResponses_data()
{
    QTest::addColumn<QByteArray>("document");
    QTest::addColumn<int>("formatHint");
    QTest::addColumn<int>("format");    ///< Unknown when the response is an error

    using Format = GeoZoneParser::Format;
    const QByteArray faaList = R"({ "type": "FeatureCollection", "features": [], "totalFeatures": 0, "timeStamp": "2026-10-31T12:20:00Z" })";
    const QByteArray wfsArea = R"({ "type": "FeatureCollection", "features": [], "totalFeatures": 0, "numberMatched": 0, "numberReturned": 0 })";
    const auto row = [](const char *name, const QByteArray &document, Format formatHint, Format format) {
        QTest::newRow(name) << document << static_cast<int>(formatHint) << static_cast<int>(format);
    };
    // An empty list from a web feature service means that no restrictions apply. Files and other feeds must have
    // zones, an empty one is an error which keeps their previous zones.
    row("FAA list", faaList, Format::FaaTfr, Format::FaaTfr);
    row("FAA list from DiPUL", faaList, Format::Dipul, Format::Dipul);
    row("FAA list without hint", faaList, Format::Unknown, Format::Unknown);
    row("FAA list from ED-269", faaList, Format::ED269, Format::Unknown);
    row("DiPUL area", wfsArea, Format::Dipul, Format::Dipul);
    row("IGN area", wfsArea, Format::FranceUas, Format::FranceUas);
    row("area from FAA", wfsArea, Format::FaaTfr, Format::Unknown);
    row("area from ED-318", wfsArea, Format::ED318, Format::Unknown);
    row("area without hint", wfsArea, Format::Unknown, Format::Unknown);
    // ArcGIS feature services answer an area without features with an empty collection, without counts
    const QByteArray arcGisArea = R"({ "type": "FeatureCollection", "crs": { "type": "name", "properties": { "name": "EPSG:4326" } }, "features": [] })";
    row("ArcGIS area from FAA", arcGisArea, Format::FaaAirspace, Format::FaaAirspace);
    row("ArcGIS area without hint", arcGisArea, Format::Unknown, Format::Unknown);
    // The OpenAIP API answers an area without airspaces with an empty list of items
    const QByteArray openAipArea = R"({ "type": "FeatureCollection", "features": [] })";
    row("OpenAIP area", openAipArea, Format::OpenAip, Format::OpenAip);
}

void GeoZoneParserTest::_testEmptyResponses()
{
    QFETCH(QByteArray, document);
    QFETCH(int, formatHint);
    QFETCH(int, format);

    GeoZoneParser::Result result;
    QString errorString;
    const bool parsed = GeoZoneParser::parse(document, result, errorString, static_cast<GeoZoneParser::Format>(formatHint));
    if (format == static_cast<int>(GeoZoneParser::Format::Unknown)) {
        QVERIFY(!parsed);
        QVERIFY(!errorString.isEmpty());
        return;
    }
    QVERIFY2(parsed, qPrintable(errorString));
    QCOMPARE(static_cast<int>(result.format), format);
    QVERIFY(result.zones.isEmpty());
}

void GeoZoneParserTest::_testParseED269()
{
    GeoZoneParser::Result result;
    QString errorString;
    // File starts with a UTF-8 byte order mark like several national feeds
    QVERIFY2(GeoZoneParser::parseFile(QStringLiteral(":/unittest/ed269_test.json"), result, errorString), qPrintable(errorString));
    QCOMPARE(result.title, QStringLiteral("Test UASZoneVersion"));
    QCOMPARE(result.zones.count(), 3);

    {
        // Changing a feature changes its hash and the source's, the hashes of the other features stay
        QFile file(QStringLiteral(":/unittest/ed269_test.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray changedBytes = file.readAll();
        const qsizetype identifier = changedBytes.indexOf("\"T-PROH\"");
        QVERIFY(identifier > 0);
        changedBytes.replace(identifier, 8, "\"T-PROX\"");
        GeoZoneParser::Result changed;
        QVERIFY2(GeoZoneParser::parse(changedBytes, changed, errorString), qPrintable(errorString));
        QVERIFY(changed.contentHash != result.contentHash);
        QVERIFY(changed.zones[0].contentHash != result.zones[0].contentHash);
        QCOMPARE(changed.zones[1].contentHash, result.zones[1].contentHash);
        QCOMPARE(changed.zones[2].contentHash, result.zones[2].contentHash);
    }

    const GeoZone::Data &prohibited = result.zones[0];
    QCOMPARE(prohibited.identifier, QStringLiteral("T-PROH"));
    QCOMPARE(prohibited.restriction, GeoZone::Prohibited);
    QCOMPARE(prohibited.reasons, QStringList({ QStringLiteral("AIR_TRAFFIC"), QStringLiteral("SENSITIVE") }));
    QCOMPARE(prohibited.volumes.count(), 2);
    const GeoZone::Volume &polygon = prohibited.volumes[0];
    QVERIFY(!polygon.isCircle);
    QCOMPARE(polygon.rings.count(), 1);
    QCOMPARE(polygon.rings[0].count(), 4); // Closing vertex removed
    QCOMPARE(GeoZone::toCoordinate(polygon.rings[0][1]), QGeoCoordinate(47.0, 8.1)); // [lon, lat] order
    QCOMPARE(polygon.upperLimit, 120.0);
    QCOMPARE(polygon.upperReference, GeoZone::VerticalReference::Ground);
    QCOMPARE(polygon.upperReferenceCode, QStringLiteral("AGL"));
    QCOMPARE(prohibited.authorities.count(), 1);
    QVERIFY(prohibited.authorities[0].contains(QStringLiteral("Test Authority")));
    QVERIFY(prohibited.authorities[0].contains(QStringLiteral("zones@example.com")));
    // The notice of an authorisation request isn't a notification
    QCOMPARE(prohibited.notificationLeadSecs, -1);

    const GeoZone::Data &authorisation = result.zones[1];
    QCOMPARE(authorisation.restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(authorisation.volumes.count(), 1);
    const GeoZone::Volume &circle = authorisation.volumes[0];
    QVERIFY(circle.isCircle);
    QVERIFY(circle.feet);
    QCOMPARE(circle.center, QGeoCoordinate(47.5, 8.5));
    QCOMPARE(circle.radius, 1000 * 0.3048); // ED-269 radius uses uomDimensions
    QCOMPARE(circle.upperLimit, 400.0);

    const GeoZone::Data &scheduled = result.zones[2];
    QCOMPARE(scheduled.restriction, GeoZone::Conditional);
    QVERIFY(qIsNaN(scheduled.volumes[0].lowerLimit));
    QVERIFY(qIsNaN(scheduled.volumes[0].upperLimit));
    QCOMPARE(scheduled.applicability.count(), 1);
    const GeoZone::TimePeriod &period = scheduled.applicability[0];
    QVERIFY(!period.permanent);
    QCOMPARE(period.start, _utc(QDate(2026, 3, 29), QTime(1, 0)));
    QCOMPARE(period.end, _utc(QDate(2026, 10, 25), QTime(0, 59, 59)));
    QCOMPARE(period.schedule.count(), 1);
    QCOMPARE(period.schedule[0].days, QList<int>({ Qt::Monday, Qt::Tuesday }));
    QCOMPARE(period.schedule[0].startTime, QTime(5, 0));
    QCOMPARE(period.schedule[0].endTime, QTime(18, 0));

    // Classification, exemptions and publisher properties are kept. Unknown units and days are reported, limits
    // in an unknown unit are ignored so the zone applies at all altitudes.
    QVERIFY2(GeoZoneParser::parse(R"({ "features": [
        { "identifier": "A", "restriction": "REQ_AUTHORISATION", "uSpaceClass": "EUROCONTROL", "regulationExemption": "YES",
          "extendedProperties": { "operator": "Airport", "phone": 123 },
          "applicability": [ { "schedule": [ { "day": [ "MON", "HOLIDAY" ], "startTime": "08:00", "endTime": "10:00" } ] } ],
          "geometry": [ { "uomDimensions": "NM", "lowerLimit": 0, "upperLimit": 5, "lowerVerticalReference": "AGL", "upperVerticalReference": "AGL",
                          "horizontalProjection": { "type": "Circle", "center": [ 8, 47 ], "radius": 1 } } ] },
        { "identifier": "B", "restriction": "PROHIBITED",
          "geometry": [ { "uomDimensions": "NM", "lowerLimit": 0, "upperLimit": 5, "lowerVerticalReference": "AGL", "upperVerticalReference": "AGL",
                          "horizontalProjection": { "type": "Circle", "center": [ 8.1, 47 ], "radius": 1 } } ] } ] })", result, errorString), qPrintable(errorString));
    QCOMPARE(result.zones.count(), 2);
    const GeoZone::Data &classified = result.zones[0];
    QCOMPARE(classified.uSpaceClass, QStringLiteral("EUROCONTROL"));
    QCOMPARE(classified.regulationExemption, QStringLiteral("YES"));
    QCOMPARE(classified.extendedProperties, QStringList({ QStringLiteral("operator: Airport"), QStringLiteral("phone: 123") }));
    QVERIFY(std::isnan(classified.volumes[0].upperLimit));
    QVERIFY(classified.applicability[0].schedule[0].days.isEmpty());
    QCOMPARE(classified.issues.count(), 3);
    const QString unitIssue = QStringLiteral("Unknown unit NM, the altitude limits are ignored");
    QCOMPARE(result.issues.value(unitIssue), 2);
    QCOMPARE(result.issues.value(QStringLiteral("Unknown day HOLIDAY in the schedule, taken as every day")), 1);
    // Local times are taken as UTC without the country
    QCOMPARE(result.issues.value(QStringLiteral("Times of day without UTC offset are taken as UTC, the zone has no country")), 1);
    QVERIFY(!classified.applicability[0].schedule[0].timeZone.isValid());
    QCOMPARE(classified.applicability[0].schedule[0].utcOffsetSecs, 0);
    QCOMPARE(GeoZoneText::regulationExemption(classified), QStringLiteral("Exemptions possible"));
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 11, 1), QTime(12, 0)));
    GeoZoneSource source(QStringLiteral("zones.json"), QString());
    source.setResult(result, environment.now(), environment.now());
    QVERIFY(source.warningText().contains(QStringLiteral("%1 (2 zone").arg(unitIssue)));
}

void GeoZoneParserTest::_testParseED318()
{
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parseFile(QStringLiteral(":/unittest/ed318_test.json"), result, errorString), qPrintable(errorString));
    QFile file(QStringLiteral(":/unittest/ed318_test.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    GeoZoneParser::Result swedishResult;
    QVERIFY2(GeoZoneParser::parse(file.readAll(), swedishResult, errorString, GeoZoneParser::Format::Unknown, QStringLiteral("sv-SE")), qPrintable(errorString));
    QCOMPARE(swedishResult.zones.count(), 3);

    QCOMPARE(result.provider, QStringLiteral("Test Authority"));
    QCOMPARE(result.issued, _utc(QDate(2026, 8, 13), QTime(0, 0)));
    QCOMPARE(result.validFrom, _utc(QDate(2026, 8, 13), QTime(0, 0)));
    QCOMPARE(result.validTo, _utc(QDate(2099, 1, 1), QTime(0, 0)));
    QCOMPARE(result.title, QStringLiteral("Test GeoZones"));
    QCOMPARE(result.zones.count(), 3);

    // Point with circle extent, localized name
    const GeoZone::Data &circleZone = result.zones[0];
    QCOMPARE(circleZone.identifier, QStringLiteral("S-CIRC"));
    QCOMPARE(circleZone.name, QStringLiteral("Example power plant"));
    QCOMPARE(swedishResult.zones[0].name, QStringLiteral("Exempel kraftverk"));
    QCOMPARE(circleZone.restriction, GeoZone::Prohibited);
    QCOMPARE(circleZone.volumes.count(), 1);
    const GeoZone::Volume &circle = circleZone.volumes[0];
    QVERIFY(circle.isCircle);
    QCOMPARE(circle.center, QGeoCoordinate(58.5, 15.5));
    QCOMPARE(circle.radius, 500.0); // ED-318 radius is always meters, uom only applies to the layer
    QVERIFY(circle.feet);
    QCOMPARE(circle.upperLimit, 1500.0);
    QCOMPARE(circle.upperReference, GeoZone::VerticalReference::MeanSeaLevel);
    QCOMPARE(circleZone.applicability.count(), 1);
    QCOMPARE(circleZone.applicability[0].schedule.count(), 1);
    QVERIFY(circleZone.applicability[0].schedule[0].days.isEmpty()); // ANY
    QVERIFY(!circleZone.applicability[0].schedule[0].startTime.isValid());
    QCOMPARE(circleZone.applicability[0].schedule[0].startEvent, QStringLiteral("SR"));

    // MultiPolygon with a hole, plain string text fields and nulls
    const GeoZone::Data &multiPolygon = result.zones[1];
    QCOMPARE(multiPolygon.restriction, GeoZone::ReqAuthorisation); // American spelling
    QCOMPARE(multiPolygon.name, QStringLiteral("Control zone"));
    QCOMPARE(multiPolygon.reasons, QStringList(QStringLiteral("AIR_TRAFFIC")));
    QVERIFY(multiPolygon.message.isEmpty());
    QVERIFY(multiPolygon.restrictionConditions.isEmpty());
    QCOMPARE(multiPolygon.volumes.count(), 2);
    QCOMPARE(multiPolygon.volumes[0].rings.count(), 2);
    QCOMPARE(multiPolygon.volumes[0].rings[1].count(), 4);
    QCOMPARE(multiPolygon.volumes[1].rings.count(), 1);
    QCOMPARE(multiPolygon.volumes[1].rings[0].count(), 3);
    QCOMPARE(multiPolygon.volumes[1].upperLimit, 2500.0);

    // GeometryCollection where each geometry has its own layer
    const GeoZone::Data &collection = result.zones[2];
    QCOMPARE(collection.identifier, QStringLiteral("F-GC")); // Feature id fallback
    QCOMPARE(collection.restriction, GeoZone::USpace);
    QCOMPARE(collection.volumes.count(), 2);
    QCOMPARE(collection.volumes[0].lowerLimit, 50.0);
    QCOMPARE(collection.volumes[0].upperLimit, 150.0);
    QCOMPARE(collection.volumes[1].lowerLimit, 0.0);
    QCOMPARE(collection.volumes[1].upperLimit, 50.0);
    QVERIFY(!collection.volumes[1].feet);
    // An authority to notify, without name or notice period
    QVERIFY(collection.notificationAuthority.isEmpty());
    QCOMPARE(collection.notificationLeadSecs, 0);

    // Times without an offset are UTC whatever the local time zone, also in its daylight saving gaps
    const QByteArray offsetless = R"({ "type": "FeatureCollection",
        "metadata": { "issued": "2026-03-08", "validFrom": "2026-03-08T02:30:00", "validTo": "2026-03-29 02:30:00.500" },
        "features": [ { "type": "Feature", "properties": { "type": "PROHIBITED" },
        "geometry": { "type": "Polygon", "coordinates": [ [ [0, 0], [1, 0], [1, 1], [0, 0] ] ] } } ] })";
    QVERIFY2(GeoZoneParser::parse(offsetless, result, errorString), qPrintable(errorString));
    QCOMPARE(result.issued, _utc(QDate(2026, 3, 8), QTime(0, 0)));
    QCOMPARE(result.validFrom, _utc(QDate(2026, 3, 8), QTime(2, 30)));
    QCOMPARE(result.validTo, _utc(QDate(2026, 3, 29), QTime(2, 30, 0, 500)));

    // The notice period of the authority to notify, an ISO 8601 duration
    const QByteArray notified = R"({ "type": "FeatureCollection",
        "features": [ { "type": "Feature", "properties": { "type": "CONDITIONAL", "zoneAuthority": [ { "name": "Info", "purpose": "INFORMATION" },
        { "name": "Tower", "phone": "+41 0000", "purpose": "NOTIFICATION", "intervalBefore": "P2DT12H" } ] },
        "geometry": { "type": "Polygon", "coordinates": [ [ [0, 0], [1, 0], [1, 1], [0, 0] ] ] } } ] })";
    QVERIFY2(GeoZoneParser::parse(notified, result, errorString), qPrintable(errorString));
    QCOMPARE(result.zones[0].notificationAuthority, QStringLiteral("Tower"));
    QCOMPARE(result.zones[0].notificationLeadSecs, qint64{ (2 * 24 + 12) * 3600 });
    QVERIFY(result.zones[0].authorities[1].contains(QStringLiteral("notice P2DT12H")));
}

void GeoZoneParserTest::_testParseNationalVariants()
{
    QString errorString;

    // ENAIRE's downloads: the zone's members in UASZone, its limits in verticalLayer, schedules of any day without
    // times which restrict nothing, and messages in HTML
    GeoZoneParser::Result enaire;
    QFile enaireFile(QStringLiteral(":/unittest/es_enaire_ed318_test.json"));
    QVERIFY(enaireFile.open(QIODevice::ReadOnly));
    QVERIFY2(GeoZoneParser::parse(enaireFile.readAll(), enaire, errorString), qPrintable(errorString));
    QCOMPARE(enaire.format, GeoZoneParser::Format::ED318);
    QCOMPARE(enaire.zones.count(), 3);
    const GeoZone::Data &aerodrome = enaire.zones[0];
    QCOMPARE(aerodrome.identifier, QStringLiteral("LETY45"));
    QCOMPARE(aerodrome.name, QStringLiteral("TINAJEROS"));
    QCOMPARE(aerodrome.country, QStringLiteral("ESP"));
    QCOMPARE(aerodrome.restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(aerodrome.reasons, QStringList(QStringLiteral("AIR_TRAFFIC")));
    QCOMPARE(aerodrome.volumes[0].lowerLimit, 45.0);
    QCOMPARE(aerodrome.volumes[0].lowerReference, GeoZone::VerticalReference::Ground);
    QCOMPARE(aerodrome.volumes[0].upperLimit, 900.0);
    QCOMPARE(aerodrome.volumes[0].upperReference, GeoZone::VerticalReference::Ground);
    QVERIFY(aerodrome.applicability.isEmpty());
    QVERIFY2(!aerodrome.message.contains(QLatin1Char('<')) && aerodrome.message.contains(QStringLiteral("TINAJEROS")), qPrintable(aerodrome.message));
    QVERIFY(aerodrome.extendedProperties.contains(QStringLiteral("Aeródromo")));
    QVERIFY2(!aerodrome.authorities.isEmpty() && aerodrome.authorities[0].startsWith(QStringLiteral("Authorisation: ")), qPrintable(aerodrome.authorities.join(QLatin1Char('|'))));
    const GeoZone::Data &photography = enaire.zones[1];
    QCOMPARE(photography.reasons, QStringList(QStringLiteral("PRIVACY")));
    QVERIFY(photography.volumes[0].feet);
    QCOMPARE(photography.volumes[0].upperLimit, 3000.0);
    QCOMPARE(enaire.zones[2].volumes[0].upperReference, GeoZone::VerticalReference::MeanSeaLevel);
    QCOMPARE(enaire.zones[2].volumes[0].upperLimit, 4938.0);

    // Its feature service has the same members as attributes of the features
    GeoZoneParser::Result servais;
    QFile servaisFile(QStringLiteral(":/unittest/es_servais_ed318_test.json"));
    QVERIFY(servaisFile.open(QIODevice::ReadOnly));
    QVERIFY2(GeoZoneParser::parse(servaisFile.readAll(), servais, errorString, GeoZoneParser::Format::ED318), qPrintable(errorString));
    QCOMPARE(servais.zones.count(), 2);
    QCOMPARE(servais.zones[0].identifier, QStringLiteral("LEGT0"));
    QCOMPARE(servais.zones[0].restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(servais.zones[0].volumes[0].upperLimit, 900.0);
    QVERIFY(servais.zones[0].applicability.isEmpty());
    const GeoZone::Data &railway = servais.zones[1];
    QCOMPARE(railway.restriction, GeoZone::Conditional);
    QCOMPARE(railway.otherReasonInfo, QStringLiteral("Protección Infraestructura Ferroviaria"));
    QCOMPARE(railway.volumes[0].upperLimit, 120.0);
    QCOMPARE(railway.notificationLeadSecs, qint64{ 20 * 24 * 3600 });
    QCOMPARE(railway.notificationAuthority, QStringLiteral("ADIF"));
    QVERIFY(railway.authorities.constFirst().contains(QStringLiteral("autorizacioneszonadeafeccion@adif.es")));
    // Untitled downloads are named after their service
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 11, 10), QTime(12, 0)));
    GeoZoneSource areaSource(QStringLiteral("enaire"), GeoZoneProviders::areaFeedUrl(QStringLiteral("enaire-infra"), QGeoRectangle(QGeoCoordinate(40.5, -3.8), QGeoCoordinate(40.3, -3.6))));
    areaSource.setResult(servais, environment.now(), environment.now());
    QCOMPARE(areaSource.name(), QStringLiteral("Spain infrastructure zones (ENAIRE), area around 40.40, -3.70"));
    QCOMPARE(GeoZoneProviders::serviceName(QStringLiteral("https://aip.enaire.es/recursos/descargas/ZGUAS/ZGUAS_Aero.zip")), QStringLiteral("Spain aeronautical zones (ENAIRE)"));
    QVERIFY(GeoZoneProviders::serviceName(QStringLiteral("https://aip.enaire.es/recursos/descargas/ZGUAS/other.zip")).isEmpty());

    // Austro Control's ED-269 zones are a list, their full messages are in the localized messages
    QFile austriaFile(QStringLiteral(":/unittest/at_ed269_test.json"));
    QVERIFY(austriaFile.open(QIODevice::ReadOnly));
    const QByteArray austriaBytes = austriaFile.readAll();
    GeoZoneParser::Result austria;
    QVERIFY2(GeoZoneParser::parse(austriaBytes, austria, errorString), qPrintable(errorString));
    QCOMPARE(austria.format, GeoZoneParser::Format::ED269);
    QCOMPARE(austria.zones.count(), 3);
    const GeoZone::Data &ctr = austria.zones[0];
    QCOMPARE(ctr.restriction, GeoZone::Conditional);
    QVERIFY2(ctr.message.startsWith(QStringLiteral("Control Zone Graz Airport (LOWG)")), qPrintable(ctr.message));
    QVERIFY(std::none_of(ctr.extendedProperties.cbegin(), ctr.extendedProperties.cend(), [](const QString &line) { return line.startsWith(QStringLiteral("localizedMessages")); }));
    QVERIFY(!ctr.extendedProperties.isEmpty());
    QCOMPARE(austria.zones[1].applicability.count(), 2);
    QCOMPARE(austria.zones[1].applicability[0].schedule.count(), 1);
    QCOMPARE(austria.zones[2].restriction, GeoZone::NoRestriction);
    GeoZoneParser::Result german;
    QVERIFY2(GeoZoneParser::parse(austriaBytes, german, errorString, GeoZoneParser::Format::Unknown, QStringLiteral("de")), qPrintable(errorString));
    QVERIFY(german.zones[0].message.startsWith(QStringLiteral("Kontrollzone Flughafen Graz (LOWG)")));

    // Markup in messages
    QCOMPARE(GeoZoneParserUtils::_plainText(QStringLiteral("<p>Zone <b>A</b> &amp; B<br/>see <a href='x'>here</a></p><p></p>")), QStringLiteral("Zone A & B\nsee here"));
    QCOMPARE(GeoZoneParserUtils::_plainText(QStringLiteral("Altitude < 120 m")), QStringLiteral("Altitude < 120 m"));
}

void GeoZoneParserTest::_testParseFaaAirspace()
{
    // Excerpts of the FAA's ArcGIS layers around Washington: UAS facility map cells, a national security restriction,
    // class B airspace, a Mode C veil left out, and special use airspace
    QFile file(QStringLiteral(":/unittest/faa_airspace_test.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(file.readAll(), result, errorString), qPrintable(errorString));
    QCOMPARE(result.format, GeoZoneParser::Format::FaaAirspace);
    QVERIFY(result.title.contains(QStringLiteral("facility maps")));
    QVERIFY(result.provider.contains(QStringLiteral("informational")));
    QCOMPARE(result.zones.count(), 6);
    QCOMPARE(result.skippedFeatures, 1);
    QMap<QString, GeoZone::Data> byCode;
    for (const GeoZone::Data &zone : std::as_const(result.zones)) {
        QCOMPARE(zone.country, QStringLiteral("USA"));
        QVERIFY(zone.extendedProperties.join(QLatin1Char('\n')).contains(QStringLiteral("informational")));
        byCode.insert(zone.restrictionCode, zone);
    }

    // Facility map cells need an authorisation up to their ceiling, cells at 0 ft further coordination
    const GeoZone::Data &cell = byCode.value(QStringLiteral("UASFM 300 FT"));
    QCOMPARE(cell.restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(cell.name, QStringLiteral("KIAD facility map, 300 ft"));
    QCOMPARE(cell.restrictionConditions, QStringLiteral("LAANC up to 300 ft AGL"));
    QVERIFY(cell.message.contains(QStringLiteral("Class B airspace")));
    QCOMPARE(cell.reasons, QStringList(QStringLiteral("AIR_TRAFFIC")));
    QCOMPARE(cell.volumes[0].lowerLimit, 0.0);
    QCOMPARE(cell.volumes[0].lowerReference, GeoZone::VerticalReference::Ground);
    QVERIFY(GeoZone(cell).contains(GeoZone::Position{ QGeoCoordinate(38.896, -77.487), 2000, 90 }));
    const GeoZone::Data &closed = byCode.value(QStringLiteral("UASFM 0 FT"));
    QCOMPARE(closed.restriction, GeoZone::Prohibited);
    QVERIFY(closed.message.contains(QStringLiteral("further coordination")));

    // National security restrictions are prohibited from the surface to their ceiling
    const GeoZone::Data &security = byCode.value(QStringLiteral("NSUFR"));
    QCOMPARE(security.restriction, GeoZone::Prohibited);
    QCOMPARE(security.name, QStringLiteral("USNO"));
    QCOMPARE(security.reasons, QStringList(QStringLiteral("SENSITIVE")));
    QCOMPARE(security.otherReasonInfo, QStringLiteral("National Security"));
    QVERIFY(security.authorities.join(QLatin1Char('\n')).contains(QStringLiteral("henry.alderete@navy.mil")));
    QVERIFY(security.volumes[0].feet);
    QCOMPARE(security.volumes[0].upperLimit, 400.0);
    QCOMPARE(security.volumes[0].upperReference, GeoZone::VerticalReference::Ground);

    // Class B from the surface to 10,000 ft MSL
    const GeoZone::Data &classB = byCode.value(QStringLiteral("CLASS_B"));
    QCOMPARE(classB.restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(classB.name, QStringLiteral("WASHINGTON-TRI AREA CLASS B"));
    QCOMPARE(classB.volumes[0].lowerReference, GeoZone::VerticalReference::Ground);
    QCOMPARE(classB.volumes[0].upperLimit, 10000.0);
    QCOMPARE(classB.volumes[0].upperReference, GeoZone::VerticalReference::MeanSeaLevel);

    // Special use airspace: prohibited and restricted areas, the latter up to unlimited with its times of use
    const GeoZone::Data &prohibited = byCode.value(QStringLiteral("P"));
    QCOMPARE(prohibited.name, QStringLiteral("P-56"));
    QCOMPARE(prohibited.restriction, GeoZone::Prohibited);
    QCOMPARE(prohibited.volumes[0].upperLimit, 18000.0);
    const GeoZone::Data &restricted = byCode.value(QStringLiteral("R"));
    QCOMPARE(restricted.name, QStringLiteral("R-4001A"));
    QCOMPARE(restricted.restriction, GeoZone::ReqAuthorisation);
    QVERIFY(restricted.restrictionConditions.contains(QStringLiteral("SURFACE TO UNLIMITED")));
    QCOMPARE(restricted.volumes[0].upperLimit, 99999.0);
    QCOMPARE(restricted.authorities, QStringList(QStringLiteral("FAA, WASHINGTON ARTCC")));

    // Errors like the rate limit of the ArcGIS services are reported
    QVERIFY(!GeoZoneParser::parse(R"({"error":{"code":429,"message":"Unable to perform query. Too many requests.","details":["Retry after 60 sec."]}})",
                                  result, errorString, GeoZoneParser::Format::FaaAirspace));
    QCOMPARE(errorString, QStringLiteral("The server reported an error: Unable to perform query. Too many requests. Retry after 60 sec."));
}

void GeoZoneParserTest::_testParseOpenAip()
{
    // A synthetic export of OpenAIP airspaces, of which those concerning drones flying low are kept
    QFile file(QStringLiteral(":/unittest/openaip_test.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(bytes, result, errorString), qPrintable(errorString));
    QCOMPARE(result.format, GeoZoneParser::Format::OpenAip);
    QCOMPARE(result.title, QStringLiteral("Airspaces of CH (OpenAIP, unofficial, CC BY-NC 4.0)"));
    QCOMPARE(result.provider, QStringLiteral("OpenAIP (unofficial, CC BY-NC 4.0)"));
    QCOMPARE(result.zones.count(), 6);
    QCOMPARE(result.skippedFeatures, 2);     // The class E TMA and the FIR
    QMap<QString, GeoZone::Data> byName;
    for (const GeoZone::Data &zone : std::as_const(result.zones)) {
        QVERIFY(zone.extendedProperties.join(QLatin1Char('\n')).contains(QStringLiteral("unofficial community data (CC BY-NC 4.0")));
        QCOMPARE(zone.country, QStringLiteral("CH"));
        byName.insert(zone.name, zone);
    }

    const GeoZone::Data &ctr = byName.value(QStringLiteral("TESTHAUSEN CTR"));
    QCOMPARE(ctr.restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(ctr.restrictionCode, QStringLiteral("CTR (class D)"));
    QCOMPARE(ctr.reasons, QStringList(QStringLiteral("AIR_TRAFFIC")));
    QCOMPARE(ctr.identifier, QStringLiteral("650000000000000000000001"));
    QVERIFY(ctr.extendedProperties.contains(QStringLiteral("Frequency: 118.100 MHz TESTHAUSEN TOWER")));
    QVERIFY(ctr.volumes[0].feet);
    QCOMPARE(ctr.volumes[0].lowerReference, GeoZone::VerticalReference::Ground);
    QCOMPARE(ctr.volumes[0].upperLimit, 2500.0);
    QCOMPARE(ctr.volumes[0].upperReference, GeoZone::VerticalReference::MeanSeaLevel);

    // Flight levels, hours of operation and activation
    const GeoZone::Data &restricted = byName.value(QStringLiteral("R-TEST RANGE"));
    QCOMPARE(restricted.restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(restricted.volumes[0].upperLimit, 9500.0);
    QCOMPARE(restricted.volumes[0].upperReference, GeoZone::VerticalReference::FlightLevel);
    QCOMPARE(restricted.restrictionConditions, QStringLiteral("Activated by NOTAM"));
    QVERIFY(restricted.extendedProperties.contains(QStringLiteral("Hours of operation: Mon 08:00-17:00, Tue 08:00-17:00, Wed 08:00-17:00, Thu 08:00-17:00, Fri 08:00-17:00")));
    QVERIFY(restricted.extendedProperties.contains(QStringLiteral("Remarks: Firing range")));

    // Limits in meters are converted to feet
    const GeoZone::Data &prohibited = byName.value(QStringLiteral("P-TEST PLANT"));
    QCOMPARE(prohibited.restriction, GeoZone::Prohibited);
    QVERIFY(qAbs(prohibited.volumes[0].upperLimit - (1000 / 0.3048)) < 0.01);
    QVERIFY(qAbs(byName.value(QStringLiteral("TEST NATURE RESERVE")).volumes[0].upperLimit - (150 / 0.3048)) < 0.01);
    QCOMPARE(byName.value(QStringLiteral("TEST NATURE RESERVE")).restriction, GeoZone::Conditional);
    QCOMPARE(byName.value(QStringLiteral("TEST NATURE RESERVE")).reasons, QStringList(QStringLiteral("NATURE")));
    QCOMPARE(byName.value(QStringLiteral("TESTHAUSEN TMA 1")).restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(byName.value(QStringLiteral("D-TEST GLIDING")).restriction, GeoZone::Conditional);
    QCOMPARE(byName.value(QStringLiteral("D-TEST GLIDING")).restrictionConditions, QStringLiteral("Active on demand"));

    // The items of the API's lists are the airspaces themselves, with their geometry
    QJsonArray items;
    for (const QJsonValue &feature : QJsonDocument::fromJson(bytes).object().value(QStringLiteral("features")).toArray()) {
        QJsonObject item = feature.toObject().value(QStringLiteral("properties")).toObject();
        item.insert(QStringLiteral("geometry"), feature.toObject().value(QStringLiteral("geometry")));
        items.append(item);
    }
    const QByteArray response = QJsonDocument(QJsonObject{ { "limit", 1000 }, { "totalCount", items.count() }, { "totalPages", 1 }, { "page", 1 }, { "items", items } }).toJson();
    const QByteArray list = GeoZoneService::extractMember(response, QStringLiteral("items"), errorString);
    GeoZoneParser::Result api;
    QVERIFY2(GeoZoneParser::parse(list, api, errorString), qPrintable(errorString));
    QCOMPARE(api.format, GeoZoneParser::Format::OpenAip);
    QCOMPARE(api.zones.count(), 6);
    QCOMPARE(api.zones[0].volumes[0].rings, result.zones[0].volumes[0].rings);
}

void GeoZoneParserTest::_testEd318Writer()
{
    // Zones like the operator draws them are written as ED-318 and read back identical
    GeoZone::Data polygon;
    polygon.identifier = QStringLiteral("MY-1");
    polygon.name = QStringLiteral("Quarry");
    polygon.restriction = GeoZone::Prohibited;
    polygon.restrictionCode = QStringLiteral("PROHIBITED");
    polygon.reasons = { QStringLiteral("OTHER") };
    polygon.otherReasonInfo = QStringLiteral("Drawn by the operator");
    polygon.message = QStringLiteral("Blasting");
    polygon.authorities = { QStringLiteral("Operator") };
    GeoZone::Volume square;
    square.rings.append(GeoZone::Ring{ QPointF(8.0, 47.0), QPointF(8.01, 47.0), QPointF(8.01, 47.01), QPointF(8.0, 47.01) });
    square.rings.append(GeoZone::Ring{ QPointF(8.004, 47.004), QPointF(8.004, 47.006), QPointF(8.006, 47.006), QPointF(8.006, 47.004) });
    square.lowerLimit = 0;
    square.upperLimit = 120;
    square.setReferences(QStringLiteral("AGL"), QStringLiteral("AGL"));
    polygon.volumes.append(square);

    GeoZone::Data circle;
    circle.identifier = QStringLiteral("MY-2");
    circle.name = QStringLiteral("Event");
    circle.restriction = GeoZone::ReqAuthorisation;
    circle.restrictionCode = GeoZoneEd318Writer::restrictionCode(GeoZone::ReqAuthorisation);
    GeoZone::Volume disc;
    disc.isCircle = true;
    disc.center = QGeoCoordinate(47.1, 8.1);
    disc.radius = 250;
    disc.feet = true;
    disc.lowerLimit = 1500;
    disc.setReferences(QStringLiteral("AMSL"), QStringLiteral("AMSL"));
    circle.volumes.append(disc);

    // Without limits, at all altitudes, in two volumes
    GeoZone::Data unlimited;
    unlimited.identifier = QStringLiteral("MY-3");
    unlimited.name = QStringLiteral("Two parts");
    unlimited.restriction = GeoZone::Conditional;
    unlimited.restrictionCode = QStringLiteral("CONDITIONAL");
    GeoZone::Volume part;
    part.rings.append(GeoZone::Ring{ QPointF(9.0, 46.0), QPointF(9.01, 46.0), QPointF(9.0, 46.01) });
    unlimited.volumes.append(part);
    part.rings[0] = GeoZone::Ring{ QPointF(9.1, 46.0), QPointF(9.11, 46.0), QPointF(9.1, 46.01) };
    unlimited.volumes.append(part);

    const QDateTime issued = _utc(QDate(2026, 11, 2), QTime(12, 0));
    const QByteArray document = GeoZoneEd318Writer::document({ polygon, circle, unlimited }, QStringLiteral("My zones"), QStringLiteral("Operator"), issued);
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(document, result, errorString), qPrintable(errorString));
    QCOMPARE(result.format, GeoZoneParser::Format::ED318);
    QCOMPARE(result.title, QStringLiteral("My zones"));
    QCOMPARE(result.provider, QStringLiteral("Operator"));
    QCOMPARE(result.issued, issued);
    QCOMPARE(result.zones.count(), 3);
    for (const GeoZone::Data &zone : std::as_const(result.zones)) {
        QVERIFY2(zone.issues.isEmpty(), qPrintable(zone.issues.join(QLatin1Char('\n'))));
    }
    GeoZoneParser::Result parsed = result;
    parsed.contentHash.clear();
    for (GeoZone::Data &zone : parsed.zones) {
        zone.contentHash.clear();
    }
    GeoZoneParser::Result expected = parsed;
    expected.zones = { polygon, circle, unlimited };
    QVERIFY2(_resultDifference(parsed, expected).isEmpty(), qPrintable(_resultDifference(parsed, expected)));
    QCOMPARE(result.zones[1].restrictionCode, QStringLiteral("REQ_AUTHORIZATION"));
    QCOMPARE(result.zones[1].restriction, GeoZone::ReqAuthorisation);

    // Clockwise rings are written counterclockwise, as GeoJSON wants them
    GeoZone::Data clockwise = polygon;
    clockwise.volumes[0].rings = { GeoZone::Ring{ QPointF(8.0, 47.0), QPointF(8.0, 47.01), QPointF(8.01, 47.01), QPointF(8.01, 47.0) } };
    const QJsonArray ring = GeoZoneEd318Writer::feature(clockwise).value(QStringLiteral("geometry")).toObject().value(QStringLiteral("coordinates")).toArray().first().toArray();
    QCOMPARE(ring.count(), 5);
    QCOMPARE(ring[0].toArray(), (QJsonArray{ 8.01, 47.0 }));
    QCOMPARE(ring[1].toArray(), (QJsonArray{ 8.01, 47.01 }));
    QCOMPARE(ring[4].toArray(), ring[0].toArray());
}

void GeoZoneParserTest::_testParseFaaTfr()
{
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parseFile(QStringLiteral(":/unittest/faa_tfr_test.json"), result, errorString), qPrintable(errorString));
    QCOMPARE(result.zones.count(), 2);
    QCOMPARE(result.issued, _utc(QDate(2026, 10, 31), QTime(12, 20)));

    const GeoZone::Data &tfr = result.zones[0];
    QCOMPARE(tfr.identifier, QStringLiteral("6/4240-1-FDC-F"));
    QCOMPARE(tfr.restriction, GeoZone::Prohibited);
    QVERIFY(tfr.name.startsWith(QStringLiteral("SAN ANGELO, TX")));
    QCOMPARE(tfr.otherReasonInfo, QStringLiteral("SECURITY"));
    QVERIFY(tfr.message.contains(QStringLiteral("6/4240")));
    QCOMPARE(tfr.volumes.count(), 1);
    QVERIFY(qIsNaN(tfr.volumes[0].lowerLimit) && qIsNaN(tfr.volumes[0].upperLimit));
    QVERIFY(tfr.applicability.isEmpty());

    // Live restrictions are refreshed more often than other feeds
    GeoZoneSource source(QStringLiteral("https://tfr.faa.gov/x"), QStringLiteral("https://tfr.faa.gov/x"));
    QVERIFY2(GeoZoneParser::parseFile(QStringLiteral(":/unittest/faa_tfr_test.json"), result, errorString), qPrintable(errorString));
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 11, 1), QTime(12, 0)));
    const QDateTime now = environment.now();
    source.setResult(result, now, now);
    const GeoZoneRefreshScheduler scheduler;
    QCOMPARE(scheduler.refreshIntervalSecs(source), 15 * 60);
    QVERIFY(!scheduler.isDue(source, now.addSecs(10 * 60)));
    QVERIFY(scheduler.isDue(source, now.addSecs(16 * 60)));

    // Restrictions change during the day, so the list is outdated after hours rather than weeks
    source.updateValidity(result.issued.addSecs(3600));
    QVERIFY(source.validityText().isEmpty());
    source.updateValidity(result.issued.addSecs(3 * 3600));
    QCOMPARE(source.validityText(), QStringLiteral("Data is 3 hour(s) old"));

    // The NOTAM details give the times and altitudes of their TFRs, which apply at all times and altitudes without
    const QByteArray detail = R"(<XNOTAM-Update><Group><Add><Not>
        <dateEffective>2026-03-06T14:00:00</dateEffective><dateExpire>2026-03-06T22:00:00</dateExpire>
        <TfrNot><TFRAreaGroup><aseTFRArea><txtName>Area A</txtName>
        <codeDistVerUpper>ALT</codeDistVerUpper><valDistVerUpper>3000</valDistVerUpper><uomDistVerUpper>FT</uomDistVerUpper>
        <codeDistVerLower>HEI</codeDistVerLower><valDistVerLower>0</valDistVerLower><uomDistVerLower>FT</uomDistVerLower>
        <ScheduleGroup><dateEffective>2026-03-06T14:00:00</dateEffective><dateExpire>2026-03-06T22:00:00</dateExpire></ScheduleGroup>
        </aseTFRArea></TFRAreaGroup></TfrNot></Not></Add></Group></XNOTAM-Update>)";
    QFile tfrFile(QStringLiteral(":/unittest/faa_tfr_test.json"));
    QVERIFY(tfrFile.open(QIODevice::ReadOnly));
    const QJsonObject tfrRoot = QJsonDocument::fromJson(tfrFile.readAll()).object();
    const QList<GeoZoneFormat::CompanionRequest> requests = GeoZoneFormat::find(GeoZoneParser::Format::FaaTfr)->companionRequests(
        QUrl(QStringLiteral("https://tfr.faa.gov/geoserver/TFR/ows?service=WFS")), tfrRoot, tfrRoot.value(QStringLiteral("features")).toArray());
    QCOMPARE(requests.count(), 2);
    QCOMPARE(requests[0].key, QStringLiteral("6/4240"));
    QCOMPARE(requests[0].version, QStringLiteral("202603052027"));
    QCOMPARE(requests[0].url, QStringLiteral("https://tfr.faa.gov/download/detail_6_4240.xml"));
    QVERIFY2(GeoZoneParser::parse(GeoZoneParser::composite(tfrRoot, { { QStringLiteral("6/4240"), { QStringLiteral("202603052027"), detail } } }), result, errorString), qPrintable(errorString));
    QCOMPARE(result.companions.count(), 1);
    for (const GeoZone::Data &tfr : std::as_const(result.zones)) {
        if (!tfr.identifier.startsWith(QStringLiteral("6/4240"))) {
            QVERIFY(tfr.applicability.isEmpty());
            QVERIFY(std::isnan(tfr.volumes[0].upperLimit));
            continue;
        }
        QCOMPARE(tfr.applicability.count(), 1);
        QCOMPARE(tfr.applicability[0].start, _utc(QDate(2026, 3, 6), QTime(14, 0)));
        QCOMPARE(tfr.applicability[0].end, _utc(QDate(2026, 3, 6), QTime(22, 0)));
        QCOMPARE(tfr.volumes[0].upperLimit, 3000.0);
        QVERIFY(tfr.volumes[0].feet);
        QCOMPARE(tfr.volumes[0].upperReference, GeoZone::VerticalReference::MeanSeaLevel);
        QCOMPARE(tfr.volumes[0].lowerReference, GeoZone::VerticalReference::Ground);
    }
}

void GeoZoneParserTest::_testParseNationalFormats()
{
    // German DiPUL WFS zones, mapped from their type to a restriction, with limits in meters
    GeoZoneParser::Result dipul;
    QString errorString;
    QFile dipulFile(QStringLiteral(":/unittest/dipul_test.json"));
    QVERIFY(dipulFile.open(QIODevice::ReadOnly));
    QVERIFY2(GeoZoneParser::parse(dipulFile.readAll(), dipul, errorString, GeoZoneParser::Format::Dipul, QStringLiteral("en")), qPrintable(errorString));
    QCOMPARE(dipul.zones.count(), 4);
    const GeoZone::Data &ctr = dipul.zones[0];
    QCOMPARE(ctr.name, QStringLiteral("Testhausen control zone 1"));
    QCOMPARE(ctr.restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(ctr.country, QStringLiteral("DEU"));
    QCOMPARE(ctr.volumes[0].lowerLimit, 0.0);
    QCOMPARE(ctr.volumes[0].lowerReference, GeoZone::VerticalReference::Ground);
    QCOMPARE(ctr.volumes[0].upperLimit, 2500 * 0.3048);
    QCOMPARE(ctr.volumes[0].upperReference, GeoZone::VerticalReference::MeanSeaLevel);
    QCOMPARE(dipul.zones[1].restriction, GeoZone::Prohibited);     // Restricted area
    const GeoZone::Data &temporary = dipul.zones[2];
    QCOMPARE(temporary.restriction, GeoZone::Prohibited);
    QCOMPARE(temporary.applicability.count(), 1);
    QCOMPARE(temporary.applicability[0].end, _utc(QDate(2026, 12, 31), QTime(22, 59)));
    QVERIFY(qAbs(temporary.volumes[0].upperLimit - 701.0) < 0.1);
    QCOMPARE(dipul.zones[3].reasons, QStringList(QStringLiteral("EMERGENCY")));
    QVERIFY(dipul.zones[3].message.contains(QStringLiteral("LuftVO")));

    // French IGN restrictions, prohibited entirely or above their maximum height, from a limited WFS response
    GeoZoneParser::Result france;
    QFile franceFile(QStringLiteral(":/unittest/france_uas_test.json"));
    QVERIFY(franceFile.open(QIODevice::ReadOnly));
    QVERIFY2(GeoZoneParser::parse(franceFile.readAll(), france, errorString), qPrintable(errorString));
    QCOMPARE(france.zones.count(), 2);
    QCOMPARE(france.zones[0].name, QStringLiteral("Vol interdit"));
    QVERIFY(std::isnan(france.zones[0].volumes[0].lowerLimit));
    QVERIFY(france.zones[0].restrictionConditions.contains(QStringLiteral("agglomération")));
    QCOMPARE(france.zones[1].volumes[0].lowerLimit, 50.0);
    QCOMPARE(france.zones[1].volumes[0].lowerReference, GeoZone::VerticalReference::Ground);
    const GeoZone limited(france.zones[1]);
    const QGeoCoordinate inside(48.81, 2.41);
    QVERIFY(!limited.contains(GeoZone::Position{ inside, 340, 300 }));
    QVERIFY(limited.contains(GeoZone::Position{ inside, 360, 300 }));
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 11, 1), QTime(12, 0)));
    GeoZoneSource source(QStringLiteral("france"), QStringLiteral("https://data.geopf.fr/wfs"));
    source.setResult(france, environment.now(), environment.now());
    QVERIFY(source.warningText().contains(QStringLiteral("8")));
    GeoZoneSource areaSource(QStringLiteral("area"), QStringLiteral("https://data.geopf.fr/wfs/ows?SERVICE=WFS&BBOX=2.25,48.8,2.45,48.9,EPSG:4326"));
    areaSource.setResult(france, environment.now(), environment.now());
    QCOMPARE(areaSource.name(), QStringLiteral("UAS restrictions (France), area around 48.85, 2.35"));

    // UK flight restriction zones in AIXM 5.1: arcs and geodesic strings, a circle and a danger area
    GeoZoneParser::Result aixm;
    QFile aixmFile(QStringLiteral(":/unittest/uk_aixm_test.xml"));
    QVERIFY(aixmFile.open(QIODevice::ReadOnly));
    const QByteArray aixmBytes = aixmFile.readAll();
    QVERIFY2(GeoZoneParser::parse(aixmBytes, aixm, errorString), qPrintable(errorString));
    QCOMPARE(aixm.zones.count(), 3);
    QMap<QString, GeoZone::Data> byName;
    for (const GeoZone::Data &data : std::as_const(aixm.zones)) {
        byName.insert(data.name, data);
    }
    const GeoZone::Data &frz = byName.value(QStringLiteral("BLACKBUSHE"));
    QCOMPARE(frz.restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(frz.country, QStringLiteral("GBR"));
    QVERIFY(frz.volumes[0].feet);
    QCOMPARE(frz.volumes[0].upperLimit, 2000.0);
    QCOMPARE(frz.volumes[0].upperReference, GeoZone::VerticalReference::Ground);
    // The arc of 2 NM around the aerodrome reference point covers most of the boundary
    const GeoZone frzZone(frz);
    const QGeoCoordinate aerodrome(51.32389, -0.84750);
    QVERIFY(frzZone.contains(aerodrome.atDistanceAndAzimuth(3500, 0)));
    QVERIFY(!frzZone.contains(aerodrome.atDistanceAndAzimuth(4000, 0)));
    const GeoZone::Data &circle = byName.value(QStringLiteral("SPRINGFIELDS"));
    QVERIFY(circle.volumes[0].isCircle);
    QCOMPARE(circle.volumes[0].radius, 2 * 1852.0);
    QCOMPARE(byName.value(QStringLiteral("RAASAY")).restriction, GeoZone::Conditional);

#ifndef GEOZONE_CORE_TEST
    // Zipped, like the NATS download
    const QTemporaryDir zipDir;
    QVERIFY(zipDir.isValid());
    QVERIFY(QDir(zipDir.path()).mkpath(QStringLiteral("content")));
    QFile xmlFile(zipDir.filePath(QStringLiteral("content/data.xml")));
    QVERIFY(xmlFile.open(QIODevice::WriteOnly));
    QCOMPARE(xmlFile.write(aixmBytes), aixmBytes.size());
    xmlFile.close();
    QVERIFY(QGCZip::zipDirectory(zipDir.filePath(QStringLiteral("content")), zipDir.filePath(QStringLiteral("data.zip"))));
    QFile zipFile(zipDir.filePath(QStringLiteral("data.zip")));
    QVERIFY(zipFile.open(QIODevice::ReadOnly));
    QCOMPARE(GeoZoneLoader::extractZip(zipFile.readAll(), errorString), aixmBytes);
    QCOMPARE(GeoZoneLoader::extractZip(QByteArray("PK\x03\x04" "broken"), errorString), QByteArray());
    QVERIFY(!errorString.isEmpty());
#endif

    // Archives expanding beyond the limit are stopped while inflating, whatever their headers declare
    const auto deflatedZeros = [](qint64 size) {
        z_stream stream{};
        (void) deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        const QByteArray zeros(1 << 20, '\0');
        QByteArray output;
        for (qint64 written = 0; written < size; written += zeros.size()) {
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(zeros.constData()));
            stream.avail_in = zeros.size();
            const int flush = ((written + zeros.size()) >= size) ? Z_FINISH : Z_NO_FLUSH;
            do {
                char buffer[65536];
                stream.next_out = reinterpret_cast<Bytef*>(buffer);
                stream.avail_out = sizeof(buffer);
                (void) deflate(&stream, flush);
                output.append(buffer, sizeof(buffer) - stream.avail_out);
            } while (stream.avail_out == 0);
        }
        deflateEnd(&stream);
        return output;
    };
    const auto zipArchive = [](const QByteArray &name, const QByteArray &compressed, quint32 declaredSize, quint16 entryCount = 1) {
        const auto u16 = [](QByteArray &bytes, quint16 value) { bytes.append(reinterpret_cast<const char*>(&value), 2); };
        const auto u32 = [](QByteArray &bytes, quint32 value) { bytes.append(reinterpret_cast<const char*>(&value), 4); };
        QByteArray archive;
        u32(archive, 0x04034b50); u16(archive, 20); u16(archive, 0); u16(archive, 8); u16(archive, 0); u16(archive, 0);
        u32(archive, 0); u32(archive, compressed.size()); u32(archive, declaredSize); u16(archive, name.size()); u16(archive, 0);
        archive.append(name);
        archive.append(compressed);
        const quint32 directoryOffset = archive.size();
        u32(archive, 0x02014b50); u16(archive, 20); u16(archive, 20); u16(archive, 0); u16(archive, 8); u16(archive, 0); u16(archive, 0);
        u32(archive, 0); u32(archive, compressed.size()); u32(archive, declaredSize); u16(archive, name.size()); u16(archive, 0); u16(archive, 0);
        u16(archive, 0); u16(archive, 0); u32(archive, 0); u32(archive, 0);
        archive.append(name);
        const quint32 directorySize = archive.size() - directoryOffset;
        u32(archive, 0x06054b50); u16(archive, 0); u16(archive, 0); u16(archive, entryCount); u16(archive, entryCount);
        u32(archive, directorySize); u32(archive, directoryOffset); u16(archive, 0);
        return archive;
    };
    constexpr qint64 limit = 1 << 20;
    const QByteArray bomb = deflatedZeros(8 * limit);
    QVERIFY(bomb.size() < (limit / 10));
    QVERIFY(GeoZoneZip::largestFile(zipArchive("zones.json", bomb, 1000), { QStringLiteral(".json") }, limit, errorString).isEmpty());
    QCOMPARE(errorString, QStringLiteral("The file in the zip archive is too large."));
    QVERIFY(GeoZoneZip::largestFile(zipArchive("zones.json", bomb, 8 * limit), { QStringLiteral(".json") }, limit, errorString).isEmpty());
    QCOMPARE(errorString, QStringLiteral("The file in the zip archive is too large."));
    QVERIFY(GeoZoneZip::largestFile(zipArchive("zones.json", bomb, 1000, GeoZoneZip::maxEntries + 1), { QStringLiteral(".json") }, limit, errorString).isEmpty());
    QCOMPARE(errorString, QStringLiteral("The zip archive has too many files."));
    QVERIFY(GeoZoneZip::largestFile(zipArchive("readme.txt", bomb, 1000), { QStringLiteral(".json") }, limit, errorString).isEmpty());
    QCOMPARE(errorString, QStringLiteral("The zip archive has no geozone file."));

    // The NATS page links the datasets of the current and the next AIRAC cycle
    const QByteArray page = "<!DOCTYPE html><html><body>"
        "<a href=\"/cms-nats/export/sites/default/en/Publications/digital-datasets/UAS_AREA_1/EG_UAS_FR_DS_AREA1_FULL_20261029_XML.zip\">XML</a>"
        "<a href=\"/cms-nats/export/sites/default/en/Publications/digital-datasets/UAS_AREA_1/EG_UAS_FR_DS_AREA1_FULL_20261126_XML.zip\">XML</a>"
        "</body></html>";
    const QString pageUrl = QStringLiteral("https://nats-uk.ead-it.com/cms-nats/opencms/en/Publications/digital-datasets/");
    QVERIFY(GeoZoneProviders::datasetLink(pageUrl, page, QDate(2026, 11, 1)).endsWith(QStringLiteral("FULL_20261029_XML.zip")));
    QVERIFY(GeoZoneProviders::datasetLink(pageUrl, page, QDate(2026, 11, 26)).startsWith(QStringLiteral("https://nats-uk.ead-it.com/cms-nats/export/")));
    QVERIFY(GeoZoneProviders::datasetLink(pageUrl, page, QDate(2026, 11, 26)).endsWith(QStringLiteral("FULL_20261126_XML.zip")));
    QVERIFY(GeoZoneProviders::datasetLink(pageUrl, page, QDate(2026, 1, 1)).endsWith(QStringLiteral("FULL_20261029_XML.zip")));
    QVERIFY(GeoZoneProviders::datasetLink(pageUrl, aixmBytes, QDate(2026, 11, 1)).isEmpty());

    // Austro Control lists its datasets with the dates they take effect, the dataset in effect is taken but not a
    // test dataset
    const QByteArray austriaPage = "<!DOCTYPE html><html><body><table><tbody>"
        "<tr class=\"avtext\"><td class=\"avtext\"><a href=\"/jart/prj3/ac/releases/de/upload/Dronespace/UASZones-ACG-20260826-131201-full-production_LOR57_LOR20.zip\" class=\"avtext\">"
        "<span class=\"avtext\">Geografische UAS-Gebiete</span></a></td><td class=\"avtext\"><span class=\"avtext\">01.09.2026</span></td>"
        "<td class=\"avtext\"><span class=\"avtext\">09.09.2026</span></td><td class=\"avtext\"><span class=\"avtext\">21.0</span></td></tr>"
        "<tr class=\"avtext\"><td class=\"avtext\"><a href=\"/jart/prj3/ac/releases/de/upload/Dronespace/UASZones-ACG-20261001-082127-full-production.zip\" class=\"avtext\">"
        "<span class=\"avtext\">Geografische UAS-Gebiete</span></a></td><td class=\"avtext\"><span class=\"avtext\">1.10.2026</span></td>"
        "<td class=\"avtext\"><span class=\"avtext\">24.10.2026</span></td><td class=\"avtext\"><span class=\"avtext\">25.0</span></td></tr>"
        "<tr class=\"avtext\"><td class=\"avtext\"><a href=\"/jart/prj3/ac/releases/de/upload/Dronespace/UASZones-ACG-20261021-130632-full-production.zip\" class=\"avtext\">"
        "<span class=\"avtext\">Geografische UAS-Gebiete</span></a></td><td class=\"avtext\"><span class=\"avtext\">25.10.2026</span></td>"
        "<td class=\"avtext\"><span class=\"avtext\">bis auf Weiteres</span></td><td class=\"avtext\"><span class=\"avtext\">26.0</span></td></tr>"
        "<tr class=\"avtext\"><td class=\"avtext\"><a href=\"/jart/prj3/ac/releases/de/upload/Dronespace/UASZones-ACG-20261030-082738-full-test_DCC.zip\" class=\"avtext\">"
        "<span class=\"avtext\">Geografische UAS-Gebiete</span></a></td><td class=\"avtext\"><span class=\"avtext\">01.11.2026</span></td>"
        "<td class=\"avtext\"><span class=\"avtext\">02.11.2026</span></td><td class=\"avtext\"><span class=\"avtext\">27.0</span></td></tr>"
        "</tbody></table></body></html>";
    const QString austriaUrl = QStringLiteral("https://www.austrocontrol.at/luftfahrtbehoerde/lizenzen__bewilligungen/drohnen/geografische_zonen");
    QCOMPARE(GeoZoneProviders::datasetLink(austriaUrl, austriaPage, QDate(2026, 9, 5)),
             QStringLiteral("https://www.austrocontrol.at/jart/prj3/ac/releases/de/upload/Dronespace/UASZones-ACG-20260826-131201-full-production_LOR57_LOR20.zip"));
    QVERIFY(GeoZoneProviders::datasetLink(austriaUrl, austriaPage, QDate(2026, 10, 24)).endsWith(QStringLiteral("UASZones-ACG-20261001-082127-full-production.zip")));
    QVERIFY(GeoZoneProviders::datasetLink(austriaUrl, austriaPage, QDate(2026, 10, 25)).endsWith(QStringLiteral("UASZones-ACG-20261021-130632-full-production.zip")));
    QVERIFY(GeoZoneProviders::datasetLink(austriaUrl, austriaPage, QDate(2026, 11, 10)).endsWith(QStringLiteral("UASZones-ACG-20261021-130632-full-production.zip")));
    QCOMPARE(GeoZoneProviders::formatHint(austriaUrl), GeoZoneParser::Format::ED269);

    // The ministry's page links the current file of the Dutch zones, without a date
    const QByteArray netherlandsPage = "<!DOCTYPE html><html lang=\"nl\"><body><p>Deze geografische zones zijn ook te downloaden via een "
        "<a aria-label=\"JSON-bestand (externe website)\" class=\"link-with-icon\" rel=\"external\" "
        "href=\"https://www.nieuwsienw.nl/api/documents/downloadfile?sectionid=178144&amp;fileid=1794622&amp;forcedownload=true\">JSON-bestand</a> "
        "(versie februari 2026).</p></body></html>";
    const QString netherlandsUrl = QStringLiteral("https://www.luchtvaartindetoekomst.nl/onderwerpen/o/onbemande-luchtvaart");
    QCOMPARE(GeoZoneProviders::datasetLink(netherlandsUrl, netherlandsPage, QDate(2026, 11, 10)),
             QStringLiteral("https://www.nieuwsienw.nl/api/documents/downloadfile?sectionid=178144&fileid=1794622&forcedownload=true"));
    QCOMPARE(GeoZoneProviders::formatHint(netherlandsUrl), GeoZoneParser::Format::ED269);

    // Area feeds of providers serving zones by area
    const QGeoRectangle frankfurt(QGeoCoordinate(50.15, 8.55), QGeoCoordinate(50.0, 8.75));
    const QString dipulUrl = GeoZoneProviders::areaFeedUrl(QStringLiteral("dipul"), frankfurt);
    QVERIFY(dipulUrl.contains(QStringLiteral("dipul:wohngrundstuecke")));
    QVERIFY(dipulUrl.endsWith(QStringLiteral("&BBOX=8.55000,50.00000,8.75000,50.15000")));
    QCOMPARE(GeoZoneProviders::formatHint(dipulUrl), GeoZoneParser::Format::Dipul);
    QVERIFY(GeoZoneProviders::areaFeedUrl(QStringLiteral("france"), frankfurt).contains(QStringLiteral("BBOX=8.55000,50.00000,8.75000,50.15000,EPSG:4326")));
    QVERIFY(GeoZoneProviders::areaFeedUrl(QStringLiteral("dipul"), QGeoRectangle(QGeoCoordinate(51, 8), QGeoCoordinate(49, 10))).isEmpty());
    QVERIFY(GeoZoneProviders::areaFeedUrl(QStringLiteral("unknown"), frankfurt).isEmpty());
    // ENAIRE's infrastructure zones from the feature service of its AIS, in pages, read as ED-318
    const QGeoRectangle madrid(QGeoCoordinate(40.5, -3.8), QGeoCoordinate(40.3, -3.6));
    const QString enaireUrl = GeoZoneProviders::areaFeedUrl(QStringLiteral("enaire-infra"), madrid);
    QVERIFY2(enaireUrl.startsWith(QStringLiteral("https://servais.enaire.es/insigniads/rest/services/NSF_SRV/SRV_UAS_ZG_data_V2/FeatureServer/0/query?")) &&
             enaireUrl.contains(QStringLiteral("geometry=-3.80000,40.30000,-3.60000,40.50000")), qPrintable(enaireUrl));
    QCOMPARE(GeoZoneProviders::formatHint(enaireUrl), GeoZoneParser::Format::ED318);
    QCOMPARE(GeoZoneProviders::serviceId(enaireUrl), QStringLiteral("enaire-infra"));
    QCOMPARE(GeoZoneProviders::feedArea(enaireUrl), madrid);
    QCOMPARE(GeoZoneProviders::paging(enaireUrl, R"({"type":"FeatureCollection","features":[)", R"(],"exceededTransferLimit":true,"properties":{"exceededTransferLimit":true}})").scheme,
             GeoZoneProviders::Paging::Scheme::ArcGisOffset);
    QCOMPARE(GeoZoneProviders::formatHint(QStringLiteral("https://aip.enaire.es/recursos/descargas/ZGUAS/ZGUAS_Aero.zip")), GeoZoneParser::Format::ED318);

    // Downloading an area adds nothing when a feed of the provider covers it, whatever the rounding of its bounds
    QCOMPARE(GeoZoneProviders::feedArea(dipulUrl), frankfurt);
    QVERIFY(!GeoZoneProviders::feedArea(QStringLiteral("https://example.com/zones.json?bbox=1,2")).isValid());
    const QString franceUrl = GeoZoneProviders::areaFeedUrl(QStringLiteral("france"), frankfurt);
    const QGeoRectangle unrounded(QGeoCoordinate(50.150004, 8.549996), QGeoCoordinate(49.999996, 8.750004));
    const QGeoRectangle insideArea(QGeoCoordinate(50.1, 8.6), QGeoCoordinate(50.05, 8.7));
    const QGeoRectangle larger(QGeoCoordinate(50.2, 8.55), QGeoCoordinate(50.0, 8.75));
    QVERIFY(GeoZoneProviders::areaDownloaded(QStringLiteral("dipul"), { GeoZoneProviders::areaFeedUrl(QStringLiteral("dipul"), unrounded) }, unrounded));
    QVERIFY(GeoZoneProviders::areaDownloaded(QStringLiteral("dipul"), { dipulUrl }, insideArea));
    QVERIFY(!GeoZoneProviders::areaDownloaded(QStringLiteral("dipul"), { dipulUrl }, larger));
    QVERIFY(!GeoZoneProviders::areaDownloaded(QStringLiteral("france"), { dipulUrl }, insideArea));
    QVERIFY(GeoZoneProviders::areaDownloaded(QStringLiteral("france"), { dipulUrl, franceUrl }, insideArea));
#ifndef GEOZONE_CORE_TEST
    GeoZoneManager manager(QTemporaryDir().path());
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);
    QVERIFY(!manager.addArea(QStringLiteral("dipul"), QGeoRectangle(QGeoCoordinate(51, 8), QGeoCoordinate(49, 10))));
    QCOMPARE(failedSpy.count(), 1);
#endif
}

#ifndef GEOZONE_CORE_TEST
void GeoZoneParserTest::_testLiveNationalFeeds()
{
    // Downloads the real services, only when asked since it depends on them
    if (qEnvironmentVariableIsEmpty("QGC_GEOZONE_LIVE_TEST")) {
        QSKIP("Set QGC_GEOZONE_LIVE_TEST to download the national services");
    }
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.path());
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);
    for (const QVariant &preset : GeoZoneManager::feedPresets()) {
        const QString url = preset.toMap().value(QStringLiteral("url")).toString();
        if (url.contains(QStringLiteral("uas-betrieb.de")) || url.contains(QStringLiteral("ead-it.com")) || url.contains(QStringLiteral("tfr.faa.gov"))) {
            manager.repository()->addUrl(url);
        }
    }
    QVERIFY(manager.addArea(QStringLiteral("dipul"), QGeoRectangle(QGeoCoordinate(50.15, 8.55), QGeoCoordinate(50.0, 8.75))));
    QVERIFY(manager.addArea(QStringLiteral("france"), QGeoRectangle(QGeoCoordinate(48.9, 2.25), QGeoCoordinate(48.8, 2.45))));
    QTRY_VERIFY_WITH_TIMEOUT(!manager.repository()->busy(), 120000);
    for (const QVariantList &failure : std::as_const(failedSpy)) {
        qWarning() << failure;
    }
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(manager.sources()->count(), 5);
    for (int i = 0; i < manager.sources()->count(); i++) {
        const GeoZoneSource *const source = manager.sources()->value<GeoZoneSource*>(i);
        qDebug() << source->name() << source->detailsText() << source->warningText();
        QVERIFY2(source->zoneCount() > 0, qPrintable(source->key()));
    }

    // The FAA TFRs get the times and altitudes of their NOTAMs
    const QList<GeoZone*> &zones = manager.zoneList();
    const qsizetype tfrs = std::count_if(zones.cbegin(), zones.cend(), [](const GeoZone *zone) { return zone->country() == QStringLiteral("USA"); });
    const qsizetype detailed = std::count_if(zones.cbegin(), zones.cend(), [](const GeoZone *zone) {
        return (zone->country() == QStringLiteral("USA")) && !zone->data().applicability.isEmpty();
    });
    qDebug() << "TFRs" << tfrs << "with NOTAM times" << detailed;
    QVERIFY((tfrs == 0) || (detailed > (tfrs / 2)));
}
#endif

void GeoZoneParserTest::_testParseErrors()
{
    GeoZoneParser::Result result;
    QString errorString;

    QVERIFY(!GeoZoneParser::parseFile(QStringLiteral("/nonexistent/geozones.json"), result, errorString));
    QVERIFY(!errorString.isEmpty());

    QVERIFY(!GeoZoneParser::parse("{ \"features\": [", result, errorString));
    QVERIFY(!errorString.isEmpty());

    QVERIFY(!GeoZoneParser::parse(R"({ "type": "Polygon", "coordinates": [] })", result, errorString));
    QVERIFY(!errorString.isEmpty());

    QVERIFY(!GeoZoneParser::parse(R"({ "features": [ { "identifier": "X", "restriction": "PROHIBITED", "geometry": [] } ] })", result, errorString));
    QCOMPARE(result.skippedFeatures, 1);

    // Other feature lists aren't taken for ED-269
    QVERIFY(!GeoZoneParser::parse(R"({ "features": [ { "name": "Somewhere", "geometry": { "type": "Point", "coordinates": [8, 47] } } ] })", result, errorString));
    QVERIFY(errorString.startsWith(QStringLiteral("The file is not in a supported geozone format.")));

    // Latitude out of range
    QVERIFY(!GeoZoneParser::parse(R"({ "type": "FeatureCollection", "features": [ { "type": "Feature", "properties": { "type": "PROHIBITED" },
        "geometry": { "type": "Polygon", "coordinates": [ [ [0, 95], [1, 95], [1, 96], [0, 95] ] ] } } ] })", result, errorString));
    QCOMPARE(result.skippedFeatures, 1);
}

void GeoZoneParserTest::_testRingValidation_data()
{
    QTest::addColumn<QJsonObject>("projection");
    QTest::addColumn<int>("vertices");          ///< Of the outer ring, 0 when the volume is rejected
    QTest::addColumn<int>("holes");
    QTest::addColumn<QString>("issue");
    QTest::addColumn<QGeoCoordinate>("inside");
    QTest::addColumn<QGeoCoordinate>("outside");

    const auto polygon = [](const QList<QList<QPointF>> &rings) {
        QJsonArray coordinates;
        for (const QList<QPointF> &ring : rings) {
            QJsonArray positions;
            for (const QPointF &vertex : ring) {
                positions.append(QJsonArray{ vertex.x(), vertex.y() });
            }
            coordinates.append(positions);
        }
        return QJsonObject{ { "type", "Polygon" }, { "coordinates", coordinates } };
    };
    const auto circle = [](double radius) { return QJsonObject{ { "type", "Circle" }, { "center", QJsonArray{ 8, 47 } }, { "radius", radius } }; };
    const QList<QPointF> square = { { 8, 47 }, { 8.01, 47 }, { 8.01, 47.01 }, { 8, 47.01 } };
    const QGeoCoordinate none;
    const QString fewPoints = QStringLiteral("Polygon with fewer than 3 distinct points ignored");
    const QString noArea = QStringLiteral("Polygon without area ignored");
    const QString pole = QStringLiteral("Polygon touching or enclosing a pole ignored");
    const QString crossing = QStringLiteral("Polygon boundary crossing itself, the zone covers all areas it encloses");

    // Rings are stored unclosed, without consecutive duplicates closer than 1 cm, the outer one counterclockwise
    QTest::newRow("closed") << polygon({ square + QList<QPointF>{ square.first() } }) << 4 << 0 << QString() << QGeoCoordinate(47.005, 8.005) << none;
    QTest::newRow("unclosed") << polygon({ square }) << 4 << 0 << QString() << QGeoCoordinate(47.005, 8.005) << none;
    QTest::newRow("duplicates") << polygon({ { { 8, 47 }, { 8, 47 }, { 8.01, 47 }, { 8.01, 47.00000001 }, { 8.01, 47.01 }, { 8, 47.01 }, { 8, 47 }, { 8, 47 } } })
                                << 4 << 0 << QString() << QGeoCoordinate(47.005, 8.005) << none;
    QTest::newRow("clockwise") << polygon({ { square[3], square[2], square[1], square[0] } }) << 4 << 0 << QString() << QGeoCoordinate(47.005, 8.005) << none;
    // Holes are clockwise, invalid ones are dropped and covered by the zone
    QTest::newRow("hole") << polygon({ square, { { 8.004, 47.004 }, { 8.006, 47.004 }, { 8.006, 47.006 }, { 8.004, 47.006 } } }) << 4 << 1 << QString()
                          << QGeoCoordinate(47.002, 8.002) << QGeoCoordinate(47.005, 8.005);
    QTest::newRow("invalid hole") << polygon({ square, { { 8.004, 47.004 }, { 8.006, 47.004 }, { 8.004, 47.004 } } }) << 4 << 0
                                  << QStringLiteral("Invalid polygon hole ignored, the zone covers it") << QGeoCoordinate(47.004, 8.005) << none;
    // Degenerate rings
    QTest::newRow("two points") << polygon({ { { 8, 47 }, { 8.01, 47 }, { 8.01, 47 } } }) << 0 << 0 << fewPoints << none << none;
    QTest::newRow("one point") << polygon({ { { 8, 47 }, { 8, 47.00000001 }, { 8, 47 } } }) << 0 << 0 << fewPoints << none << none;
    QTest::newRow("collinear") << polygon({ { { 8, 47 }, { 8.01, 47 }, { 8.02, 47 } } }) << 0 << 0 << noArea << none << none;
    QTest::newRow("folded back") << polygon({ { { 8, 47 }, { 8.02, 47 }, { 8.01, 47 } } }) << 0 << 0 << noArea << none << none;
    QTest::newRow("tiny") << polygon({ { { 8, 47 }, { 8.00001, 47 }, { 8, 47.00001 } } }) << 0 << 0 << noArea << none << none;
    // The poles: a ring through one, and one winding around one
    QTest::newRow("pole vertex") << polygon({ { { 0, 80 }, { 90, 90 }, { 180, 80 } } }) << 0 << 0 << pole << none << none;
    QTest::newRow("around pole") << polygon({ { { -180, 85 }, { -90, 85 }, { 0, 85 }, { 90, 85 } } }) << 0 << 0 << pole << none << none;
    QTest::newRow("south polar") << polygon({ { { 0, -89.95 }, { 1, -89.95 }, { 1, -89 } } }) << 0 << 0 << pole << none << none;
    // Crossing rings are kept and filled with the non-zero winding rule: both lobes of a bow tie are inside, and so is
    // the center of a pentagram which its boundary winds around twice
    QTest::newRow("bow tie") << polygon({ { { 8, 47 }, { 8.01, 47.01 }, { 8.01, 47 }, { 8, 47.01 } } }) << 4 << 0 << crossing
                             << QGeoCoordinate(47.005, 8.009) << QGeoCoordinate(47.002, 8.005);
    QList<QPointF> pentagram;
    for (int i = 0; i < 5; i++) {
        const double angle = qDegreesToRadians(90.0 + (i * 144.0));
        pentagram.append(QPointF(8 + (0.01 * std::cos(angle)), 47 + (0.01 * std::sin(angle))));
    }
    QTest::newRow("pentagram") << polygon({ pentagram }) << 5 << 0 << crossing << QGeoCoordinate(47, 8) << QGeoCoordinate(47.005663, 8.004114);
    // Circles need a positive radius
    QTest::newRow("circle") << circle(100) << 0 << 0 << QString() << QGeoCoordinate(47, 8) << none;
    QTest::newRow("zero radius") << circle(0) << 0 << 0 << QStringLiteral("Circle without a valid radius ignored") << none << none;
    QTest::newRow("negative radius") << circle(-100) << 0 << 0 << QStringLiteral("Circle without a valid radius ignored") << none << none;
}

void GeoZoneParserTest::_testRingValidation()
{
    QFETCH(QJsonObject, projection);
    QFETCH(int, vertices);
    QFETCH(int, holes);
    QFETCH(QString, issue);
    QFETCH(QGeoCoordinate, inside);
    QFETCH(QGeoCoordinate, outside);

    // With a valid zone, so that the file loads when the tested one is rejected
    const QJsonObject tested{ { "identifier", "TESTED" }, { "restriction", "PROHIBITED" },
                              { "geometry", QJsonArray{ QJsonObject{ { "horizontalProjection", projection } } } } };
    const QJsonArray features{ tested, _circleFeature(QStringLiteral("VALID"), QStringLiteral("PROHIBITED"), QGeoCoordinate(40, 8), 100) };
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(QJsonDocument(QJsonObject{ { "features", features } }).toJson(QJsonDocument::Compact), result, errorString), qPrintable(errorString));

    const bool rejected = !inside.isValid();
    QCOMPARE(result.zones.count(), rejected ? 1 : 2);
    QCOMPARE(result.skippedFeatures, rejected ? 1 : 0);
    const QMap<QString, int> issues = issue.isEmpty() ? QMap<QString, int>() : QMap<QString, int>{ { issue, 1 } };
    QCOMPARE(result.issues, issues);
    if (rejected) {
        return;
    }

    const GeoZone::Data &data = result.zones.first();
    QCOMPARE(data.issues, issue.isEmpty() ? QStringList() : QStringList{ issue });
    const GeoZone::Volume &volume = data.volumes.first();
    if (!volume.isCircle) {
        QCOMPARE(volume.rings.first().count(), vertices);
        QCOMPARE(volume.rings.count(), holes + 1);
        QVERIFY(volume.rings.first().first() != volume.rings.first().last());
        if (issue != QStringLiteral("Polygon boundary crossing itself, the zone covers all areas it encloses")) {
            QVERIFY(GeoZoneGeometry::signedArea(volume.rings.first()) > 0);
        }
        for (qsizetype i = 1; i < volume.rings.count(); i++) {
            QVERIFY(GeoZoneGeometry::signedArea(volume.rings[i]) < 0);
        }
    }
    const GeoZone zone(data);
    QVERIFY(zone.contains(inside));
    QVERIFY(!outside.isValid() || !zone.contains(outside));

    // The checks are kept by the parse cache
    QByteArray bytes;
    {
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setVersion(GeoZoneParseCache::streamVersion);
        stream << result;
    }
    GeoZoneParser::Result readBack;
    QDataStream stream(bytes);
    stream.setVersion(GeoZoneParseCache::streamVersion);
    stream >> readBack;
    QCOMPARE(stream.status(), QDataStream::Ok);
    QCOMPARE(readBack.issues, result.issues);
    QCOMPARE(readBack.skippedFeatures, result.skippedFeatures);
    QCOMPARE(readBack.zones.first().volumes.first().rings, volume.rings);
}

void GeoZoneParserTest::_testEdgeInterpolation_data()
{
    QTest::addColumn<QByteArray>("document");
    QTest::addColumn<bool>("greatCircles");

    // The same zone between 60N and 61N, 111 km wide, in each format
    const QList<QGeoCoordinate> ring = { QGeoCoordinate(60, 10), QGeoCoordinate(60, 12), QGeoCoordinate(61, 12), QGeoCoordinate(61, 10) };
    QJsonArray positions;
    for (const QGeoCoordinate &vertex : ring + QList<QGeoCoordinate>{ ring.first() }) {
        positions.append(QJsonArray{ vertex.longitude(), vertex.latitude() });
    }
    const QJsonObject polygon{ { "type", "Polygon" }, { "coordinates", QJsonArray{ positions } } };
    const auto json = [](const QJsonObject &root) { return QJsonDocument(root).toJson(QJsonDocument::Compact); };

    // GeoJSON lines are straight in latitude and longitude (RFC 7946 3.1.1), ED-269 polygons are GeoJSON polygons
    QTest::newRow("ED-269") << json(QJsonObject{ { "features", QJsonArray{ _polygonFeature(QStringLiteral("Z"), QStringLiteral("PROHIBITED"), ring) } } }) << false;
    QTest::newRow("ED-318") << json(QJsonObject{ { "type", "FeatureCollection" }, { "features", QJsonArray{ QJsonObject{
        { "type", "Feature" }, { "properties", QJsonObject{ { "identifier", "Z" }, { "type", "PROHIBITED" } } }, { "geometry", polygon } } } } }) << false;

    // AIXM geodesic strings are great circles, line string segments in EPSG:4326 straight in latitude and longitude
    const QString aixm = QStringLiteral(R"(<message:AIXMBasicMessage xmlns:message="http://www.aixm.aero/schema/5.1/message"
        xmlns:aixm="http://www.aixm.aero/schema/5.1" xmlns:gml="http://www.opengis.net/gml/3.2"><message:hasMember><aixm:Airspace gml:id="A">
        <aixm:timeSlice><aixm:AirspaceTimeSlice gml:id="T"><aixm:type>P</aixm:type><aixm:designator>Z</aixm:designator>
        <aixm:geometryComponent><aixm:AirspaceGeometryComponent><aixm:theAirspaceVolume><aixm:AirspaceVolume gml:id="V">
        <aixm:horizontalProjection><aixm:Surface gml:id="S" srsName="urn:ogc:def:crs:EPSG::4326"><gml:patches><gml:PolygonPatch><gml:exterior>
        <gml:Ring><gml:curveMember><gml:Curve gml:id="C"><gml:segments><gml:%1><gml:posList>60 10 60 12 61 12 61 10 60 10</gml:posList></gml:%1>
        </gml:segments></gml:Curve></gml:curveMember></gml:Ring></gml:exterior></gml:PolygonPatch></gml:patches></aixm:Surface></aixm:horizontalProjection>
        </aixm:AirspaceVolume></aixm:theAirspaceVolume></aixm:AirspaceGeometryComponent></aixm:geometryComponent>
        </aixm:AirspaceTimeSlice></aixm:timeSlice></aixm:Airspace></message:hasMember></message:AIXMBasicMessage>)");
    QTest::newRow("AIXM geodesic string") << aixm.arg(QStringLiteral("GeodesicString")).toUtf8() << true;
    QTest::newRow("AIXM line string") << aixm.arg(QStringLiteral("LineStringSegment")).toUtf8() << false;

    // FAA TFRs are GeoJSON, whose NOTAM details give the vertices of the merged areas as great circle points
    const QJsonObject tfrRoot{ { "type", "FeatureCollection" }, { "totalFeatures", 1 }, { "timeStamp", "2026-10-31T12:00:00Z" }, { "features", QJsonArray{ QJsonObject{
        { "type", "Feature" }, { "geometry", polygon },
        { "properties", QJsonObject{ { "NOTAM_KEY", "6/0001-1-FDC-F" }, { "TITLE", "TEST" }, { "LAST_MODIFICATION_DATETIME", "202610311200" } } } } } } };
    QByteArray vertices;
    for (const QGeoCoordinate &vertex : ring + QList<QGeoCoordinate>{ ring.first() }) {
        vertices += QStringLiteral("<Avx><codeDatum>WGE</codeDatum><codeType>%3</codeType><geoLat>%1N</geoLat><geoLong>%2E</geoLong></Avx>")
                        .arg(vertex.latitude()).arg(vertex.longitude()).toUtf8();
    }
    const QByteArray detail = R"(<XNOTAM-Update><Group><Add><Not><TfrNot><TFRAreaGroup><aseTFRArea><txtName>Area A</txtName>
        <codeDistVerUpper>ALT</codeDistVerUpper><valDistVerUpper>3000</valDistVerUpper><uomDistVerUpper>FT</uomDistVerUpper>
        <codeDistVerLower>HEI</codeDistVerLower><valDistVerLower>0</valDistVerLower><uomDistVerLower>FT</uomDistVerLower></aseTFRArea>
        <abdMergedArea><AbdUid><AseUid><codeType>RAS</codeType><codeId>1</codeId></AseUid></AbdUid>)" + vertices + R"(</abdMergedArea>
        </TFRAreaGroup></TfrNot></Not></Add></Group></XNOTAM-Update>)";
    const auto tfr = [&tfrRoot, &detail](const QString &codeType) {
        return GeoZoneParser::composite(tfrRoot, { { QStringLiteral("6/0001"), { QStringLiteral("202610311200"), QString::fromUtf8(detail).arg(codeType).toUtf8() } } });
    };
    QTest::newRow("FAA TFR great circles") << tfr(QStringLiteral("GRC")) << true;
    QTest::newRow("FAA TFR rhumb lines") << tfr(QStringLiteral("RHL")) << false;
    QTest::newRow("FAA TFR without details") << json(tfrRoot) << false;
}

void GeoZoneParserTest::_testEdgeInterpolation()
{
    QFETCH(QByteArray, document);
    QFETCH(bool, greatCircles);

    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(document, result, errorString), qPrintable(errorString));
    QCOMPARE(result.zones.count(), 1);
    QVERIFY(result.issues.isEmpty());
    const GeoZone::Ring &published = result.zones.first().volumes.first().rings.first();
    const GeoZone zone(result.zones.first());
    if (greatCircles) {
        // The east-west edges are great circles 420 m and 411 m north of the parallels in the middle, which get points
        // on them, the meridians none
        QVERIFY((published.count() > 20) && (published.count() < 200));
        QVERIFY(!zone.contains(QGeoCoordinate(60.003, 11)));
        QVERIFY(zone.contains(QGeoCoordinate(60.0045, 11)));
        QVERIFY(zone.contains(QGeoCoordinate(61.0033, 11)));
        QVERIFY(!zone.contains(QGeoCoordinate(61.0045, 11)));
        QVERIFY(qAbs(zone.box().north() - 61.0037) < 0.0001);
    } else {
        // Along the parallels
        QCOMPARE(published.count(), 4);
        QVERIFY(!zone.contains(QGeoCoordinate(59.9999, 11)));
        QVERIFY(zone.contains(QGeoCoordinate(60.0001, 11)));
        QVERIFY(zone.contains(QGeoCoordinate(60.9999, 11)));
        QVERIFY(!zone.contains(QGeoCoordinate(61.0001, 11)));
        QCOMPARE(zone.box().north(), 61.0);
    }
    QCOMPARE(zone.box().south(), 60.0);
}

void GeoZoneParserTest::_testTfrCircles()
{
    // A TFR of 1 NM around a point, published as the 36-sided polygon of its merged area like the FAA does, whose sides
    // are 7 m inside the circle in their middle
    const QGeoCoordinate center(26.07194444, -97.16361111);
    QJsonArray positions;
    QByteArray vertices;
    for (int i = 0; i <= 36; i++) {
        const QGeoCoordinate vertex = center.atDistanceAndAzimuth(1852, -(i % 36) * 10.0);
        positions.append(QJsonArray{ vertex.longitude(), vertex.latitude() });
        vertices += QStringLiteral("<Avx><codeDatum>WGE</codeDatum><codeType>GRC</codeType><geoLat>%1N</geoLat><geoLong>%2W</geoLong></Avx>")
                        .arg(vertex.latitude(), 0, 'f', 8).arg(-vertex.longitude(), 0, 'f', 8).toUtf8();
    }
    const QJsonObject tfrRoot{ { "type", "FeatureCollection" }, { "totalFeatures", 1 }, { "timeStamp", "2026-10-31T12:00:00Z" }, { "features", QJsonArray{ QJsonObject{
        { "type", "Feature" }, { "geometry", QJsonObject{ { "type", "Polygon" }, { "coordinates", QJsonArray{ positions } } } },
        { "properties", QJsonObject{ { "NOTAM_KEY", "6/0002-1-FDC-F" }, { "TITLE", "CIRCLE" }, { "LAST_MODIFICATION_DATETIME", "202610311200" } } } } } } };
    // The area is based on a shape which is the circle
    const QByteArray detail = R"(<XNOTAM-Update><Group><Add><Not><TfrNot><TFRAreaGroup><aseTFRArea>
        <AseUid><codeType>RAS</codeType><codeId>300</codeId></AseUid><txtName>Area A</txtName>
        <codeDistVerUpper>ALT</codeDistVerUpper><valDistVerUpper>3000</valDistVerUpper><uomDistVerUpper>FT</uomDistVerUpper>
        <codeDistVerLower>HEI</codeDistVerLower><valDistVerLower>0</valDistVerLower><uomDistVerLower>FT</uomDistVerLower></aseTFRArea>
        <Aac><AacUid><AseUidChi><codeType>RAS</codeType><codeId>300</codeId></AseUidChi><AseUidPar><codeType>PART</codeType><codeId>301</codeId></AseUidPar></AacUid>
        <codeType>BOM</codeType><codeOpr>BASE</codeOpr></Aac>
        <abdMergedArea><AbdUid><AseUid><codeType>RAS</codeType><codeId>300</codeId></AseUid></AbdUid>)" + vertices + R"(</abdMergedArea>
        <aseShapes><AseUid><codeType>PART</codeType><codeId>301</codeId></AseUid><Abd><AbdUid><AseUid><codeType>PART</codeType><codeId>301</codeId></AseUid></AbdUid>
        <Avx><codeType>CIR</codeType><geoLat>26.07194444N</geoLat><geoLong>097.16361111W</geoLong><codeDatum>WGE</codeDatum>
        <valRadiusArc>RADIUS</valRadiusArc><uomRadiusArc>NM</uomRadiusArc></Avx></Abd></aseShapes>
        </TFRAreaGroup></TfrNot></Not></Add></Group></XNOTAM-Update>)";
    const auto withDetail = [&tfrRoot, &detail](const QByteArray &radius) {
        return GeoZoneParser::composite(tfrRoot, { { QStringLiteral("6/0002"), { QStringLiteral("202610311200"), QByteArray(detail).replace("RADIUS", radius) } } });
    };
    const auto parse = [](const QByteArray &document) {
        GeoZoneParser::Result result;
        QString errorString;
        if (!GeoZoneParser::parse(document, result, errorString)) {
            qWarning() << errorString;
        }
        return result;
    };
    // 3 m inside the circle in the middle of a side of the polygon
    const QGeoCoordinate inside = center.atDistanceAndAzimuth(1849, 5);

    // The circle of the NOTAM replaces the polygon, with the limits of its area
    const GeoZoneParser::Result circular = parse(withDetail("1.0"));
    QCOMPARE(circular.zones.count(), 1);
    QCOMPARE(circular.zones.first().volumes.count(), 1);
    const GeoZone::Volume &circle = circular.zones.first().volumes.first();
    QVERIFY(circle.isCircle);
    QVERIFY(circle.rings.isEmpty());
    QCOMPARE(circle.center, center);
    QCOMPARE(circle.radius, 1852.0);
    QCOMPARE(circle.upperLimit, 3000.0);
    const GeoZone circleZone(circular.zones.first());
    QVERIFY(circleZone.contains(inside));
    QVERIFY(!circleZone.contains(center.atDistanceAndAzimuth(1853, 5)));
    QCOMPARE(circleZone.exclusionCircles().count(), 1);

    // The polygon stays without the details, and when the circle of the NOTAM isn't the one it follows
    for (const QByteArray &document : { QJsonDocument(tfrRoot).toJson(QJsonDocument::Compact), withDetail("1.5") }) {
        const GeoZoneParser::Result polygonal = parse(document);
        QCOMPARE(polygonal.zones.count(), 1);
        const GeoZone::Volume &polygon = polygonal.zones.first().volumes.first();
        QVERIFY(!polygon.isCircle);
        QCOMPARE(polygon.rings.first().count(), 36);
        QVERIFY(!GeoZone(polygonal.zones.first()).contains(inside));
    }
}

void GeoZoneParserTest::_testAixmArcs()
{
    // Arcs get points close enough that the chords stay within 1 m of them: a 20 NM arc every 0.84 degrees
    const QString aixm = QStringLiteral(R"(<message:AIXMBasicMessage xmlns:message="http://www.aixm.aero/schema/5.1/message"
        xmlns:aixm="http://www.aixm.aero/schema/5.1" xmlns:gml="http://www.opengis.net/gml/3.2"><message:hasMember><aixm:Airspace gml:id="A">
        <aixm:timeSlice><aixm:AirspaceTimeSlice gml:id="T"><aixm:type>D</aixm:type><aixm:designator>ARC</aixm:designator>
        <aixm:geometryComponent><aixm:AirspaceGeometryComponent><aixm:theAirspaceVolume><aixm:AirspaceVolume gml:id="V">
        <aixm:horizontalProjection><aixm:Surface gml:id="S"><gml:patches><gml:PolygonPatch><gml:exterior><gml:Ring><gml:curveMember>
        <gml:Curve gml:id="C"><gml:segments>
        <gml:GeodesicString><gml:posList>52 0 52.33287 0</gml:posList></gml:GeodesicString>
        <gml:ArcByCenterPoint numArc="1"><gml:pos>52 0</gml:pos><gml:radius uom="[nmi_i]">20</gml:radius>
        <gml:startAngle uom="deg">0</gml:startAngle><gml:endAngle uom="deg">90</gml:endAngle></gml:ArcByCenterPoint>
        <gml:GeodesicString><gml:posList>52 0.54 52 0</gml:posList></gml:GeodesicString>
        </gml:segments></gml:Curve></gml:curveMember></gml:Ring></gml:exterior></gml:PolygonPatch></gml:patches></aixm:Surface>
        </aixm:horizontalProjection></aixm:AirspaceVolume></aixm:theAirspaceVolume></aixm:AirspaceGeometryComponent></aixm:geometryComponent>
        </aixm:AirspaceTimeSlice></aixm:timeSlice></aixm:Airspace></message:hasMember></message:AIXMBasicMessage>)");
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(aixm.toUtf8(), result, errorString), qPrintable(errorString));
    QCOMPARE(result.zones.count(), 1);
    const QList<QGeoCoordinate> vertices = GeoZone::toCoordinates(result.zones.first().volumes.first().rings.first());
    const QGeoCoordinate center(52, 0);
    const double radius = 20 * 1852;
    int arcVertices = 0;
    for (qsizetype i = 0; i < vertices.count(); i++) {
        const QGeoCoordinate &from = vertices[i];
        const QGeoCoordinate &to = vertices[(i + 1) % vertices.count()];
        if ((qAbs(center.distanceTo(from) - radius) > 0.01) || (qAbs(center.distanceTo(to) - radius) > 0.01)) {
            continue;
        }
        arcVertices++;
        const QGeoCoordinate middle = from.atDistanceAndAzimuth(from.distanceTo(to) / 2, from.azimuthTo(to));
        QVERIFY2((radius - center.distanceTo(middle)) <= GeoZoneGeometry::kGreatCircleToleranceMeters, qPrintable(QString::number(radius - center.distanceTo(middle))));
    }
    QVERIFY((arcVertices > 100) && (arcVertices < 200));
}

void GeoZoneParserTest::_testRejectedAixmGeometry()
{
    // AIXM volumes are checked like the others: a circle without radius leaves its airspace without volume
    QFile file(QStringLiteral(":/unittest/uk_aixm_test.xml"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QByteArray bytes = file.readAll();
    const QByteArray radius = "<gml:radius uom=\"[nmi_i]\">2</gml:radius>";
    const qsizetype circleRadius = bytes.lastIndexOf(radius);
    QVERIFY(circleRadius > bytes.indexOf("CircleByCenterPoint"));
    bytes.replace(circleRadius, radius.size(), "<gml:radius uom=\"[nmi_i]\">0</gml:radius>");

    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(bytes, result, errorString), qPrintable(errorString));
    QCOMPARE(result.zones.count(), 2);
    QCOMPARE(result.skippedFeatures, 1);
    QCOMPARE(result.issues, (QMap<QString, int>{ { QStringLiteral("Circle without a valid radius ignored"), 1 } }));
}

void GeoZoneParserTest::_testParserFuzz()
{
    // Mutated documents must load or fail cleanly, and loaded zones must be usable
    QRandomGenerator random(20261031);
    const QGeoCoordinate probe(47, 8);
    const QDateTime reference = _utc(QDate(2026, 6, 1), QTime(12, 0));
    int loadedCount = 0;
    for (const QString &name : { QStringLiteral("ed269_test.json"), QStringLiteral("ed318_test.json"), QStringLiteral("faa_tfr_test.json"), QStringLiteral("ch_zurich_ed269.json") }) {
        QFile file(QStringLiteral(":/unittest/") + name);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray original = file.readAll();
        const QJsonValue document = QJsonDocument::fromJson(original).isObject() ?
                                        QJsonValue(QJsonDocument::fromJson(original).object()) : QJsonValue(QJsonDocument::fromJson(original).array());
        const GeoZoneParser::Format hint = name.startsWith(QStringLiteral("faa")) ? GeoZoneParser::Format::FaaTfr : GeoZoneParser::Format::Unknown;

        for (int i = 0; i < 300; i++) {
            QByteArray bytes;
            if (i % 10 == 0) {
                // Raw corruption: truncation and flipped bytes
                bytes = original.left(random.bounded(static_cast<int>(original.size())));
                for (int flips = random.bounded(5); flips > 0 && !bytes.isEmpty(); flips--) {
                    bytes[random.bounded(static_cast<int>(bytes.size()))] = static_cast<char>(random.bounded(256));
                }
            } else {
                QJsonValue mutated = document;
                for (int mutations = 1 + random.bounded(4); mutations > 0; mutations--) {
                    mutated = _mutateJson(mutated, random);
                }
                bytes = mutated.isObject() ? QJsonDocument(mutated.toObject()).toJson(QJsonDocument::Compact) :
                        (mutated.isArray() ? QJsonDocument(mutated.toArray()).toJson(QJsonDocument::Compact) : QByteArray("1"));
            }

            GeoZoneParser::Result result;
            QString errorString;
            if (!GeoZoneParser::parse(bytes, result, errorString, hint)) {
                QVERIFY(!errorString.isEmpty());
                continue;
            }
            loadedCount++;
            for (const GeoZone::Data &data : std::as_const(result.zones)) {
                for (const GeoZone::Volume &volume : data.volumes) {
                    if (volume.isCircle) {
                        QVERIFY(volume.center.isValid() && std::isfinite(volume.radius) && (volume.radius > 0));
                    } else {
                        QVERIFY(!volume.rings.isEmpty() && (volume.rings.first().count() >= 3));
                    }
                    QVERIFY(!std::isinf(volume.lowerLimit) && !std::isinf(volume.upperLimit));
                }

                const GeoZone zone(data);
                QVERIFY(zone.boundingBox().isValid());
                for (const QGeoCoordinate &point : { probe, zone.center() }) {
                    const double distance = zone.distanceTo(point);
                    QVERIFY(distance >= 0);
                    QVERIFY(!zone.contains(point) || (distance == 0));
                }
                (void) zone.distanceTo(GeoZone::Position{ probe, 500, 400 }, 30);
                (void) zone.intersectsPath(QList<QGeoCoordinate>{ probe, probe.atDistanceAndAzimuth(5000, 45) });
                for (const QList<QGeoCoordinate> &polygon : zone.exclusionPolygons(25)) {
                    QVERIFY(polygon.count() >= 3);
                }
                (void) zone.exclusionCircles();
                (void) zone.isActiveDuring(reference, reference.addDays(2));
                (void) GeoZoneText::verticalLimits(zone.data());
                (void) GeoZoneSchedule::text(zone.data().applicability);
            }
        }
    }
    // Many mutations still yield documents with zones, so the zone functions are exercised too
    QVERIFY2(loadedCount > 400, qPrintable(QString::number(loadedCount)));
}

void GeoZoneParserTest::_testFuzzFindings()
{
    // Documents found by the fuzz targets, whose numbers were out of the range of int
    GeoZoneParser::Result result;
    QString errorString;

    // A web feature service response limited to far fewer features than it matched
    const QByteArray limited = R"({ "type": "FeatureCollection", "numberMatched": 1e300, "numberReturned": 1, "features": [ { "type": "Feature",
        "properties": { "type": "PROHIBITED" }, "geometry": { "type": "Polygon", "coordinates": [ [ [8, 47], [8.01, 47], [8.01, 47.01], [8, 47] ] ] } } ] })";
    QVERIFY2(GeoZoneParser::parse(limited, result, errorString), qPrintable(errorString));
    QCOMPARE(result.missingFeatures, std::numeric_limits<int>::max());

    // AIXM arcs sweeping more than a turn end after one, those with angles which aren't numbers are left out
    const QString aixm = QStringLiteral(R"(<message:AIXMBasicMessage xmlns:message="http://www.aixm.aero/schema/5.1/message"
        xmlns:aixm="http://www.aixm.aero/schema/5.1" xmlns:gml="http://www.opengis.net/gml/3.2"><message:hasMember><aixm:Airspace>
        <aixm:timeSlice><aixm:AirspaceTimeSlice><aixm:type>R</aixm:type><aixm:designator>ARC</aixm:designator>
        <aixm:geometryComponent><aixm:AirspaceGeometryComponent><aixm:theAirspaceVolume><aixm:AirspaceVolume>
        <aixm:horizontalProjection><aixm:Surface><gml:patches><gml:PolygonPatch><gml:exterior><gml:Ring><gml:curveMember><gml:Curve>
        <gml:segments><gml:ArcByCenterPoint><gml:pointProperty><aixm:Point><gml:pos>51.3 -0.8</gml:pos></aixm:Point></gml:pointProperty>
        <gml:radius uom="[nmi_i]">2</gml:radius><gml:startAngle uom="deg">0</gml:startAngle><gml:endAngle uom="deg">%1</gml:endAngle>
        </gml:ArcByCenterPoint></gml:segments></gml:Curve></gml:curveMember></gml:Ring></gml:exterior></gml:PolygonPatch></gml:patches>
        </aixm:Surface></aixm:horizontalProjection></aixm:AirspaceVolume></aixm:theAirspaceVolume></aixm:AirspaceGeometryComponent>
        </aixm:geometryComponent></aixm:AirspaceTimeSlice></aixm:timeSlice></aixm:Airspace></message:hasMember></message:AIXMBasicMessage>)");
    for (const QString &endAngle : { QStringLiteral("1e6"), QStringLiteral("1e12") }) {
        QVERIFY2(GeoZoneParser::parse(aixm.arg(endAngle).toUtf8(), result, errorString), qPrintable(errorString));
        QCOMPARE(result.zones.count(), 1);
        // One turn, with a point every 2.66 degrees keeping the chords of the 2 NM arc within 1 m of it
        QVERIFY(qAbs(result.zones[0].volumes[0].rings[0].count() - 136) <= 1);
        QVERIFY(GeoZone(result.zones[0]).contains(QGeoCoordinate(51.3, -0.8)));
    }
    for (const QString &endAngle : { QStringLiteral("inf"), QStringLiteral("nan") }) {
        QVERIFY(!GeoZoneParser::parse(aixm.arg(endAngle).toUtf8(), result, errorString));
        QVERIFY(!errorString.isEmpty());
    }
}

void GeoZoneParserTest::_testDisplayText()
{
    const GeoZoneParser::Result ed269 = _parseResource(QStringLiteral("ed269_test.json"));
    QCOMPARE(ed269.zones.count(), 3);

    // Each volume on a line, see _testLimitTexts for the units
    const auto meters = _setVerticalUnits(false);
    const GeoZone prohibited(ed269.zones[0]);
    QCOMPARE(GeoZoneText::verticalLimits(prohibited.data()), QStringLiteral("GND - 120 m AGL\n120 m AGL - Unlimited"));
    QCOMPARE(GeoZoneSchedule::text(prohibited.data().applicability), QStringLiteral("Permanent"));
    QCOMPARE(GeoZoneText::reasons(prohibited.data()), QStringLiteral("Air traffic, Sensitive area"));
    QCOMPARE(GeoZoneText::verticalLimits(ed269.zones[1]), QStringLiteral("GND - 122 m AGL (400 ft)"));

    const GeoZone scheduled(ed269.zones[2]);
    QCOMPARE(GeoZoneText::verticalLimits(scheduled.data()), QStringLiteral("Not specified"));
    QCOMPARE(GeoZoneSchedule::text(scheduled.data().applicability), QStringLiteral("From 2026-03-29 01:00 UTC to 2026-10-25 00:59 UTC\n  MON, TUE 05:00-18:00 UTC"));
}

void GeoZoneParserTest::_testLimitTexts_data()
{
    QTest::addColumn<double>("lowerLimit");
    QTest::addColumn<double>("upperLimit");
    QTest::addColumn<QString>("lowerReference");
    QTest::addColumn<QString>("upperReference");
    QTest::addColumn<bool>("publishedInFeet");
    QTest::addColumn<bool>("shownInFeet");
    QTest::addColumn<QString>("text");

    // Limits are shown in the user's units, rounded, with the published value when it is in the other unit
    QTest::newRow("m AGL in m") << 0.0 << 120.0 << "AGL" << "AGL" << false << false << "GND - 120 m AGL";
    QTest::newRow("m AGL in ft") << 0.0 << 120.0 << "AGL" << "AGL" << false << true << "GND - 394 ft AGL (120 m)";
    QTest::newRow("ft AGL in m") << 0.0 << 400.0 << "AGL" << "AGL" << true << false << "GND - 122 m AGL (400 ft)";
    QTest::newRow("ft AGL in ft") << 0.0 << 400.0 << "AGL" << "AGL" << true << true << "GND - 400 ft AGL";
    QTest::newRow("ft AMSL in m") << 1000.0 << 2500.0 << "AMSL" << "AMSL" << true << false << "305 m AMSL (1000 ft) - 762 m AMSL (2500 ft)";
    QTest::newRow("m AMSL in ft") << 150.0 << 300.0 << "AMSL" << "AMSL" << false << true << "492 ft AMSL (150 m) - 984 ft AMSL (300 m)";
    // Flight levels are shown with their approximate height above mean sea level with the QNH, standard here
    QTest::newRow("ft STD in m") << 9500.0 << 19500.0 << "STD" << "STD" << true << false << "FL 95 ≈ 2896 m AMSL - FL 195 ≈ 5944 m AMSL (QNH 1013.25 hPa)";
    QTest::newRow("m STD in ft") << 2895.6 << 99999.0 << "STD" << "STD" << false << true << "FL 95 ≈ 9500 ft AMSL - Unlimited (QNH 1013.25 hPa)";
    QTest::newRow("AGL to STD in ft") << 0.0 << 2895.6 << "AGL" << "STD" << false << true << "GND - FL 95 ≈ 9500 ft AMSL (QNH 1013.25 hPa)";
    QTest::newRow("m WGS84 in m") << 500.0 << 1000.0 << "WGS84" << "WGS84" << false << false << "500 m WGS84 - 1000 m WGS84";
    // Only zones from the ground start at GND, the codes are shown as published
    QTest::newRow("0 m AMSL") << 0.0 << 120.0 << "AMSL" << "AMSL" << false << false << "0 m AMSL - 120 m AMSL";
    QTest::newRow("SFC MSL") << 0.0 << 120.0 << " sfc" << "msl " << false << false << "GND - 120 m MSL";
    QTest::newRow("unlimited") << 120.0 << 99999.0 << "AGL" << "AMSL" << false << true << "394 ft AGL (120 m) - Unlimited";
    QTest::newRow("no floor") << qQNaN() << 120.0 << "AGL" << "AGL" << false << false << "not specified - 120 m AGL";
    QTest::newRow("not specified") << qQNaN() << qQNaN() << "AGL" << "AGL" << true << false << "Not specified";
}

void GeoZoneParserTest::_testLimitTexts()
{
    QFETCH(double, lowerLimit);
    QFETCH(double, upperLimit);
    QFETCH(QString, lowerReference);
    QFETCH(QString, upperReference);
    QFETCH(bool, publishedInFeet);
    QFETCH(bool, shownInFeet);
    QFETCH(QString, text);

    GeoZone::Volume volume;
    volume.isCircle = true;
    volume.center = QGeoCoordinate(47, 8);
    volume.radius = 500;
    volume.feet = publishedInFeet;
    volume.lowerLimit = lowerLimit;
    volume.upperLimit = upperLimit;
    volume.setReferences(lowerReference, upperReference);
    GeoZone::Data data;
    data.volumes.append(volume);
    const auto units = _setVerticalUnits(shownInFeet);
    QCOMPARE(GeoZoneText::verticalLimits(data), text);
}

void GeoZoneParserTest::_testFlightLevelQnh()
{
    // A zone from FL 95 up, published as pressure altitude in feet
    QJsonObject feature = _circleFeature(QStringLiteral("FL95"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47, 8), 1000);
    QJsonObject geometry = feature.value(QStringLiteral("geometry")).toArray().first().toObject();
    geometry.insert(QStringLiteral("uomDimensions"), QStringLiteral("FT"));
    geometry.insert(QStringLiteral("lowerLimit"), 9500);
    geometry.insert(QStringLiteral("lowerVerticalReference"), QStringLiteral("STD"));
    geometry.insert(QStringLiteral("upperLimit"), 99999);
    geometry.insert(QStringLiteral("upperVerticalReference"), QStringLiteral("AMSL"));
    geometry.insert(QStringLiteral("horizontalProjection"), QJsonObject{ { "type", "Circle" }, { "center", QJsonArray{ 8, 47 } }, { "radius", 1000 / 0.3048 } });
    feature.insert(QStringLiteral("geometry"), QJsonArray{ geometry });
    const QByteArray document = QJsonDocument(QJsonObject{ { "features", QJsonArray{ feature } } }).toJson(QJsonDocument::Compact);

    ScopedEnvironment environment;
    const auto meters = _setVerticalUnits(false);
    const auto parse = [&document]() {
        GeoZoneParser::Result result;
        QString errorString;
        if (!GeoZoneParser::parse(document, result, errorString)) {
            qWarning() << errorString;
        }
        return result;
    };
    const auto serialize = [](const GeoZoneParser::Result &result) {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setVersion(GeoZoneParseCache::streamVersion);
        stream << result;
        return bytes;
    };

    // The parse keeps the pressure altitude, so parse caches don't depend on the QNH
    environment.setQnh(1003);
    const GeoZoneParser::Result result = parse();
    QCOMPARE(result.zones.count(), 1);
    environment.setQnh(1030);
    QCOMPARE(serialize(parse()), serialize(result));

    // The same zone follows the QNH: FL 95 is compared from 2697.5 m with 1003 hPa and 2912.7 m with 1030 hPa
    const GeoZone zone(result.zones.first());
    QVERIFY(zone.approximateLimits());
    const GeoZone::Position position{ QGeoCoordinate(47, 8), 2800 };
    environment.setQnh(1003);
    QVERIFY(zone.contains(position));
    QCOMPARE(GeoZoneText::verticalLimits(zone.data()), QStringLiteral("FL 95 ≈ 2810 m AMSL - Unlimited (QNH 1003 hPa)"));
    environment.setQnh(1030);
    QVERIFY(!zone.contains(position));
    QCOMPARE(GeoZoneText::verticalLimits(zone.data()), QStringLiteral("FL 95 ≈ 3034 m AMSL - Unlimited (QNH 1030 hPa)"));
}

void GeoZoneParserTest::_testProviders()
{
    // Each service is recognized from its own URLs, and identifiers are unique
    const QList<GeoZoneProviders::Service> services = GeoZoneProviders::services();
    QSet<QString> ids;
    const QGeoRectangle area(QGeoCoordinate(48.2, 2.2), QGeoCoordinate(48.0, 2.5));
    for (const GeoZoneProviders::Service &service : services) {
        QVERIFY2(!ids.contains(service.id), qPrintable(service.id));
        ids.insert(service.id);
        QVERIFY(!service.name.isEmpty());
        QVERIFY(!service.feedUrl.isEmpty() || service.servesAreas());
        if (!service.feedUrl.isEmpty()) {
            QVERIFY2(QUrl(service.feedUrl).host().endsWith(service.host), qPrintable(service.id));
            if (service.format != GeoZoneParser::Format::Unknown) {
                QCOMPARE(GeoZoneProviders::formatHint(service.feedUrl), service.format);
            }
        }
        if (service.servesAreas()) {
            const QString url = GeoZoneProviders::areaFeedUrl(service.id, area);
            QVERIFY2(QUrl(url).host().endsWith(service.host), qPrintable(url));
            QCOMPARE(GeoZoneProviders::formatHint(url), service.format);
        }
    }
    QCOMPARE(GeoZoneProviders::feedPresets().count() + GeoZoneProviders::areaProviders().count(), services.count());
    // The FAA layers share a host, their paths tell them apart, and their URLs give the area as an envelope
    const QGeoRectangle washington(QGeoCoordinate(38.95, -77.1), QGeoCoordinate(38.85, -76.95));
    const QString sua = GeoZoneProviders::areaFeedUrl(QStringLiteral("faa-sua"), washington);
    QVERIFY2(sua.contains(QStringLiteral("/Special_Use_Airspace/FeatureServer/0/query?")) && sua.contains(QStringLiteral("geometry=-77.10000,38.85000,-76.95000,38.95000")),
             qPrintable(sua));
    QCOMPARE(GeoZoneProviders::serviceId(sua), QStringLiteral("faa-sua"));
    QCOMPARE(GeoZoneProviders::serviceId(GeoZoneProviders::areaFeedUrl(QStringLiteral("faa-uasfm"), washington)), QStringLiteral("faa-uasfm"));
    QCOMPARE(GeoZoneProviders::feedArea(sua), washington);
    QVERIFY(GeoZoneProviders::areaDownloaded(QStringLiteral("faa-sua"), { sua }, washington));
    QVERIFY(!GeoZoneProviders::areaDownloaded(QStringLiteral("faa-class"), { sua }, washington));
    QCOMPARE(GeoZoneProviders::formatHint(QStringLiteral("https://services6.arcgis.com/other/arcgis/rest/services/Layer/FeatureServer/0/query")), GeoZoneParser::Format::Unknown);

    // The OpenAIP templates' responses are expected in its format, also when empty
    QCOMPARE(GeoZoneProviders::formatHint(QStringLiteral("https://api.core.openaip.net/api/airspaces?bbox=8,47,9,48&limit=1000&page=1")), GeoZoneParser::Format::OpenAip);
    QCOMPARE(GeoZoneProviders::formatHint(QStringLiteral("https://storage.openaip.net/openaip-system-exports/de_asp.geojson")), GeoZoneParser::Format::OpenAip);

    // Further pages of ArcGIS and numbered responses
    using Paging = GeoZoneProviders::Paging;
    const Paging arcGis = GeoZoneProviders::paging(sua, R"({"type":"FeatureCollection","properties":{"exceededTransferLimit":true},"features":[)", QByteArray());
    QCOMPARE(arcGis.scheme, Paging::Scheme::ArcGisOffset);
    QCOMPARE(arcGis.pageSize, 1000);
    QVERIFY(GeoZoneProviders::pageUrl(sua, arcGis, 2).contains(QStringLiteral("resultOffset=2000")));
    QCOMPARE(GeoZoneProviders::paging(sua, R"({"type":"FeatureCollection","features":[)", "]}").scheme, Paging::Scheme::None);
    const QString numbered = QStringLiteral("https://example.com/api/airspaces?bbox=1,2,3,4&limit=100&page=1");
    const Paging pages = GeoZoneProviders::paging(numbered, R"({"limit":100,"totalCount":250,"totalPages":3,"page":1,"items":[)", QByteArray());
    QCOMPARE(pages.scheme, Paging::Scheme::PageNumber);
    QCOMPARE(pages.pageCount, 2);
    QCOMPARE(GeoZoneProviders::pageUrl(numbered, pages, 2), QStringLiteral("https://example.com/api/airspaces?bbox=1,2,3,4&limit=100&page=3"));
    QCOMPARE(GeoZoneProviders::paging(numbered, R"({"limit":100,"totalCount":50,"totalPages":1,"page":1,"items":[)", QByteArray()).scheme, Paging::Scheme::None);

    QCOMPARE(GeoZoneProviders::formatHint(QStringLiteral("https://example.com/zones.json")), GeoZoneParser::Format::Unknown);
    QCOMPARE(GeoZoneProviders::formatHint(QStringLiteral("https://notfaa.gov/zones.json")), GeoZoneParser::Format::Unknown);
}

void GeoZoneParserTest::_testFeedCatalogue()
{
    // The built-in catalogue is valid
    const GeoZoneFeedCatalogue::Catalogue builtIn = GeoZoneFeedCatalogue::builtIn();
    QVERIFY2(builtIn.warnings.isEmpty(), qPrintable(builtIn.warnings.join(QLatin1Char('\n'))));
    QCOMPARE(builtIn.feeds.count(), 19);
    QCOMPARE(builtIn.templates.count(), 6);
    QVERIFY(builtIn.disabled.isEmpty());

    // A catalogue replaces, adds and turns off entries, entries which can't be used are left out with a warning
    const QByteArray overlay = R"json({
        "fileType": "GeoZoneFeeds", "version": 1,
        "feeds": [
            { "id": "foca", "name": "Switzerland (mirror)", "url": "https://mirror.example.com/ch.json", "format": "ed269", "refreshHours": 12, "licence": "Test licence" },
            { "id": "acme", "country": "nl", "name": "Acme zones", "url": "https://zones.example.com/nl.json", "live": true },
            { "id": "acme-area", "name": "Acme area", "areaUrl": "https://zones.example.com/area?bbox={bbox}", "coverage": [3, 50, 8, 54], "format": "ed318",
              "serviceTemplate": { "maxAreaKm2": 500, "note": "Acme note" } },
            { "id": "faa-tfr", "enabled": false },
            { "id": "broken-format", "name": "Broken", "url": "https://zones.example.com/x.json", "format": "kml" },
            { "id": "no-url", "name": "No URL" },
            { "id": "bad-area", "name": "Bad area", "areaUrl": "https://zones.example.com/area" },
            { "id": "bad-pattern", "name": "Bad pattern", "url": "https://zones.example.com/page", "datasetLink": { "pattern": "href=\"(" } },
            { "name": "No id", "url": "https://zones.example.com/y.json" },
            { "id": "acme", "name": "Twice", "url": "https://zones.example.com/twice.json" }
        ],
        "services": [
            { "id": "acme-ed318", "name": "Acme ED-318", "url": "https://utm.example.com/uas-zones", "note": "Acme account needed",
              "service": { "method": "ed318Post", "auth": "basic", "areaOfInterest": true, "maxAreaKm2": 200 } },
            { "id": "airspacelink", "enabled": false }
        ]
    })json";
    GeoZoneFeedCatalogue::Catalogue parsed;
    QString errorString;
    QVERIFY2(GeoZoneFeedCatalogue::parse(overlay, QStringLiteral("feeds.json"), parsed, errorString), qPrintable(errorString));
    QCOMPARE(parsed.feeds.count(), 3);
    QCOMPARE(parsed.templates.count(), 1);
    QCOMPARE(parsed.disabled, QStringList({ QStringLiteral("faa-tfr"), QStringLiteral("airspacelink") }));
    QCOMPARE(parsed.warnings.count(), 6);
    QCOMPARE(parsed.warnings[0], QStringLiteral("Feed broken-format: Its format kml is unknown. It is left out."));
    QCOMPARE(parsed.warnings[1], QStringLiteral("Feed no-url: It has neither a url nor an areaUrl. It is left out."));
    QVERIFY(parsed.warnings[2].startsWith(QStringLiteral("Feed bad-area: Its areaUrl must be")));
    QVERIFY(parsed.warnings[3].startsWith(QStringLiteral("Feed bad-pattern: Its datasetLink pattern is invalid")));
    QCOMPARE(parsed.warnings[4], QStringLiteral("Feed 9 has no id, it is left out."));
    QCOMPARE(parsed.warnings[5], QStringLiteral("Feed acme is listed twice, the first is used."));
    QCOMPARE(parsed.feeds[1].country, QStringLiteral("NL"));
    QCOMPARE(parsed.feeds[0].origin, QStringLiteral("feeds.json"));

    GeoZoneFeedCatalogue::Catalogue merged = builtIn;
    GeoZoneFeedCatalogue::apply(merged, parsed);
    GeoZoneProviders::setCatalogue(merged.feeds, merged.templates);
    const auto restore = qScopeGuard([]() { GeoZoneProviders::resetCatalogue(); });
    QStringList names;
    for (const QVariant &preset : GeoZoneProviders::feedPresets()) {
        names.append(preset.toMap().value(QStringLiteral("name")).toString());
    }
    QVERIFY(names.contains(QStringLiteral("Switzerland (mirror)")) && names.contains(QStringLiteral("Acme zones")));
    QVERIFY(!names.contains(QStringLiteral("Switzerland (FOCA)")) && !names.contains(QStringLiteral("United States TFRs (FAA)")));
    QCOMPARE(GeoZoneProviders::formatHint(QStringLiteral("https://mirror.example.com/ch.json")), GeoZoneParser::Format::ED269);
    QCOMPARE(GeoZoneProviders::refreshHours(QStringLiteral("https://mirror.example.com/ch.json")), 12.0);
    QVERIFY(GeoZoneProviders::isLiveUrl(QStringLiteral("https://zones.example.com/nl.json")));
    const QGeoRectangle area(QGeoCoordinate(52.2, 4.5), QGeoCoordinate(52.0, 4.8));
    QCOMPARE(GeoZoneProviders::areaFeedUrl(QStringLiteral("acme-area"), area), QStringLiteral("https://zones.example.com/area?bbox=4.50000,52.00000,4.80000,52.20000"));
    QCOMPARE(GeoZoneProviders::serviceName(GeoZoneProviders::areaFeedUrl(QStringLiteral("acme-area"), area)), QStringLiteral("Acme area"));
    const QList<GeoZoneProviders::ServiceTemplate> templates = GeoZoneProviders::serviceTemplates();
    const auto findTemplate = [&templates](const QString &id) {
        return std::find_if(templates.cbegin(), templates.cend(), [&id](const GeoZoneProviders::ServiceTemplate &serviceTemplate) { return serviceTemplate.service.id == id; });
    };
    QVERIFY(findTemplate(QStringLiteral("airspacelink")) == templates.cend());
    const auto acme = findTemplate(QStringLiteral("acme-ed318"));
    QVERIFY(acme != templates.cend());
    QCOMPARE(acme->service.method, GeoZoneService::Method::Ed318Post);
    QCOMPARE(acme->service.auth, GeoZoneService::Auth::Basic);
    QCOMPARE(acme->service.maxAreaKm2, 200.0);
    QCOMPARE(acme->service.name, QStringLiteral("Acme ED-318"));
    const auto acmeArea = findTemplate(QStringLiteral("acme-area"));
    QVERIFY(acmeArea != templates.cend());
    QCOMPARE(acmeArea->url, QStringLiteral("https://zones.example.com/area?bbox={bbox}"));
    QCOMPARE(acmeArea->service.maxAreaKm2, 500.0);
    QVERIFY(acmeArea->service.areaOfInterest);

    // Documents which aren't catalogues of this version are refused whole
    for (const QByteArray &invalid : { QByteArray("[]"), QByteArray("not json"), QByteArray(R"({ "fileType": "FactMetaData", "feeds": [] })"),
                                       QByteArray(R"({ "version": 2, "feeds": [] })"), QByteArray(R"({ "feeds": {} })"), QByteArray("{}") }) {
        QVERIFY2(!GeoZoneFeedCatalogue::parse(invalid, QStringLiteral("feeds.json"), parsed, errorString), invalid.constData());
        QVERIFY(!errorString.isEmpty());
    }
    QCOMPARE(errorString, QStringLiteral("Not a feed catalogue: it has no feeds or services."));
}

void GeoZoneParserTest::_testXmlAndPackages()
{
    // XML documents go to the format recognizing their root element, others are reported
    GeoZoneParser::Result result;
    QString errorString;
    QFile aixmFile(QStringLiteral(":/unittest/uk_aixm_test.xml"));
    QVERIFY(aixmFile.open(QIODevice::ReadOnly));
    const QByteArray aixmBytes = aixmFile.readAll();
    QVERIFY2(GeoZoneParser::parse(aixmBytes, result, errorString), qPrintable(errorString));
    QCOMPARE(result.format, GeoZoneParser::Format::Aixm);
    QVERIFY(!GeoZoneParser::parse(R"(<?xml version="1.0"?><ows:ExceptionReport xmlns:ows="http://www.opengis.net/ows/1.1"><ows:Exception><ows:ExceptionText>Illegal BBOX</ows:ExceptionText></ows:Exception></ows:ExceptionReport>)",
                                  result, errorString));
    QCOMPARE(errorString, QStringLiteral("The server reported an error: Illegal BBOX"));
    QVERIFY(!GeoZoneParser::parse("<!DOCTYPE html><html><body>Maintenance</body></html>", result, errorString));
    QCOMPARE(errorString, QStringLiteral("The address returned a web page instead of geozone data."));
    QVERIFY(!GeoZoneParser::parse("<metadata><title>Zones</title></metadata>", result, errorString));
    QCOMPARE(errorString, QStringLiteral("Unsupported XML document (metadata)."));

#ifndef GEOZONE_CORE_TEST
    // A package with the zones split into two files, one zone in both, and a metadata file
    QFile ed269File(QStringLiteral(":/unittest/ed269_test.json"));
    QVERIFY(ed269File.open(QIODevice::ReadOnly));
    QByteArray ed269 = ed269File.readAll();
    if (ed269.startsWith("\xEF\xBB\xBF")) {
        ed269.remove(0, 3);
    }
    const QJsonObject document = QJsonDocument::fromJson(ed269).object();
    const QJsonArray features = document.value(QStringLiteral("features")).toArray();
    QCOMPARE(features.count(), 4);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid() && QDir(dir.path()).mkpath(QStringLiteral("package")));
    const auto writePart = [&dir, &document, &features](const QString &name, qsizetype first, qsizetype last) {
        QJsonObject part = document;
        QJsonArray partFeatures;
        for (qsizetype i = first; i <= last; i++) {
            partFeatures.append(features[i]);
        }
        part.insert(QStringLiteral("features"), partFeatures);
        QFile file(dir.filePath(QStringLiteral("package/") + name));
        return file.open(QIODevice::WriteOnly) && (file.write(QJsonDocument(part).toJson()) > 0);
    };
    QVERIFY(writePart(QStringLiteral("part1.json"), 0, 2));
    QVERIFY(writePart(QStringLiteral("part2.json"), 2, 3));
    QFile metadata(dir.filePath(QStringLiteral("package/metadata.xml")));
    QVERIFY(metadata.open(QIODevice::WriteOnly) && (metadata.write("<metadata><title>Zones</title></metadata>") > 0));
    metadata.close();
    const QString packagePath = dir.filePath(QStringLiteral("package.zip"));
    QVERIFY(QGCZip::zipDirectory(dir.filePath(QStringLiteral("package")), packagePath));

    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    GeoZoneManager manager(storageDir.path());
    QVERIFY2(manager.repository()->loadFile(packagePath, errorString), qPrintable(errorString));
    // The fourth feature has no supported geometry, the zone in both files is loaded once
    QCOMPARE(manager.zoneCount(), 3);
    QCOMPARE(manager.sources()->value<GeoZoneSource*>(0)->formatName(), QStringLiteral("ED-269"));

    QFile archive(packagePath);
    QVERIFY(archive.open(QIODevice::ReadOnly));
    const QList<GeoZoneZip::File> files = GeoZoneZip::files(archive.readAll(), { QStringLiteral(".json"), QStringLiteral(".xml") }, 1 << 20, errorString);
    QCOMPARE(files.count(), 3);
    QVERIFY(files.first().data.size() >= files.last().data.size());
#endif
}

void GeoZoneParserTest::_testParseCancel()
{
    // The first feature has no geometry, the parser logs it before reading the others
    QJsonArray features{ QJsonObject{ { "identifier", "NOGEOMETRY" }, { "restriction", "PROHIBITED" }, { "geometry", QJsonArray() } } };
    for (int i = 0; i < 1000; i++) {
        features.append(_circleFeature(QStringLiteral("Z%1").arg(i), QStringLiteral("PROHIBITED"), QGeoCoordinate(47 + ((i % 100) * 0.01), 8 + ((i / 100) * 0.01)), 500));
    }
    const QByteArray document = QJsonDocument(QJsonObject{ { "features", features } }).toJson(QJsonDocument::Compact);
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parse(document, result, errorString), qPrintable(errorString));
    QCOMPARE(result.zones.count(), 1000);
    QCOMPARE(result.skippedFeatures, 1);

    QLoggingCategory &category = const_cast<QLoggingCategory&>(GeoZoneParserLog());
    const bool debugEnabled = category.isDebugEnabled();
    category.setEnabled(QtDebugMsg, true);
    const QtMessageHandler previousHandler = qInstallMessageHandler(cancelOnSkippedFeature);
    const auto restore = qScopeGuard([&category, debugEnabled, previousHandler]() {
        (void) qInstallMessageHandler(previousHandler);
        category.setEnabled(QtDebugMsg, debugEnabled);
        s_cancelOnSkip = nullptr;
    });

    // Canceled while reading the features, the parse stops at the next one
    GeoZoneParser::Options options;
    s_cancelOnSkip = &options.cancelToken;
    QVERIFY(!GeoZoneParser::parse(document, result, errorString, options));
    QVERIFY(options.cancelToken.isCancelled());
    QCOMPARE(errorString, QStringLiteral("The parse was canceled."));
    QVERIFY(result.zones.isEmpty());

    // Documents of all formats stop, the token is already canceled
    QFile aixm(QStringLiteral(":/unittest/uk_aixm_test.xml"));
    QVERIFY(aixm.open(QIODevice::ReadOnly));
    QVERIFY(!GeoZoneParser::parse(aixm.readAll(), result, errorString, options));
    QCOMPARE(errorString, QStringLiteral("The parse was canceled."));
    QVERIFY(result.zones.isEmpty());

    // A load canceled while it parses, like when its source is removed, stops and stores nothing
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneLoader::Job job = GeoZoneLoader::fileJob(_writeFeatures(dir, QStringLiteral("zones.json"), features));
    s_cancelOnSkip = &job.cancelToken;
    const GeoZoneStore store(dir.filePath(QStringLiteral("store")));
    const GeoZoneLoader::Result loadResult = GeoZoneLoader::run(job, store);
    QVERIFY(loadResult.cancelled);
    QVERIFY(!loadResult.stored);
    QVERIFY(loadResult.result.zones.isEmpty());
    QVERIFY(!QFile::exists(store.path(job.storedFileName)));
}

void GeoZoneParserTest::_testParseCacheRoundTrip_data()
{
    _testParseFixtures_data();
    QTest::newRow("synthetic") << QString() << 0 << 0 << 0 << 0 << 0 << QString();
}

void GeoZoneParserTest::_testParseCacheRoundTrip()
{
    QFETCH(QString, fileName);
    QFETCH(int, formatHint);

    GeoZoneParser::Result result;
    QByteArray bytes = "synthetic";
    if (fileName.isEmpty()) {
        // Fields added to these structures must be serialized, set by _syntheticResult and compared by
        // _resultDifference, then counted here
        QCOMPARE(_fieldCount<GeoZoneParser::Result>(), 12);
        QCOMPARE(_fieldCount<GeoZoneParser::Companion>(), 2);
        QCOMPARE(_fieldCount<GeoZone::Data>(), 19);
        QCOMPARE(_fieldCount<GeoZone::Volume>(), 11);
        QCOMPARE(_fieldCount<GeoZone::TimePeriod>(), 4);
        QCOMPARE(_fieldCount<GeoZone::DailyPeriod>(), 8);
        result = _syntheticResult();
    } else {
        QFile file(QStringLiteral(":/unittest/") + fileName);
        QVERIFY(file.open(QIODevice::ReadOnly));
        bytes = file.readAll();
        QString errorString;
        QVERIFY2(GeoZoneLoader::parse(bytes, result, errorString, static_cast<GeoZoneParser::Format>(formatHint)), qPrintable(errorString));
    }
    const auto serialize = [](const GeoZoneParser::Result &value) {
        QByteArray serialized;
        QDataStream stream(&serialized, QIODevice::WriteOnly);
        stream.setVersion(GeoZoneParseCache::streamVersion);
        stream << value;
        return serialized;
    };

    // Read back, the result and its serialization are the same
    const QByteArray serialized = serialize(result);
    GeoZoneParser::Result readBack;
    {
        QDataStream stream(serialized);
        stream.setVersion(GeoZoneParseCache::streamVersion);
        stream >> readBack;
        QCOMPARE(stream.status(), QDataStream::Ok);
        QVERIFY(stream.atEnd());
    }
    QString difference = _resultDifference(result, readBack);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QCOMPARE(serialize(readBack), serialized);
    for (qsizetype i = 0; i < result.zones.count(); i++) {
        const GeoZone zone(result.zones[i]);
        const GeoZone readZone(readBack.zones[i]);
        QCOMPARE(GeoZoneSchedule::text(readZone.data().applicability), GeoZoneSchedule::text(zone.data().applicability));
        QCOMPARE(GeoZoneText::verticalLimits(readZone.data()), GeoZoneText::verticalLimits(zone.data()));
        QCOMPARE(readZone.box().toRectangle(), zone.box().toRectangle());
    }

    // Through the cache of a stored copy
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filePath = dir.filePath(QStringLiteral("zones.json"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly) && (file.write(bytes) == bytes.size()));
    file.close();
    const GeoZoneParseCache::Key key{ QStringLiteral("de"), static_cast<GeoZoneParser::Format>(formatHint) };
    QVERIFY(!GeoZoneParseCache::write(filePath, bytes.size() + 1, GeoZoneHash::contentHash(bytes), key, result));
    QVERIFY(GeoZoneParseCache::write(filePath, bytes.size(), GeoZoneHash::contentHash(bytes), key, result));
    GeoZoneParser::Result cached;
    QVERIFY(GeoZoneParseCache::read(filePath, key, cached));
    difference = _resultDifference(result, cached);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // Damaged data fails the stream rather than crashing or allocating for the counts it reads
    QRandomGenerator random(static_cast<quint32>(serialized.size()));
    for (int i = 0; i < 300; i++) {
        QByteArray damaged = serialized;
        const bool truncated = (i % 2) == 0;
        if (truncated) {
            damaged.truncate(random.bounded(static_cast<int>(serialized.size())));
        } else {
            for (int j = 0; j < 4; j++) {
                damaged[random.bounded(static_cast<int>(damaged.size()))] = static_cast<char>(random.bounded(256));
            }
        }
        QDataStream stream(damaged);
        stream.setVersion(GeoZoneParseCache::streamVersion);
        GeoZoneParser::Result damagedResult;
        stream >> damagedResult;
        if (truncated) {
            QVERIFY(stream.status() != QDataStream::Ok);
        }
    }
    QByteArray huge;
    {
        QDataStream stream(&huge, QIODevice::WriteOnly);
        stream.setVersion(GeoZoneParseCache::streamVersion);
        stream << quint8(0) << QString() << QString() << QDateTime() << QDateTime() << QDateTime() << quint32(0xFFFFFFFF);
    }
    QDataStream stream(huge);
    stream.setVersion(GeoZoneParseCache::streamVersion);
    GeoZoneParser::Result hugeResult;
    stream >> hugeResult;
    QCOMPARE(stream.status(), QDataStream::ReadCorruptData);
    QVERIFY(hugeResult.zones.isEmpty());
}

void GeoZoneParserTest::_testParseCacheValidation()
{
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const GeoZoneStore store(dir.path());
    QFile fixture(QStringLiteral(":/unittest/ed269_test.json"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    QByteArray bytes = fixture.readAll();
    QString errorString;
    QVERIFY(store.writeFile(QStringLiteral("zones.json"), bytes, errorString));
    const QString filePath = store.path(QStringLiteral("zones.json"));
    const QString cachePath = GeoZoneParseCache::path(filePath);
    GeoZoneParser::Result parsed;
    QVERIFY(GeoZoneLoader::parse(bytes, parsed, errorString, GeoZoneParser::Format::Unknown, QStringLiteral("en")));

    const auto readFile = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto writeAt = [](const QString &path, qint64 offset, const QByteArray &data) {
        QFile file(path);
        return file.open(QIODevice::ReadWrite) && file.seek(offset) && (file.write(data) == data.size());
    };
    const auto setModified = [](const QString &path, const QDateTime &time) {
        QFile file(path);
        return file.open(QIODevice::ReadWrite) && file.setFileTime(time, QFileDevice::FileModificationTime);
    };
    const auto bigEndian = [](auto value) {
        QByteArray data(sizeof(value), Qt::Uninitialized);
        qToBigEndian(value, data.data());
        return data;
    };

    // Restoring the stored copy parses it and writes its cache, the next restores read the cache
    GeoZoneLoader::Job job;
    job.key = QStringLiteral("zones");
    job.sourcePath = filePath;
    job.fromStore = true;
    job.language = QStringLiteral("en");
    const auto restore = [&job, &store](bool fromCache) {
        const GeoZoneLoader::Result loaded = GeoZoneLoader::run(job, store);
        return loaded.errorString.isEmpty() && !loaded.stored && (loaded.fromCache == fromCache) ? loaded.result : GeoZoneParser::Result();
    };
    QVERIFY(!QFile::exists(cachePath));
    QCOMPARE(restore(false).zones.count(), 3);
    QVERIFY(QFile::exists(cachePath));
    const QByteArray cache = readFile(cachePath);
    QVERIFY(cache.startsWith("GZPC"));
    const QString difference = _resultDifference(restore(true), parsed);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // Caches of another cache format, parser or Qt data stream version are parsed again and rewritten
    const QList<std::pair<qint64, QByteArray>> versions = {
        { 4, bigEndian(quint16(GeoZoneParseCache::formatVersion + 1)) },
        { 6, bigEndian(quint16(GeoZoneParser::version + 1)) },
        { 8, bigEndian(qint32(GeoZoneParseCache::streamVersion - 1)) },
    };
    for (const auto &[offset, version] : versions) {
        QVERIFY(writeAt(cachePath, offset, version));
        QCOMPARE(restore(false).zones.count(), 3);
        QCOMPARE(readFile(cachePath), cache);
        QCOMPARE(restore(true).zones.count(), 3);
    }

    // And those of another language or format hint
    job.language = QStringLiteral("de");
    QCOMPARE(restore(false).zones.count(), 3);
    QCOMPARE(restore(true).zones.count(), 3);
    job.language = QStringLiteral("en");
    job.formatHint = GeoZoneParser::Format::ED269;
    QCOMPARE(restore(false).zones.count(), 3);
    job.formatHint = GeoZoneParser::Format::Unknown;
    QCOMPARE(restore(false).zones.count(), 3);
    QCOMPARE(readFile(cachePath), cache);

    // A copied stored copy has another time and the same bytes, it is hashed and the cache takes its time
    const QDateTime modified = QFileInfo(filePath).lastModified();
    QVERIFY(setModified(filePath, modified.addSecs(60)));
    QCOMPARE(restore(true).zones.count(), 3);
    QCOMPARE(readFile(cachePath).mid(20, 8), bigEndian(modified.addSecs(60).toMSecsSinceEpoch()));
    QCOMPARE(restore(true).zones.count(), 3);

    // A stored copy with other bytes of the same size is parsed again
    const qsizetype identifier = bytes.indexOf("\"T-PROH\"");
    QVERIFY(identifier > 0);
    QVERIFY(writeAt(filePath, identifier, "\"T-PROX\""));
    QVERIFY(setModified(filePath, modified.addSecs(120)));
    QCOMPARE(restore(false).zones.value(0).identifier, QStringLiteral("T-PROX"));
    QCOMPARE(restore(true).zones.value(0).identifier, QStringLiteral("T-PROX"));
    QVERIFY(writeAt(filePath, QFileInfo(filePath).size(), " "));
    QCOMPARE(restore(false).zones.count(), 3);

    // Damaged and truncated caches, or other files, are parsed again
    const QByteArray current = readFile(cachePath);
    const QList<QByteArray> damagedCaches = {
        current.left(current.size() / 2),
        current.left(current.size() - 1),
        current.left(10),
        QByteArray(),
        current.left(current.size() - 1) + char(current.back() ^ 0x55),
        QByteArray(current.size(), 'x'),
        bytes,
    };
    for (const QByteArray &damaged : damagedCaches) {
        QFile damagedFile(cachePath);
        QVERIFY(damagedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(damagedFile.write(damaged), damaged.size());
        damagedFile.close();
        QCOMPARE(restore(false).zones.value(0).identifier, QStringLiteral("T-PROX"));
        QCOMPARE(readFile(cachePath), current);
    }

    // Loading a new copy writes its cache, which is removed with it
    const QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());
    const QString sourcePath = sourceDir.filePath(QStringLiteral("ed318.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), sourcePath));
    GeoZoneLoader::Job fileJob = GeoZoneLoader::fileJob(sourcePath);
    const GeoZoneLoader::Result added = GeoZoneLoader::run(fileJob, store);
    QVERIFY(added.stored && !added.fromCache);
    QVERIFY(QFile::exists(GeoZoneParseCache::path(store.path(fileJob.storedFileName))));
    store.removeFile(fileJob.storedFileName);
    QVERIFY(!QFile::exists(store.path(fileJob.storedFileName)));
    QVERIFY(!QFile::exists(GeoZoneParseCache::path(store.path(fileJob.storedFileName))));

    // Caches of files which aren't stored anymore are removed with the unreferenced files
    QVERIFY(store.writeFile(QStringLiteral("orphan.json.gzc"), "cache", errorString));
    store.removeOrphanFiles({ QStringLiteral("zones.json") });
    QVERIFY(QFile::exists(cachePath));
    QVERIFY(!QFile::exists(store.path(QStringLiteral("orphan.json.gzc"))));
    store.removeFile(QStringLiteral("zones.json"));
    QVERIFY(!QFile::exists(cachePath));
    QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());

    // Feeds from servers have no file to read before their download, which names the stored copy
    QTest::failOnWarning(QRegularExpression(QStringLiteral("No file name specified")));
    QVERIFY(GeoZoneLoader::feedJob(QStringLiteral("https://example.com/zones.json")).storedFileName.isEmpty());
    QCOMPARE(GeoZoneStore::fileSuffix(QString()), QStringLiteral("json"));
}

GEOZONE_TEST(GeoZoneParserTest)
