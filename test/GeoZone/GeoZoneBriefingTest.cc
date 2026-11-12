/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneBriefingTest.h"
#include "GeoZoneBriefing.h"
#include "GeoZonePlanAudit.h"
#include "GeoZoneSourceVersion.h"
#include "GeoZoneTestHelpers.h"

using namespace GeoZoneTestHelpers;

void GeoZoneBriefingTest::_testBriefing()
{
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2027, 1, 1), QTime(8, 0)));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    BriefingMission mission(dir);
    QString errorString;
    QVERIFY2(mission.load(errorString), qPrintable(errorString));
    const GeoZone *const ctr = mission.zones.zoneList().constFirst();
    const QDateTime &start = mission.start;
    const GeoZoneBriefing briefing = mission.build();

    // The zones entered in the order the path enters them, where, when and at which altitudes
    const QList<const GeoZoneBriefing::Zone*> entered = briefing.zonesWith(GeoZoneBriefing::Involvement::Entered);
    QCOMPARE(entered.count(), 2);
    const GeoZoneBriefing::Zone &briefedCtr = *entered[0];
    QCOMPARE(briefedCtr.identifier, QStringLiteral("CTR-1"));
    QCOMPARE(briefedCtr.restriction, GeoZone::ReqAuthorisation);
    QCOMPARE(briefedCtr.country, QStringLiteral("CHE"));
    QCOMPARE(briefedCtr.crossings.count(), 1);
    const GeoZoneBriefing::Crossing &crossing = briefedCtr.crossings.constFirst();
    QCOMPARE(crossing.entryItem, 1);
    QVERIFY(qAbs(crossing.entryDistance - 800) < 2);
    QVERIFY(qAbs(crossing.exitDistance - 1200) < 2);
    QVERIFY(qAbs(crossing.entryTime.secsTo(start.addSecs(120))) <= 1);
    QCOMPARE(crossing.pathAltitudeMin, 100.0);
    QCOMPARE(crossing.pathAltitudeMax, 100.0);
    QVERIFY(qIsNaN(crossing.zoneFloor));
    QCOMPARE(crossing.zoneCeiling, 500.0);
    QVERIFY(briefedCtr.authorities.join(QChar(u'\n')).contains(QStringLiteral("Skyguide")));
    QVERIFY(briefedCtr.authorities.join(QChar(u'\n')).contains(QStringLiteral("ops@example.org")));
    QCOMPARE(briefedCtr.message, QStringLiteral("Call the tower"));
    QVERIFY(!briefedCtr.schedule.isEmpty());
    // Active from 10:45 within the flight widened by the tolerance, 10:15 to 10:55
    QVERIFY(briefedCtr.activeDuringFlight);
    QCOMPARE(briefedCtr.activePeriods.count(), 1);
    QCOMPARE(briefedCtr.activePeriods[0].start, _utc(QDate(2027, 1, 1), QTime(10, 45)));
    QCOMPARE(briefedCtr.activePeriods[0].end, _utc(QDate(2027, 1, 1), QTime(10, 55)));
    QVERIFY(briefedCtr.authorisation.has_value());
    QCOMPARE(briefedCtr.authorisation->permit, QStringLiteral("P-77"));
    QVERIFY(briefedCtr.authorisation->coversFlight);
    QCOMPARE(briefedCtr.contentHash, ctr->data().contentHash);
    QCOMPARE(entered[1]->displayName(), QStringLiteral("Prison"));
    QCOMPARE(entered[1]->activePeriods, (QList<GeoZoneBriefing::Interval>{ { start.addSecs(-15 * 60), start.addSecs(25 * 60) } }));
    QVERIFY(!entered[1]->authorisation);

    // The zone only the return enters, and the restricted zone near the path
    const QList<const GeoZoneBriefing::Zone*> contingencyZones = briefing.zonesWith(GeoZoneBriefing::Involvement::Contingency);
    QCOMPARE(contingencyZones.count(), 1);
    QCOMPARE(contingencyZones[0]->identifier, QStringLiteral("RET"));
    QCOMPARE(contingencyZones[0]->contingencies, QStringList{ QStringLiteral("a return to rally point 1") });
    const QList<const GeoZoneBriefing::Zone*> nearby = briefing.zonesWith(GeoZoneBriefing::Involvement::Nearby);
    QCOMPARE(nearby.count(), 1);
    QCOMPARE(nearby[0]->identifier, QStringLiteral("NEAR"));
    QVERIFY(qAbs(nearby[0]->distance - 250) < 5);

    // The version of the data
    QCOMPARE(briefing.sources.count(), 1);
    QVERIFY(!briefing.sources[0].contentHash.isEmpty());
    QCOMPARE(GeoZoneSourceVersion::airacCycleOf(QDate(2026, 1, 22)), QStringLiteral("2601"));
    QCOMPARE(GeoZoneSourceVersion::airacCycleOf(QDate(2026, 1, 21)), QStringLiteral("2513"));
    QCOMPARE(GeoZoneSourceVersion::airacCycleOf(QDate(2020, 12, 31)), QStringLiteral("2014"));
    QCOMPARE(GeoZoneSourceVersion::airacCycleOf(QDate(2021, 1, 28)), QStringLiteral("2101"));
    QCOMPARE(GeoZoneSourceVersion::airacCycleOf(QDate(2019, 12, 31)), QStringLiteral("1913"));
}

