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

using namespace GeoZoneTestHelpers;

namespace {


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

void GeoZoneParserTest::_testParseED269()
{
    GeoZoneParser::Result result;
    QString errorString;
    // File starts with a UTF-8 byte order mark like several national feeds
    QVERIFY2(GeoZoneParser::parseFile(QStringLiteral(":/unittest/ed269_test.json"), result, errorString), qPrintable(errorString));
    QCOMPARE(result.format, GeoZoneParser::Format::ED269);
    QCOMPARE(result.title, QStringLiteral("Test UASZoneVersion"));
    QCOMPARE(result.zones.count(), 3);
    QCOMPARE(result.skippedFeatures, 1);

    const GeoZone::Data &prohibited = result.zones[0];
    QCOMPARE(prohibited.identifier, QStringLiteral("T-PROH"));
    QCOMPARE(prohibited.restriction, GeoZone::Prohibited);
    QCOMPARE(prohibited.reasons, QStringList({ QStringLiteral("AIR_TRAFFIC"), QStringLiteral("SENSITIVE") }));
    QCOMPARE(prohibited.volumes.count(), 2);
    const GeoZone::Volume &polygon = prohibited.volumes[0];
    QVERIFY(!polygon.isCircle);
    QCOMPARE(polygon.rings.count(), 1);
    QCOMPARE(polygon.rings[0].count(), 4); // Closing vertex removed
    QCOMPARE(polygon.rings[0][1], QGeoCoordinate(47.0, 8.1)); // [lon, lat] order
    QCOMPARE(polygon.upperLimit, 120.0);
    QCOMPARE(polygon.upperReference, GeoZone::VerticalReference::Ground);
    QCOMPARE(polygon.upperReferenceCode, QStringLiteral("AGL"));
    QCOMPARE(prohibited.authorities.count(), 1);
    QVERIFY(prohibited.authorities[0].contains(QStringLiteral("Test Authority")));
    QVERIFY(prohibited.authorities[0].contains(QStringLiteral("zones@example.com")));

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

    QCOMPARE(result.format, GeoZoneParser::Format::ED318);
    QCOMPARE(result.provider, QStringLiteral("Test Authority"));
    QCOMPARE(result.issued, _utc(QDate(2026, 8, 13), QTime(0, 0)));
    QCOMPARE(result.validFrom, _utc(QDate(2026, 8, 13), QTime(0, 0)));
    QCOMPARE(result.validTo, _utc(QDate(2099, 1, 1), QTime(0, 0)));
    QCOMPARE(result.title, QStringLiteral("Test GeoZones"));
    QCOMPARE(result.zones.count(), 3);
    QCOMPARE(result.skippedFeatures, 1); // LineString

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

    // Times without an offset are UTC whatever the local time zone, also in its daylight saving gaps
    const QByteArray offsetless = R"({ "type": "FeatureCollection",
        "metadata": { "issued": "2026-03-08", "validFrom": "2026-03-08T02:30:00", "validTo": "2026-03-29 02:30:00.500" },
        "features": [ { "type": "Feature", "properties": { "type": "PROHIBITED" },
        "geometry": { "type": "Polygon", "coordinates": [ [ [0, 0], [1, 0], [1, 1], [0, 0] ] ] } } ] })";
    QVERIFY2(GeoZoneParser::parse(offsetless, result, errorString), qPrintable(errorString));
    QCOMPARE(result.issued, _utc(QDate(2026, 3, 8), QTime(0, 0)));
    QCOMPARE(result.validFrom, _utc(QDate(2026, 3, 8), QTime(2, 30)));
    QCOMPARE(result.validTo, _utc(QDate(2026, 3, 29), QTime(2, 30, 0, 500)));
}

