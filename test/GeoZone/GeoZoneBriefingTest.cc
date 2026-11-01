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