void GeoZoneBriefingTest::_testEnteredVolume()
{
    // A zone of two volumes above each other, up to 50 m and from 200 m to 300 m, and one of two volumes which overlap,
    // from 100 m to 260 m and from 240 m to 400 m
    const QGeoCoordinate center(47, 8);
    const auto twoVolumes = [](QJsonObject feature, double lower, double upper) {
        QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
        QJsonObject volume = geometry[0].toObject();
        volume[QStringLiteral("lowerLimit")] = lower;
        volume[QStringLiteral("upperLimit")] = upper;
        geometry.append(volume);
        feature[QStringLiteral("geometry")] = geometry;
        return feature;
    };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        twoVolumes(_withLimits(_circleFeature(QStringLiteral("STACKED"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1000, 90), 200),
                               0, QStringLiteral("AMSL"), 50, QStringLiteral("AMSL")), 200, 300),
        twoVolumes(_withLimits(_circleFeature(QStringLiteral("OVERLAPPING"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(3000, 90), 200),
                               100, QStringLiteral("AMSL"), 260, QStringLiteral("AMSL")), 240, 400),
    }), errorString), qPrintable(errorString));
    QCOMPARE(zones.zoneList().count(), 2);
    QCOMPARE(zones.zoneList().constFirst()->data().volumes.count(), 2);

    GeoZoneBriefing::Input input;
    input.paths = { GeoZonePathEvaluator::Path::fromPositions({ GeoZone::Position{ center, 250 }, GeoZone::Position{ center.atDistanceAndAzimuth(4000, 90), 250 } }) };
    const GeoZoneBriefing briefing = GeoZoneBriefing::build(zones.catalog(), input);
    const QList<const GeoZoneBriefing::Zone*> entered = briefing.zonesWith(GeoZoneBriefing::Involvement::Entered);
    QCOMPARE(entered.count(), 2);

    // The limits at the entry are those of the volume entered, not also of the one passed above
    QCOMPARE(entered[0]->identifier, QStringLiteral("STACKED"));
    QCOMPARE(entered[0]->crossings.count(), 1);
    QCOMPARE(entered[0]->crossings.constFirst().zoneFloor, 200.0);
    QCOMPARE(entered[0]->crossings.constFirst().zoneCeiling, 300.0);
    // Of volumes entered at once, the lowest floor and the highest ceiling
    QCOMPARE(entered[1]->identifier, QStringLiteral("OVERLAPPING"));
    QCOMPARE(entered[1]->crossings.count(), 1);
    QCOMPARE(entered[1]->crossings.constFirst().zoneFloor, 100.0);
    QCOMPARE(entered[1]->crossings.constFirst().zoneCeiling, 400.0);
}

