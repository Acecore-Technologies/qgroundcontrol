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
#ifndef GEOZONE_CORE_TEST
#include "GeoZoneMissionReader.h"
#include "RallyPoint.h"
#include "RallyPointController.h"
#endif

#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace GeoZoneTestHelpers;

#ifndef GEOZONE_CORE_TEST
GeoZoneMissionPathTest::GeoZoneMissionPathTest() = default;

GeoZoneMissionPathTest::~GeoZoneMissionPathTest() = default;

void GeoZoneMissionPathTest::initTestCase()
{
    _metricUnits = std::make_unique<ScopedMetricUnits>();
}

void GeoZoneMissionPathTest::cleanupTestCase()
{
    _metricUnits.reset();
}
#endif

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

void GeoZoneMissionPathTest::_testFlightEndsAtReturn()
{
    using Kind = GeoZoneMissionItem::Kind;
    // Home, two waypoints, a return to launch with the drawn segment home, then a pattern with a flight line of its own,
    // which the mission controller keeps drawing though the mission ends at the return
    const QGeoCoordinate home(47, 8);
    const QGeoCoordinate first = _offsetFrom(home, 1000, 0);
    const QGeoCoordinate second = _offsetFrom(home, 1000, 1000);
    const QGeoCoordinate patternEntry = _offsetFrom(home, 0, 3000);
    const QGeoCoordinate patternExit = _offsetFrom(home, 0, 4000);
    const auto segment = [](const QGeoCoordinate &from, const QGeoCoordinate &to) {
        GeoZoneMissionPath::Segment drawn;
        drawn.from = from;
        drawn.to = to;
        drawn.fromAltitudeAMSL = drawn.toAltitudeAMSL = 450;
        drawn.distance = from.distanceTo(to);
        return drawn;
    };
    const auto item = [](int sequenceNumber, Kind kind, const QGeoCoordinate &entry = QGeoCoordinate(), const QGeoCoordinate &exit = QGeoCoordinate()) {
        GeoZoneMissionPathBuilder::Item builderItem;
        builderItem.item.kind = kind;
        builderItem.item.sequenceNumber = builderItem.item.lastSequenceNumber = sequenceNumber;
        builderItem.item.flies = entry.isValid();
        builderItem.item.entry = GeoZone::Position{ entry, 450 };
        builderItem.item.exit = GeoZone::Position{ exit.isValid() ? exit : entry, 450 };
        return builderItem;
    };
    QList<GeoZoneMissionPathBuilder::Item> items = { item(0, Kind::Other, home), item(1, Kind::Other, first), item(2, Kind::Other, second), item(3, Kind::ReturnToLaunch),
                                                     item(4, Kind::Pattern, patternEntry, patternExit) };
    items[1].outgoing = segment(first, second);
    items[2].outgoing = segment(second, home);
    items[4].segments = { segment(patternEntry, patternExit) };
    GeoZoneMissionPathBuilder::Parameters parameters;
    parameters.home = home;
    const double untilReturn = first.distanceTo(second) + second.distanceTo(home);
    const auto flownTo = [](const GeoZoneMissionPath &path, const QGeoCoordinate &coordinate) {
        return std::any_of(path.paths().cbegin(), path.paths().cend(), [&coordinate](const GeoZoneMissionPath::Path &flown) {
            return std::any_of(flown.positions.cbegin(), flown.positions.cend(), [&coordinate](const GeoZone::Position &position) {
                return position.coordinate.distanceTo(coordinate) < 1;
            });
        });
    };
    const QDateTime start = _utc(QDate(2027, 1, 1), QTime(10, 0));
    const QDateTime end = start.addSecs(600);

    // The flight ends at the return: the flight line of the pattern after it isn't flown, nor counted in the distance the
    // duration of the mission is of
    const GeoZoneMissionPath path = GeoZoneMissionPathBuilder::build(items, parameters);
    QCOMPARE(path.segments().count(), 3);
    QVERIFY(flownTo(path, second));
    QVERIFY(flownTo(path, home));
    QVERIFY(!flownTo(path, patternEntry));
    QVERIFY(!flownTo(path, patternExit));
    QVERIFY(_near(path.flightDistance(), untilReturn, 0.01));
    QVERIFY(_near(path.drawnFlightDistance(), untilReturn, 0.01));
    QVERIFY(_near(path.totalDistance(), untilReturn + 1000, 1));
    QCOMPARE(path.flightEnd(start, end), end);
    const QList<GeoZonePathEvaluator::Path> evaluatorPaths = path.evaluatorPaths(start, end);
    QCOMPARE(evaluatorPaths.count(), 1);
    QVERIFY(!evaluatorPaths.constFirst().vertices.isEmpty());
    QCOMPARE(evaluatorPaths.constFirst().vertices.constLast().time, end);

    // Like the flight which follows a DO_JUMP repeating the leg before the return
    QList<GeoZoneMissionPathBuilder::Item> jumping = items;
    jumping.insert(3, item(3, Kind::Jump));
    jumping[3].item.jumpSequence = 1;
    jumping[3].item.jumpRepeat = 1;
    for (int i = 4; i < jumping.count(); i++) {
        jumping[i].item.sequenceNumber = jumping[i].item.lastSequenceNumber = i;
    }
    const GeoZoneMissionPath jumped = GeoZoneMissionPathBuilder::build(jumping, parameters);
    QVERIFY(flownTo(jumped, second));
    QVERIFY(flownTo(jumped, home));
    QVERIFY(!flownTo(jumped, patternEntry));
    QVERIFY(!flownTo(jumped, patternExit));
    const double loop = first.distanceTo(second) + second.distanceTo(first);
    QVERIFY(_near(jumped.flightDistance(), untilReturn + loop, 0.01));
    QVERIFY(_near(jumped.drawnFlightDistance(), untilReturn, 0.01));
    // The repeat lengthens the flight at the pace of the drawn flight path, without the pattern
    QVERIFY(_near(static_cast<double>(start.msecsTo(jumped.flightEnd(start, end))), 600000 * (untilReturn + loop) / untilReturn, 2));

    // Without a return the pattern is flown
    QList<GeoZoneMissionPathBuilder::Item> withoutReturn = items;
    withoutReturn[3] = item(3, Kind::Other);
    const GeoZoneMissionPath whole = GeoZoneMissionPathBuilder::build(withoutReturn, parameters);
    QVERIFY(flownTo(whole, patternExit));
    QVERIFY(_near(whole.flightDistance(), whole.totalDistance(), 0.01));
    QVERIFY(_near(whole.drawnFlightDistance(), whole.totalDistance(), 0.01));
}