void GeoZoneParserTest::_testParseFaaTfr()
{
    GeoZoneParser::Result result;
    QString errorString;
    QVERIFY2(GeoZoneParser::parseFile(QStringLiteral(":/unittest/faa_tfr_test.json"), result, errorString), qPrintable(errorString));
    QCOMPARE(result.format, GeoZoneParser::Format::FaaTfr);
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

    // An empty list from the FAA is valid, all restrictions have ended. From other GeoServer feeds it's an error,
    // which keeps their previous zones.
    const QByteArray emptyList = R"({ "type": "FeatureCollection", "features": [], "totalFeatures": 0, "timeStamp": "2026-10-31T12:20:00Z" })";
    QVERIFY2(GeoZoneParser::parse(emptyList, result, errorString, GeoZoneParser::Format::FaaTfr), qPrintable(errorString));
    QCOMPARE(result.format, GeoZoneParser::Format::FaaTfr);
    QVERIFY(result.zones.isEmpty());
    QVERIFY(!GeoZoneParser::parse(emptyList, result, errorString));

    // Live restrictions are refreshed more often than other feeds
    GeoZoneSource source(QStringLiteral("https://tfr.faa.gov/x"), QStringLiteral("https://tfr.faa.gov/x"));
    QVERIFY2(GeoZoneParser::parseFile(QStringLiteral(":/unittest/faa_tfr_test.json"), result, errorString), qPrintable(errorString));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    source.setResult(result, now, now);
    QCOMPARE(source.refreshIntervalSecs(), 15 * 60);
    QVERIFY(!source.refreshDue(now.addSecs(10 * 60)));
    QVERIFY(source.refreshDue(now.addSecs(16 * 60)));

    // Restrictions change during the day, so the list is outdated after hours rather than weeks
    source.updateValidity(result.issued.addSecs(3600));
    QVERIFY(source.validityText().isEmpty());
    source.updateValidity(result.issued.addSecs(3 * 3600));
    QCOMPARE(source.validityText(), QStringLiteral("Data is 3 hours old"));
}

