/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMissionPathTest.h"
#include "GeoZoneMissionJumps.h"
#include "GeoZoneMissionPath.h"
#include "GeoZoneMissionPathBuilder.h"
#include "GeoZoneTestHelpers.h"

#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace GeoZoneTestHelpers;

void GeoZoneMissionPathTest::_testMissionPathTimes()
{
    // Mission paths estimate the times in proportion to the distance along the mission
    const QGeoCoordinate start(47, 8);
    const QDate day(2027, 1, 1);
    GeoZoneMissionPath::Segment segment;
    segment.from = start;
    segment.to = start.atDistanceAndAzimuth(4000, 90);
    segment.distance = 4000;
    const GeoZoneMissionPath missionPath = GeoZoneMissionPath::fromSegments({ segment });
    const QList<GeoZonePathEvaluator::Path> missionPaths = missionPath.evaluatorPaths(_utc(day, QTime(10, 0)), _utc(day, QTime(10, 40)));
    QCOMPARE(missionPaths.count(), 1);
    QVERIFY(missionPaths[0].vertices.count() > 2);
    QCOMPARE(missionPaths[0].vertices.constFirst().time, _utc(day, QTime(10, 0)));
    QCOMPARE(missionPaths[0].vertices.constLast().time, _utc(day, QTime(10, 40)));
    const double middleMSecs = (missionPath.paths()[0].distances[1] / 4000) * 40 * 60 * 1000;
    QVERIFY(_near(static_cast<double>(_utc(day, QTime(10, 0)).msecsTo(missionPaths[0].vertices[1].time)), middleMSecs));
}

namespace {

/// Mission items for GeoZoneMissionJumps, with the drawn segments between consecutive items with a coordinate
struct JumpMission {
    QList<GeoZoneMissionItem> items;
    QList<GeoZoneMissionJumps::ItemSegments> itemSegments;
    QList<double> segmentDistances;

    /// Adds an item flown to, with the drawn segment from the previous one
    void addWaypoint(const QGeoCoordinate &coordinate, bool drawn = true)
    {
        GeoZoneMissionItem item;
        item.sequenceNumber = static_cast<int>(items.count());
        item.lastSequenceNumber = item.sequenceNumber;
        item.flies = true;
        item.entry = GeoZone::Position{ coordinate, 450 };
        item.exit = item.entry;
        const auto previous = std::find_if(items.crbegin(), items.crend(), [](const GeoZoneMissionItem &other) { return other.flies; });
        if (drawn && (previous != items.crend())) {
            itemSegments[std::distance(previous, items.crend()) - 1].outgoing = static_cast<int>(segmentDistances.count());
            segmentDistances.append(previous->exit.coordinate.distanceTo(coordinate));
        }
        items.append(item);
        itemSegments.append(GeoZoneMissionJumps::ItemSegments());
    }

    void addCommand(GeoZoneMissionItem::Kind kind, int jumpSequence = -1, int jumpRepeat = 0)
    {
        GeoZoneMissionItem item;
        item.kind = kind;
        item.sequenceNumber = static_cast<int>(items.count());
        item.lastSequenceNumber = item.sequenceNumber;
        item.jumpSequence = jumpSequence;
        item.jumpRepeat = jumpRepeat;
        items.append(item);
        itemSegments.append(GeoZoneMissionJumps::ItemSegments());
    }

    GeoZoneMissionJumps::Walk walk(int returnSegment = -1, int maxSteps = GeoZoneMissionPath::maxFlightSegments) const
    {
        return GeoZoneMissionJumps::walk(items, itemSegments, segmentDistances, returnSegment, maxSteps);
    }
};

/// The drawn segments and straight legs of the walk, like "0 1 2 3>1 1 2"
QString stepsText(const GeoZoneMissionJumps::Walk &walk)
{
    QStringList steps;
    for (const GeoZoneMissionJumps::Step &step : walk.steps) {
        steps.append((step.segment >= 0) ? QString::number(step.segment) : QStringLiteral("%1>%2").arg(step.fromItem).arg(step.toItem));
    }
    return steps.join(QChar(u' '));
}

} // namespace