#ifndef GEOZONE_CORE_TEST
// The paths of the missions of a MissionController, read by GeoZoneMissionReader

namespace {

/// The feature with AMSL limits in meters
QJsonObject withAmslLimits(QJsonObject feature, double lower, double upper)
{
    QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = lower;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
    layer[QStringLiteral("upperLimit")] = upper;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AMSL");
    geometry[0] = layer;
    feature[QStringLiteral("geometry")] = geometry;
    return feature;
}

QStringList zoneIdentifiers(const QList<const GeoZone*> &zones)
{
    QStringList identifiers;
    for (const GeoZone *zone : zones) {
        identifiers.append(zone->identifier());
    }
    return identifiers;
}

} // namespace

void GeoZoneMissionPathTest::_testMissionCoverage()
{
    const ScopedEnvironment environment;
    // L under a loiter circle away from the legs, V past a back transition, and R on the way home above the drawn flight
    // path, which descends home, at the altitude of the return
    const QGeoCoordinate home(47, 8);
    const QGeoCoordinate loiter = home.atDistanceAndAzimuth(2000, 90);
    const QGeoCoordinate last = loiter.atDistanceAndAzimuth(2000, 0);
    QJsonObject returnZone = _circleFeature(QStringLiteral("R"), QStringLiteral("PROHIBITED"), home.atDistanceAndAzimuth(home.distanceTo(last) / 2, home.azimuthTo(last)), 100);
    QJsonArray geometry = returnZone[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = 440;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
    layer[QStringLiteral("upperLimit")] = 2000;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AMSL");
    geometry[0] = layer;
    returnZone[QStringLiteral("geometry")] = geometry;
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("L"), QStringLiteral("PROHIBITED"), loiter.atDistanceAndAzimuth(300, 180), 40),
        _circleFeature(QStringLiteral("V"), QStringLiteral("PROHIBITED"), last.atDistanceAndAzimuth(150, 0), 30),
        returnZone,
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    SimpleMissionItem *const loiterItem = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(loiter, 1));
    QVERIFY(loiterItem);
    loiterItem->setCommand(MAV_CMD_NAV_LOITER_UNLIM);
    loiterItem->setRadius(300);
    (void) missionController->insertSimpleMissionItem(last, 2);
    SimpleMissionItem *const transitionItem = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(last, 3));
    transitionItem->setCommand(MAV_CMD_DO_VTOL_TRANSITION);
    transitionItem->missionItem().setParam1(MAV_VTOL_STATE_MC);
    SimpleMissionItem *const returnItem = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(last, 4));
    returnItem->setCommand(MAV_CMD_NAV_RETURN_TO_LAUNCH);

    // The drawn flight path enters none of them. Offline the PX4 multicopter returns at its default altitude, 30 m above
    // home, which is below the items 50 m high.
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 3);
    const auto zone = [&check](const QString &identifier) -> const GeoZone* {
        const QList<const GeoZone*> crossed = check.zones(GeoZoneMissionCheck::Crossed);
        const auto it = std::find_if(crossed.cbegin(), crossed.cend(), [&identifier](const GeoZone *zone) { return zone->identifier() == identifier; });
        return (it != crossed.cend()) ? *it : nullptr;
    };
    QCOMPARE(check.entry(zone(QStringLiteral("L")))->labels, QStringList({ QStringLiteral("the loiter at item 1") }));
    QCOMPARE(check.entry(zone(QStringLiteral("V")))->labels, QStringList({ QStringLiteral("the VTOL transition at item 3") }));
    QCOMPARE(check.entry(zone(QStringLiteral("R")))->labels, QStringList({ QStringLiteral("the return to launch assuming the PX4 default return altitude of 30 m") }));
    QVERIFY2(check.warningText().contains(QStringLiteral("Only the loiter at item 1 enters L.")), qPrintable(check.warningText()));

    // A smaller loiter stays clear of L
    loiterItem->setRadius(100);
    QTRY_VERIFY(!zone(QStringLiteral("L")));
    QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 2);
}