void GeoZoneParserTest::_testParseNationalFormats()
{
    // German DiPUL WFS zones, mapped from their type to a restriction, with limits in meters
    GeoZoneParser::Result dipul;
    QString errorString;
    QFile dipulFile(QStringLiteral(":/unittest/dipul_test.json"));
    QVERIFY(dipulFile.open(QIODevice::ReadOnly));
    QVERIFY2(GeoZoneParser::parse(dipulFile.readAll(), dipul, errorString, GeoZoneParser::Format::Dipul, QStringLiteral("en")), qPrintable(errorString));
    QCOMPARE(dipul.format, GeoZoneParser::Format::Dipul);
    QCOMPARE(dipul.zones.count(), 4);
    const GeoZone::Data &ctr = dipul.zones[0];
    QCOMPARE(ctr.name, QStringLiteral("Frankfurt Main (EDDF) zone 1"));
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
    // An empty area is valid
    const QByteArray emptyArea = R"({ "type": "FeatureCollection", "features": [], "totalFeatures": 0, "numberMatched": 0, "numberReturned": 0 })";
    QVERIFY2(GeoZoneParser::parse(emptyArea, dipul, errorString, GeoZoneParser::Format::Dipul), qPrintable(errorString));
    QVERIFY(dipul.zones.isEmpty());
    QVERIFY(!GeoZoneParser::parse(emptyArea, dipul, errorString));

    // French IGN restrictions, prohibited entirely or above their maximum height, from a limited WFS response
    GeoZoneParser::Result france;
    QFile franceFile(QStringLiteral(":/unittest/france_uas_test.json"));
    QVERIFY(franceFile.open(QIODevice::ReadOnly));
    QVERIFY2(GeoZoneParser::parse(franceFile.readAll(), france, errorString), qPrintable(errorString));
    QCOMPARE(france.format, GeoZoneParser::Format::FranceUas);
    QCOMPARE(france.missingFeatures, 8);
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
    GeoZoneSource source(QStringLiteral("france"), QStringLiteral("https://data.geopf.fr/wfs"));
    source.setResult(france, QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc());
    QVERIFY(source.warningText().contains(QStringLiteral("8")));
    GeoZoneSource areaSource(QStringLiteral("area"), QStringLiteral("https://data.geopf.fr/wfs/ows?SERVICE=WFS&BBOX=2.25,48.8,2.45,48.9,EPSG:4326"));
    areaSource.setResult(france, QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc());
    QCOMPARE(areaSource.name(), QStringLiteral("UAS restrictions (France), area around 48.85, 2.35"));

    // UK flight restriction zones in AIXM 5.1: arcs and geodesic strings, a circle and a danger area
    GeoZoneParser::Result aixm;
    QFile aixmFile(QStringLiteral(":/unittest/uk_aixm_test.xml"));
    QVERIFY(aixmFile.open(QIODevice::ReadOnly));
    const QByteArray aixmBytes = aixmFile.readAll();
    QVERIFY2(GeoZoneParser::parse(aixmBytes, aixm, errorString), qPrintable(errorString));
    QCOMPARE(aixm.format, GeoZoneParser::Format::Aixm);
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

    // The NATS page links the datasets of the current and the next AIRAC cycle
    const QByteArray page = "<!DOCTYPE html><html><body>"
        "<a href=\"/cms-nats/export/sites/default/en/Publications/digital-datasets/UAS_AREA_1/EG_UAS_FR_DS_AREA1_FULL_20261029_XML.zip\">XML</a>"
        "<a href=\"/cms-nats/export/sites/default/en/Publications/digital-datasets/UAS_AREA_1/EG_UAS_FR_DS_AREA1_FULL_20261126_XML.zip\">XML</a>"
        "</body></html>";
    const QString pageUrl = QStringLiteral("https://nats-uk.ead-it.com/cms-nats/opencms/en/Publications/digital-datasets/");
    QVERIFY(GeoZoneLoader::datasetLink(pageUrl, page, QDate(2026, 11, 1)).endsWith(QStringLiteral("FULL_20261029_XML.zip")));
    QVERIFY(GeoZoneLoader::datasetLink(pageUrl, page, QDate(2026, 11, 26)).startsWith(QStringLiteral("https://nats-uk.ead-it.com/cms-nats/export/")));
    QVERIFY(GeoZoneLoader::datasetLink(pageUrl, page, QDate(2026, 11, 26)).endsWith(QStringLiteral("FULL_20261126_XML.zip")));
    QVERIFY(GeoZoneLoader::datasetLink(pageUrl, page, QDate(2026, 1, 1)).endsWith(QStringLiteral("FULL_20261029_XML.zip")));
    QVERIFY(GeoZoneLoader::datasetLink(pageUrl, aixmBytes, QDate(2026, 11, 1)).isEmpty());

    // Area feeds of providers serving zones by area
    GeoZoneManager manager(QTemporaryDir().path());
    const QGeoRectangle frankfurt(QGeoCoordinate(50.15, 8.55), QGeoCoordinate(50.0, 8.75));
    const QString dipulUrl = manager.areaFeedUrl(QStringLiteral("dipul"), frankfurt);
    QVERIFY(dipulUrl.contains(QStringLiteral("dipul:wohngrundstuecke")));
    QVERIFY(dipulUrl.endsWith(QStringLiteral("&BBOX=8.55000,50.00000,8.75000,50.15000")));
    QCOMPARE(GeoZoneLoader::formatHint(dipulUrl), GeoZoneParser::Format::Dipul);
    QVERIFY(manager.areaFeedUrl(QStringLiteral("france"), frankfurt).contains(QStringLiteral("BBOX=8.55000,50.00000,8.75000,50.15000,EPSG:4326")));
    QVERIFY(manager.areaFeedUrl(QStringLiteral("dipul"), QGeoRectangle(QGeoCoordinate(51, 8), QGeoCoordinate(49, 10))).isEmpty());
    QVERIFY(manager.areaFeedUrl(QStringLiteral("unknown"), frankfurt).isEmpty());
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);
    QVERIFY(!manager.addArea(QStringLiteral("dipul"), QGeoRectangle(QGeoCoordinate(51, 8), QGeoCoordinate(49, 10))));
    QCOMPARE(failedSpy.count(), 1);
}

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
        if (url.contains(QStringLiteral("uas-betrieb.de")) || url.contains(QStringLiteral("ead-it.com"))) {
            manager.addUrl(url);
        }
    }
    QVERIFY(manager.addArea(QStringLiteral("dipul"), QGeoRectangle(QGeoCoordinate(50.15, 8.55), QGeoCoordinate(50.0, 8.75))));
    QVERIFY(manager.addArea(QStringLiteral("france"), QGeoRectangle(QGeoCoordinate(48.9, 2.25), QGeoCoordinate(48.8, 2.45))));
    QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 120000);
    for (const QVariantList &failure : std::as_const(failedSpy)) {
        qWarning() << failure;
    }
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(manager.sources()->count(), 4);
    for (int i = 0; i < manager.sources()->count(); i++) {
        const GeoZoneSource *const source = manager.sources()->value<GeoZoneSource*>(i);
        qDebug() << source->name() << source->detailsText() << source->warningText();
        QVERIFY2(source->zoneCount() > 0, qPrintable(source->key()));
    }
}

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

    // Latitude out of range
    QVERIFY(!GeoZoneParser::parse(R"({ "type": "FeatureCollection", "features": [ { "type": "Feature", "properties": { "type": "PROHIBITED" },
        "geometry": { "type": "Polygon", "coordinates": [ [ [0, 95], [1, 95], [1, 96], [0, 95] ] ] } } ] })", result, errorString));
    QCOMPARE(result.skippedFeatures, 1);
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
                (void) zone.verticalLimitsText();
                (void) zone.applicabilityText();
            }
        }
    }
    // Many mutations still yield documents with zones, so the zone functions are exercised too
    QVERIFY2(loadedCount > 400, qPrintable(QString::number(loadedCount)));
}