void GeoZoneBriefingTest::_testPlanAuditRecord()
{
    // A zone of each restriction
    const QList<GeoZone::Restriction> restrictions = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::NoRestriction,
                                                       GeoZone::USpace, GeoZone::Unknown };
    GeoZonePlanAudit::Record record;
    record.savedAt = _utc(QDate(2027, 1, 1), QTime(8, 0));
    for (const GeoZone::Restriction restriction : restrictions) {
        GeoZonePlanAudit::Zone zone;
        zone.key = QStringLiteral("zones.json/Z%1").arg(record.zones.count());
        zone.identifier = QStringLiteral("Z%1").arg(record.zones.count());
        zone.restriction = restriction;
        zone.contentHash = QByteArray::fromHex("0123456789abcdef");
        record.zones.append(zone);
    }

    // Kept with the restrictions as their stable codes, whatever the numbers of the enumerators
    const QJsonObject json = record.toJson();
    QCOMPARE(json[QStringLiteral("version")].toInt(), 2);
    QCOMPARE(GeoZonePlanAudit::version, 2);
    const QJsonArray zones = json[QStringLiteral("zones")].toArray();
    QCOMPARE(zones.count(), restrictions.count());
    const QStringList codes = { QStringLiteral("prohibited"), QStringLiteral("req_authorisation"), QStringLiteral("conditional"), QStringLiteral("no_restriction"),
                                QStringLiteral("u_space"), QStringLiteral("unknown") };
    for (qsizetype i = 0; i < zones.count(); i++) {
        QCOMPARE(zones[i].toObject()[QStringLiteral("restriction")].toString(), codes[i]);
        QCOMPARE(GeoZoneBriefing::restrictionFromCode(codes[i]), restrictions[i]);
    }
    QCOMPARE(GeoZoneBriefing::restrictionFromCode(QStringLiteral("something else")), GeoZone::Unknown);
    const std::optional<GeoZonePlanAudit::Record> read = GeoZonePlanAudit::Record::fromJson(json);
    QVERIFY(read.has_value());
    QCOMPARE(read->zones, record.zones);
    QCOMPARE(read->savedAt, record.savedAt);

    // Records of version 1 kept the numbers of the enumerators, and are still read
    QJsonObject version1 = json;
    version1[QStringLiteral("version")] = 1;
    QJsonArray numbered;
    for (qsizetype i = 0; i < zones.count(); i++) {
        QJsonObject zone = zones[i].toObject();
        zone[QStringLiteral("restriction")] = static_cast<int>(i);
        numbered.append(zone);
    }
    QJsonObject outOfRange = zones[0].toObject();
    outOfRange[QStringLiteral("key")] = QStringLiteral("zones.json/other");
    outOfRange[QStringLiteral("restriction")] = 42;
    numbered.append(outOfRange);
    version1[QStringLiteral("zones")] = numbered;
    const std::optional<GeoZonePlanAudit::Record> readVersion1 = GeoZonePlanAudit::Record::fromJson(version1);
    QVERIFY(readVersion1.has_value());
    QCOMPARE(readVersion1->zones.count(), restrictions.count() + 1);
    QCOMPARE(readVersion1->zones.first(restrictions.count()), record.zones);
    QCOMPARE(readVersion1->zones.constLast().restriction, GeoZone::Unknown);

    // A record of a newer version, whose fields may mean something else, isn't read, nor JSON which isn't a record
    QJsonObject newer = json;
    newer[QStringLiteral("version")] = GeoZonePlanAudit::version + 1;
    QVERIFY(!GeoZonePlanAudit::Record::fromJson(newer).has_value());
    QJsonObject fractional = json;
    fractional[QStringLiteral("version")] = 1.5;
    QVERIFY(!GeoZonePlanAudit::Record::fromJson(fractional).has_value());
    QJsonObject versionless = json;
    versionless.remove(QStringLiteral("version"));
    QVERIFY(!GeoZonePlanAudit::Record::fromJson(versionless).has_value());
}

GEOZONE_TEST(GeoZoneBriefingTest)