void GeoZoneMissionPathTest::_testMissionJumps()
{
    const ScopedEnvironment environment;
    // A loop north and east of home flown again by a DO_JUMP, then on east. J is on the jump back, away from the drawn path.
    const QGeoCoordinate home(47, 8);
    const QGeoCoordinate north = _offsetFrom(home, 1000, 0);
    const QGeoCoordinate northEast = _offsetFrom(home, 1000, 1000);
    const QGeoCoordinate east = _offsetFrom(home, 0, 1000);
    const QGeoCoordinate farEast = _offsetFrom(home, 0, 3000);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("J"), QStringLiteral("PROHIBITED"), _offsetFrom(home, 500, 500), 50),
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    (void) missionController->insertSimpleMissionItem(north, 1);
    (void) missionController->insertSimpleMissionItem(northEast, 2);
    (void) missionController->insertSimpleMissionItem(east, 3);
    SimpleMissionItem *const jumpItem = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(east, 4));
    QVERIFY(jumpItem);
    jumpItem->setCommand(MAV_CMD_DO_JUMP);
    jumpItem->missionItem().setParam1(1);
    jumpItem->missionItem().setParam2(2);
    (void) missionController->insertSimpleMissionItem(farEast, 5);
    QCOMPARE(jumpItem->sequenceNumber(), 4);
    QTRY_COMPARE(GeoZoneMissionReader::path(missionController).segments().count(), 4);

    // The loop from the first item to the third and the jump back to the first are flown twice more
    const GeoZoneMissionPath path = GeoZoneMissionReader::path(missionController);
    const double loop = path.itemDistance(3) - path.itemDistance(1) + east.distanceTo(north);
    QVERIFY(loop > 3000);
    QVERIFY2(std::abs(path.flightDistance() - (path.totalDistance() + (2 * loop))) < 1, qPrintable(QString::number(path.flightDistance())));
    QCOMPARE(path.itemFlightDistance(3), path.itemDistance(3));
    QVERIFY(std::abs(path.itemFlightDistance(5) - (path.itemDistance(5) + (2 * loop))) < 1);
    QVERIFY(!path.repeatsIndefinitely());
    int jumps = 0;
    for (const GeoZoneMissionPath::Path &flown : path.paths()) {
        if (flown.label == QStringLiteral("the jump at item 4")) {
            jumps++;
            QVERIFY(flown.positions.first().coordinate.distanceTo(east) < 1);
            QVERIFY(flown.positions.last().coordinate.distanceTo(north) < 1);
            // At the distance the vehicle flies it, after the first pass of the loop
            QVERIFY(flown.distances.first() >= path.itemDistance(3));
        }
    }
    QCOMPARE(jumps, 2);
    // The flight lasts longer than the estimate without the loops, at the same pace
    const QDateTime start = _utc(QDate(2026, 6, 15), QTime(10, 0));
    const QDateTime end = start.addSecs(1000);
    QVERIFY(std::abs(start.msecsTo(path.flightEnd(start, end)) - (1000000 * path.flightDistance() / path.totalDistance())) < 2);

    // The mission check reports the zone only the jump enters
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(zoneIdentifiers(check.zones(GeoZoneMissionCheck::Crossed)), QStringList{ QStringLiteral("J") });
    QCOMPARE(check.entry(check.zones(GeoZoneMissionCheck::Crossed).first())->labels, QStringList{ QStringLiteral("the jump at item 4") });
    // Without repeats the jump isn't flown
    for (int i = 0; i < jumpItem->textFieldFacts()->count(); i++) {
        Fact *const fact = jumpItem->textFieldFacts()->value<Fact*>(i);
        if (fact->rawValue().toInt() == 2) {
            fact->setRawValue(0);
        }
    }
    QCOMPARE(qRound(jumpItem->missionItem().param2()), 0);
    QTRY_VERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());

    // Repeating without end the loop is flown once more
    jumpItem->missionItem().setParam2(-1);
    const GeoZoneMissionPath endless = GeoZoneMissionReader::path(missionController);
    QVERIFY(endless.repeatsIndefinitely());
    QVERIFY(std::abs(endless.flightDistance() - (endless.totalDistance() + loop)) < 1);
    const auto endlessJump = std::find_if(endless.paths().cbegin(), endless.paths().cend(), [](const GeoZoneMissionPath::Path &flown) {
        return flown.label == QStringLiteral("the jump at item 4, repeated without end");
    });
    QVERIFY(endlessJump != endless.paths().cend());

    // Many repeats stop at the limit of the segments flown
    jumpItem->missionItem().setParam2(1000000);
    const GeoZoneMissionPath capped = GeoZoneMissionReader::path(missionController);
    QVERIFY(capped.flightDistance() > (capped.totalDistance() + (100 * loop)));
    QVERIFY(capped.flightDistance() < (capped.totalDistance() + (((GeoZoneMissionPath::maxFlightSegments / 3) + 2) * loop)));

    // A jump past a return to launch: the mission goes on to the items after it, whose legs the mission controller
    // doesn't draw, and the vehicle doesn't return. S is on such a leg, R on the return the jump skips.
    const QGeoCoordinate skippedFrom = _offsetFrom(home, 1000, -3000);
    const QGeoCoordinate afterReturn = _offsetFrom(home, 1000, -1000);
    const QGeoCoordinate missionEnd = _offsetFrom(home, 0, -1000);
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("skipped.json"), QJsonArray{
        _circleFeature(QStringLiteral("S"), QStringLiteral("PROHIBITED"), _offsetFrom(home, 500, -1000), 50),
        _circleFeature(QStringLiteral("R"), QStringLiteral("PROHIBITED"), _offsetFrom(home, 500, -1500), 50),
    }), errorString), qPrintable(errorString));
    PlanMasterController skippingController;
    skippingController.setFlyView(false);
    skippingController.start();
    MissionController *const skipping = skippingController.missionController();
    skipping->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    (void) skipping->insertSimpleMissionItem(skippedFrom, 1);
    SimpleMissionItem *const skippingJump = qobject_cast<SimpleMissionItem*>(skipping->insertSimpleMissionItem(skippedFrom, 2));
    skippingJump->setCommand(MAV_CMD_DO_JUMP);
    skippingJump->missionItem().setParam1(4);
    skippingJump->missionItem().setParam2(1);
    qobject_cast<SimpleMissionItem*>(skipping->insertSimpleMissionItem(skippedFrom, 3))->setCommand(MAV_CMD_NAV_RETURN_TO_LAUNCH);
    (void) skipping->insertSimpleMissionItem(afterReturn, 4);
    (void) skipping->insertSimpleMissionItem(missionEnd, 5);
    QCOMPARE(skipping->visualItems()->value<VisualMissionItem*>(5)->sequenceNumber(), 5);
    QTRY_VERIFY(!GeoZoneMissionReader::path(skipping).segments().isEmpty());
    const GeoZoneMissionPath skippedPath = GeoZoneMissionReader::path(skipping);
    const auto flownTo = [&skippedPath](const QGeoCoordinate &coordinate) {
        return std::any_of(skippedPath.paths().cbegin(), skippedPath.paths().cend(), [&coordinate](const GeoZoneMissionPath::Path &flown) {
            return std::any_of(flown.positions.cbegin(), flown.positions.cend(), [&coordinate](const GeoZone::Position &position) {
                return position.coordinate.distanceTo(coordinate) < 1;
            });
        });
    };
    QVERIFY(flownTo(afterReturn));
    QVERIFY(flownTo(missionEnd));
    QVERIFY(std::abs(skippedPath.itemFlightDistance(5) - (skippedFrom.distanceTo(afterReturn) + afterReturn.distanceTo(missionEnd))) < 1);
    GeoZoneMissionCheck skippingCheck(&manager);
    skippingCheck.setUpdateDelay(0);
    skippingCheck.setMissionController(skipping);
    QTRY_COMPARE(zoneIdentifiers(skippingCheck.zones(GeoZoneMissionCheck::Crossed)), QStringList{ QStringLiteral("S") });
    QCOMPARE(skippingCheck.entry(skippingCheck.zones(GeoZoneMissionCheck::Crossed).first())->labels, QStringList{ QStringLiteral("the jump at item 2") });
}