void GeoZoneParserTest::_testDisplayText()
{
    const GeoZoneParser::Result ed269 = _parseResource(QStringLiteral("ed269_test.json"));
    QCOMPARE(ed269.zones.count(), 3);

    Fact *const verticalUnits = SettingsManager::instance()->unitsSettings()->verticalDistanceUnits();
    const QVariant savedVerticalUnits = verticalUnits->rawValue();
    const auto restoreVerticalUnits = qScopeGuard([verticalUnits, savedVerticalUnits] { verticalUnits->setRawValue(savedVerticalUnits); });
    verticalUnits->setRawValue(UnitsSettings::VerticalDistanceUnitsMeters);

    const GeoZone prohibited(ed269.zones[0]);
    QCOMPARE(prohibited.verticalLimitsText(), QStringLiteral("GND - 120 m AGL\n120 m AGL - Unlimited"));
    QCOMPARE(prohibited.applicabilityText(), QStringLiteral("Permanent"));
    QCOMPARE(prohibited.reasonText(), QStringLiteral("Air traffic, Sensitive area"));

    // Limits are shown in the user's units with the published value when it differs
    const GeoZone circle(ed269.zones[1]);
    QCOMPARE(circle.verticalLimitsText(), QStringLiteral("GND - 122 m AGL (400 ft)"));

    verticalUnits->setRawValue(UnitsSettings::VerticalDistanceUnitsFeet);
    QCOMPARE(prohibited.verticalLimitsText(), QStringLiteral("GND - 394 ft AGL (120 m)\n394 ft AGL (120 m) - Unlimited"));
    QCOMPARE(circle.verticalLimitsText(), QStringLiteral("GND - 400 ft AGL"));

    const GeoZone scheduled(ed269.zones[2]);
    QCOMPARE(scheduled.verticalLimitsText(), QStringLiteral("Not specified"));
    QCOMPARE(scheduled.applicabilityText(), QStringLiteral("From 2026-03-29 01:00 UTC to 2026-10-25 00:59 UTC\n  MON, TUE 05:00-18:00 UTC"));
}