void GeoZoneMissionPathTest::_testMissionJumpWalk()
{
    using Kind = GeoZoneMissionItem::Kind;
    // Home, then items 1 to 3 around a square, a DO_JUMP to item 1 and item 5
    const QGeoCoordinate home(47, 8);
    JumpMission mission;
    mission.addWaypoint(home);
    mission.addWaypoint(_offsetFrom(home, 1000, 0));
    mission.addWaypoint(_offsetFrom(home, 1000, 1000));
    mission.addWaypoint(_offsetFrom(home, 0, 1000));
    mission.addCommand(Kind::Jump, 1, 2);
    mission.addWaypoint(_offsetFrom(home, 0, 3000));
    QCOMPARE(mission.segmentDistances.count(), 4);
    QVERIFY(GeoZoneMissionJumps::hasJumps(mission.items));

    // The loop and the straight leg back to item 1 are flown twice more
    GeoZoneMissionJumps::Walk walk = mission.walk();
    QCOMPARE(stepsText(walk), QStringLiteral("0 1 2 3>1 1 2 3>1 1 2 3"));
    QCOMPARE(walk.steps[3].label, QStringLiteral("the jump at item 4"));
    QVERIFY(walk.steps[1].label.isEmpty());
    const double loop = mission.segmentDistances[1] + mission.segmentDistances[2] + walk.steps[3].distance;
    QVERIFY(std::abs(walk.steps[3].distance - std::hypot(1000.0, 1000.0)) < 1);
    const double drawn = std::accumulate(mission.segmentDistances.cbegin(), mission.segmentDistances.cend(), 0.0);
    QVERIFY(std::abs(walk.distance - (drawn + (2 * loop))) < 0.01);
    // Where the vehicle first reaches the items
    QCOMPARE(walk.itemDistances.value(1), mission.segmentDistances[0]);
    QCOMPARE(walk.itemDistances.value(3), mission.segmentDistances[0] + mission.segmentDistances[1] + mission.segmentDistances[2]);
    QVERIFY(std::abs(walk.itemDistances.value(5) - (drawn + (2 * loop))) < 0.01);
    QVERIFY(!walk.itemDistances.contains(4));
    QVERIFY(!walk.repeatsIndefinitely);

    // The builder makes the path of the mission from the items with their drawn segments, without a mission controller
    QList<GeoZoneMissionPathBuilder::Item> builderItems;
    int previous = -1;
    for (int i = 0; i < mission.items.count(); i++) {
        builderItems.append(GeoZoneMissionPathBuilder::Item{ mission.items[i] });
        if (!mission.items[i].flies) {
            continue;
        }
        if (previous >= 0) {
            GeoZoneMissionPath::Segment segment;
            segment.from = mission.items[previous].exit.coordinate;
            segment.to = mission.items[i].entry.coordinate;
            segment.fromAltitudeAMSL = mission.items[previous].exit.altitudeAMSL;
            segment.toAltitudeAMSL = mission.items[i].entry.altitudeAMSL;
            segment.distance = segment.from.distanceTo(segment.to);
            builderItems[previous].outgoing = segment;
        }
        previous = i;
    }
    const GeoZoneMissionPath path = GeoZoneMissionPathBuilder::build(builderItems, GeoZoneMissionPathBuilder::Parameters());
    QCOMPARE(path.segments().count(), 4);
    QCOMPARE(path.legs().count(), 4);
    QVERIFY(std::abs(path.flightDistance() - walk.distance) < 0.01);
    QCOMPARE(path.itemFlightDistance(5), walk.itemDistances.value(5));
    QCOMPARE(std::count_if(path.paths().cbegin(), path.paths().cend(), [](const GeoZoneMissionPath::Path &flown) {
        return flown.label == QStringLiteral("the jump at item 4");
    }), 2);

    // Without repeats the drawn path is flown
    mission.items[4].jumpRepeat = 0;
    QCOMPARE(stepsText(mission.walk()), QStringLiteral("0 1 2 3"));
    // Without end the loop is flown once more
    mission.items[4].jumpRepeat = -1;
    walk = mission.walk();
    QCOMPARE(stepsText(walk), QStringLiteral("0 1 2 3>1 1 2 3"));
    QVERIFY(walk.repeatsIndefinitely);
    QCOMPARE(walk.steps[3].label, QStringLiteral("the jump at item 4, repeated without end"));
    // Many repeats stop after the steps flown
    mission.items[4].jumpRepeat = 1000000;
    walk = mission.walk(-1, 20);
    QVERIFY(walk.steps.count() >= 20);
    QVERIFY(walk.steps.count() <= 24);
    QCOMPARE(walk.steps.constLast().segment, 3);
    // A jump to an unknown item, or to itself, isn't one
    mission.items[4].jumpSequence = 42;
    QVERIFY(!GeoZoneMissionJumps::hasJumps(mission.items));
    QCOMPARE(stepsText(mission.walk()), QStringLiteral("0 1 2 3"));
    mission.items[4].jumpSequence = 4;
    QVERIFY(!GeoZoneMissionJumps::hasJumps(mission.items));

    // A jump to a pattern jumps to its first item, the jump target being any of its sequence numbers
    JumpMission patterns;
    patterns.addWaypoint(home);
    patterns.addWaypoint(_offsetFrom(home, 1000, 0));
    patterns.items[1].kind = Kind::Pattern;
    patterns.items[1].lastSequenceNumber = 9;
    patterns.items[1].exit.coordinate = _offsetFrom(home, 1000, 500);
    patterns.itemSegments[1].segments = { static_cast<int>(patterns.segmentDistances.count()) };
    patterns.segmentDistances.append(500);
    patterns.addWaypoint(_offsetFrom(home, 0, 500));
    patterns.items[2].sequenceNumber = patterns.items[2].lastSequenceNumber = 10;
    patterns.addCommand(Kind::Jump, 5, 1);
    patterns.items[3].sequenceNumber = patterns.items[3].lastSequenceNumber = 11;
    QCOMPARE(stepsText(patterns.walk()), QStringLiteral("0 1 2 2>1 1 2"));

    // The drawn flight path ends with the segment home before the first return to launch, which a jump skips here
    JumpMission skipped;
    skipped.addWaypoint(home);
    skipped.addWaypoint(_offsetFrom(home, 1000, 0));
    skipped.addCommand(Kind::Jump, 4, 1);
    skipped.addCommand(Kind::ReturnToLaunch);
    skipped.addWaypoint(_offsetFrom(home, 1000, 2000), false);
    skipped.addWaypoint(_offsetFrom(home, 0, 2000), false);
    skipped.addCommand(Kind::ReturnToLaunch);
    skipped.itemSegments[1].outgoing = static_cast<int>(skipped.segmentDistances.count());
    skipped.segmentDistances.append(1000);
    walk = skipped.walk(1);
    // On to the items after it, whose legs aren't drawn, until the second return to launch, without the segment home
    QCOMPARE(stepsText(walk), QStringLiteral("0 1>4 4>5"));
    QCOMPARE(walk.steps[1].label, QStringLiteral("the jump at item 2"));
    QCOMPARE(walk.steps[2].label, QStringLiteral("the jump at item 2"));
    QVERIFY(std::abs(walk.steps[2].distance - 1000) < 1);
    QCOMPARE(walk.returnItem, 6);
    QCOMPARE(walk.lastCoordinateItem, 5);
    // Without the jump, the first return to launch ends the flight along the drawn segment home
    skipped.items[2].jumpRepeat = 0;
    walk = skipped.walk(1);
    QCOMPARE(stepsText(walk), QStringLiteral("0 1"));
    QCOMPARE(walk.returnItem, 3);
    QCOMPARE(walk.lastCoordinateItem, 1);
    // Nor is the segment home flown when a jump reaches the return from another item
    skipped.items[2].jumpRepeat = 1;
    skipped.items[6].kind = Kind::Other;
    skipped.addCommand(Kind::Jump, 3, 1);
    walk = skipped.walk(1);
    QCOMPARE(stepsText(walk), QStringLiteral("0 1>4 4>5"));
    QCOMPARE(walk.returnItem, 3);
    QCOMPARE(walk.lastCoordinateItem, 5);
}

GEOZONE_TEST(GeoZoneMissionPathTest)