void GeoZoneMissionPathTest::_testMissionContingencies()
{
    const ScopedEnvironment environment;
    // Around a square north and east of home. X is in the middle of the square, where returns home from the second
    // item pass, R on the way from it to a rally point east of it.
    const QGeoCoordinate home(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        withAmslLimits(_circleFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), _offsetFrom(home, 1000, 1000), 100), 440, 1000),
        _circleFeature(QStringLiteral("R"), QStringLiteral("PROHIBITED"), _offsetFrom(home, 2000, 2500), 50),
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    (void) missionController->insertSimpleMissionItem(_offsetFrom(home, 2000, 0), 1);
    (void) missionController->insertSimpleMissionItem(_offsetFrom(home, 2000, 2000), 2);
    (void) missionController->insertSimpleMissionItem(_offsetFrom(home, 0, 2000), 3);

    // The offline PX4 multicopter returns home at the altitude of the items, 50 m above home, higher than its default
    // return altitude. The mission doesn't enter X, returns from around the second item would.
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(zoneIdentifiers(check.zones(GeoZoneMissionCheck::Contingency)), QStringList{ QStringLiteral("X") });
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    QVERIFY(check.warningText().isEmpty());
    const GeoZone *const x = check.zones(GeoZoneMissionCheck::Contingency).first();
    QCOMPARE(check.entry(x)->labels, QStringList{ QStringLiteral(u"a return to launch assuming the PX4 default return altitude of 30 m from items 1\u20133") });
    QCOMPARE(check.contingencyText(), QStringLiteral(u"A return to launch assuming the PX4 default return altitude of 30 m from items 1\u20133 would enter X (Prohibited)."));

    // The starts are grouped into ranges of items
    const GeoZoneMissionPath path = GeoZoneMissionReader::path(missionController, GeoZoneMissionPath::Contingencies::With);
    const QList<GeoZoneMissionPath::ContingencyStart> &starts = path.contingencyStarts();
    QVERIFY(starts.count() > 10);
    QVERIFY(starts.count() <= GeoZoneMissionPath::maxContingencyStarts);
    const auto atItem = [&starts](int sequenceNumber) {
        const auto it = std::find_if(starts.cbegin(), starts.cend(), [sequenceNumber](const GeoZoneMissionPath::ContingencyStart &start) {
            return (start.fromSequenceNumber == sequenceNumber) && (start.toSequenceNumber == sequenceNumber);
        });
        return (it != starts.cend()) ? static_cast<int>(it - starts.cbegin()) : -1;
    };
    const int second = atItem(2);
    QVERIFY((atItem(1) == 0) && (second > 0) && (atItem(3) == (starts.count() - 1)));
    QCOMPARE(path.startsText({ second }), QStringLiteral("item 2"));
    QCOMPARE(path.startsText({ second + 1, second, second - 1 }), QStringLiteral(u"items 1\u20133"));
    QCOMPARE(path.startsText({ 0, second }), QStringLiteral("items 1, 2"));
    QCOMPARE(path.contingencyPaths().count(), starts.count());

    // A rally point closer than home, the returns from around the second item go there
    RallyPointController *const rallyPointController = masterController.rallyPointController();
    rallyPointController->addPoint(_offsetFrom(home, 2000, 3000));
    QTRY_COMPARE(zoneIdentifiers(check.zones(GeoZoneMissionCheck::Contingency)), QStringList{ QStringLiteral("R") });
    QVERIFY(check.entry(check.zones(GeoZoneMissionCheck::Contingency).first())->labels.first().startsWith(QStringLiteral("a return to rally point 1 assuming the PX4 default return altitude of 30 m from items")));
    // Moved away it isn't closer anymore
    rallyPointController->points()->value<RallyPoint*>(0)->setCoordinate(_offsetFrom(home, 6000, 6000));
    QTRY_COMPARE(zoneIdentifiers(check.zones(GeoZoneMissionCheck::Contingency)), QStringList{ QStringLiteral("X") });

    // Zones are released before the manager deletes them
    manager.repository()->clear();
    QVERIFY(check.zones(GeoZoneMissionCheck::Contingency).isEmpty());
    QVERIFY(check.contingencyText().isEmpty());
}

void GeoZoneMissionPathTest::_testMissionReturnParameters()
{
    const ScopedEnvironment environment;
    // X is above the returns at 60 m from around the second item, 70 m above home and higher. B is on the way from
    // the first item to the breach return point west of it.
    const QGeoCoordinate home(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        withAmslLimits(_circleFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), _offsetFrom(home, 1000, 1000), 100), 470, 1000),
        _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), _offsetFrom(home, 2000, -500), 50),
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    for (const QGeoCoordinate &coordinate : { _offsetFrom(home, 2000, 0), _offsetFrom(home, 2000, 2000), _offsetFrom(home, 0, 2000) }) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
        QVERIFY(item);
        item->altitude()->setRawValue(30);
    }
    QTRY_COMPARE(GeoZoneMissionReader::path(missionController).segments().count(), 3);
    const auto contingencies = [&manager, missionController](const GeoZoneReturnModel::Parameters &parameters) {
        return GeoZoneMissionCheck::evaluate(manager.catalog(), GeoZoneMissionReader::path(missionController, parameters, GeoZoneMissionPath::Contingencies::With));
    };

    // Of unknown firmware at an unknown altitude, entering X
    GeoZoneMissionCheck::Evaluation evaluation = contingencies(GeoZoneReturnModel::Parameters());
    QCOMPARE(zoneIdentifiers(evaluation.contingencyZones), QStringList{ QStringLiteral("X") });
    QCOMPARE(evaluation.contingencyLabels.value(evaluation.contingencyZones.first()),
             QStringList{ QStringLiteral(u"a return to launch at an unknown altitude from items 1\u20133") });

    // ArduCopter at 60 m stays below X, at 100 m it enters it
    GeoZoneReturnModel::Parameters copter;
    copter.autopilot = GeoZoneReturnModel::Autopilot::ArduCopter;
    copter.multicopter = true;
    copter.returnAltitude = 60;
    evaluation = contingencies(copter);
    QVERIFY(evaluation.zones.isEmpty());
    QVERIFY(evaluation.contingencyZones.isEmpty());
    QVERIFY(contingencies(GeoZoneReturnModel::defaultParameters(GeoZoneReturnModel::Firmware::ArduPilot, GeoZoneReturnModel::VehicleClass::Multirotor))
                .contingencyZones.isEmpty());
    copter.returnAltitude = 100;
    evaluation = contingencies(copter);
    QCOMPARE(zoneIdentifiers(evaluation.contingencyZones), QStringList{ QStringLiteral("X") });
    QCOMPARE(evaluation.contingencyLabels.value(evaluation.contingencyZones.first()), QStringList{ QStringLiteral(u"a return to launch from items 1\u20133") });
    // The cone of a PX4 multicopter doesn't lower the return this far from home
    GeoZoneReturnModel::Parameters px4;
    px4.autopilot = GeoZoneReturnModel::Autopilot::PX4;
    px4.multicopter = true;
    px4.returnAltitude = 100;
    px4.coneHalfAngle = 45;
    px4.coneRadius = 10;
    QCOMPARE(zoneIdentifiers(contingencies(px4).contingencyZones), QStringList{ QStringLiteral("X") });

    // ArduPlane flying to the breach return point after a fence breach, at FENCE_RET_ALT
    masterController.geoFenceController()->setBreachReturnPoint(_offsetFrom(home, 2000, -1000));
    GeoZoneReturnModel::Parameters plane;
    plane.autopilot = GeoZoneReturnModel::Autopilot::ArduPlane;
    plane.returnAltitude = 50;
    plane.loiterRadius = 80;
    plane.breachAction = GeoZoneReturnModel::BreachAction::ReturnPoint;
    plane.breachReturnAltitude = 40;
    evaluation = contingencies(plane);
    QCOMPARE(zoneIdentifiers(evaluation.contingencyZones), QStringList{ QStringLiteral("B") });
    QCOMPARE(evaluation.contingencyLabels.value(evaluation.contingencyZones.first()),
             QStringList{ QStringLiteral(u"a flight to the breach return point after a fence breach from items 1\u20133") });
    // Without the guided breach action only the returns home are checked
    plane.breachAction = GeoZoneReturnModel::BreachAction::Return;
    QVERIFY(contingencies(plane).contingencyZones.isEmpty());

    // The return to launch at the end is part of the planned flight, to the rally point closer than home
    masterController.rallyPointController()->addPoint(_offsetFrom(home, 1000, 1000));
    SimpleMissionItem *const returnItem = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(home, missionController->visualItems()->count()));
    QVERIFY(returnItem);
    returnItem->setCommand(MAV_CMD_NAV_RETURN_TO_LAUNCH);
    evaluation = contingencies(copter);
    QCOMPARE(zoneIdentifiers(evaluation.zones), QStringList{ QStringLiteral("X") });
    QCOMPARE(evaluation.pathLabels.value(evaluation.zones.first()), QStringList{ QStringLiteral("the return to rally point 1") });
}
#endif

GEOZONE_TEST(GeoZoneMissionPathTest)
