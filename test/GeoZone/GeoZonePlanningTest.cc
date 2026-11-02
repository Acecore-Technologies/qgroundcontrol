/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePlanningTest.h"
#include "GeoZoneBriefing.h"
#include "GeoZoneBriefingExport.h"
#include "GeoZoneFenceTags.h"
#include "GeoZoneMissionJumps.h"
#include "GeoZoneMissionWorker.h"
#include "GeoZonePlanAudit.h"
#include "GeoZoneSourceRepository.h"
#include "GeoZoneTestBase.h"
#include "GeoZoneTestHelpers.h"
#include "GeoFenceManager.h"
#include "MockLink.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "RallyPoint.h"
#include "RallyPointController.h"
#include "Vehicle.h"

#include <QtCore/QHash>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QStandardPaths>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPalette>

#include <algorithm>
#include <numeric>

using namespace GeoZoneTestHelpers;

GeoZonePlanningTest::GeoZonePlanningTest() = default;

GeoZonePlanningTest::~GeoZonePlanningTest() = default;

void GeoZonePlanningTest::initTestCase()
{
    _metricUnits = std::make_unique<ScopedMetricUnits>();
}

void GeoZonePlanningTest::cleanupTestCase()
{
    _metricUnits.reset();
}

void GeoZonePlanningTest::_testGuidedPathWarning()
{
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate square = center.atDistanceAndAzimuth(3000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("B"), QStringLiteral("CONDITIONAL"), { square.atDistanceAndAzimuth(282.8, 45), square.atDistanceAndAzimuth(282.8, 135),
                                                                              square.atDistanceAndAzimuth(282.8, 225), square.atDistanceAndAzimuth(282.8, 315) }),
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1000, 90), 200),
        // Unpublished restriction types can't be assumed harmless
        _circleFeature(QStringLiteral("X"), QStringLiteral("MYSTERY"), center.atDistanceAndAzimuth(2000, 90), 100),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(500, 90), 100),
    }), errorString), qPrintable(errorString));

    // Restricted zones and unknown restrictions are warned about, others are information
    const QList<GeoZone::Position> east = { GeoZone::Position{ center }, GeoZone::Position{ center.atDistanceAndAzimuth(2000, 90) },
                                            GeoZone::Position{ center.atDistanceAndAzimuth(4000, 90) }, GeoZone::Position{ center } };
    GeoZoneMonitor monitor(&manager);
    const QString warning = monitor.guidedCheck()->pathWarning(east);
    QVERIFY(warning.contains(QStringLiteral("A (Prohibited)")));
    QVERIFY(warning.contains(QStringLiteral("X (Unknown)")));
    QVERIFY(!warning.contains(QStringLiteral("N (")));
}

void GeoZonePlanningTest::_testApproximationReports()
{
    // East through a zone above a flight level, one with limits in an unknown reference, and one active from an unknown
    // daylight event to sunset, see GeoZoneEvaluatorTest::_testApproximations
    const QGeoCoordinate start(47, 8);
    QJsonObject scheduled = _circleFeature(QStringLiteral("SCHEDULED"), QStringLiteral("PROHIBITED"), start.atDistanceAndAzimuth(3000, 90), 200);
    scheduled[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "schedule", QJsonArray{ QJsonObject{
        { "day", QJsonArray{ "ANY" } }, { "startEvent", "MOONRISE" }, { "endEvent", "SS" } } } } } };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _withLimits(_circleFeature(QStringLiteral("LEVEL"), QStringLiteral("PROHIBITED"), start.atDistanceAndAzimuth(1000, 90), 200), 0, QStringLiteral("STD"), 3000, QStringLiteral("STD")),
        _withLimits(_circleFeature(QStringLiteral("UNKNOWN"), QStringLiteral("PROHIBITED"), start.atDistanceAndAzimuth(2000, 90), 200), 100, QStringLiteral("XYZ"), 200, QStringLiteral("XYZ")),
        scheduled,
    }), errorString), qPrintable(errorString));
    const auto zoneNamed = [&manager](const QString &identifier) {
        const auto it = std::find_if(manager.zoneList().cbegin(), manager.zoneList().cend(), [&identifier](const GeoZone *zone) { return zone->identifier() == identifier; });
        return (it == manager.zoneList().cend()) ? nullptr : *it;
    };

    // The briefing documents have them per zone and overall, in the JSON and with a note in the web page
    const QDateTime flightStart = _utc(QDate(2027, 6, 1), QTime(10, 0));
    const QDateTime flightEnd = flightStart.addSecs(600);
    GeoZonePathEvaluator::Path path = _evaluatorPath({ start, start.atDistanceAndAzimuth(4000, 90) }, { 500, 500 });
    path.setTimes(flightStart, flightEnd);
    GeoZoneBriefing::Input input;
    input.mission.name = QStringLiteral("Approximate");
    input.paths = { path };
    input.start = flightStart;
    input.end = flightEnd;
    const GeoZoneBriefing briefing = GeoZoneBriefing::build(manager.catalog(), input);
    const QJsonObject json = GeoZoneBriefingExport::toJson(briefing);
    QCOMPARE(json[QStringLiteral("approximations")].toArray().count(), 3);
    const QJsonObject levelJson = json[QStringLiteral("zones")].toArray()[0].toObject();
    QCOMPARE(levelJson[QStringLiteral("identifier")].toString(), QStringLiteral("LEVEL"));
    QCOMPARE(levelJson[QStringLiteral("approximations")].toArray(), QJsonArray({ QStringLiteral("flight_level") }));
    const QString html = GeoZoneBriefingExport::toHtml(briefing);
    QVERIFY(html.contains(QStringLiteral("Some results are approximate")));
    QVERIFY(html.contains(QStringLiteral("Flight level limits converted with the QNH setting")));

    // The mission check notes them in its warning
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    (void) missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(500, 270), 1);
    (void) missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(1500, 90), 2);
    QTRY_VERIFY(check.zones(GeoZoneMissionCheck::Crossed).contains(zoneNamed(QStringLiteral("LEVEL"))));
    QVERIFY(check.entry(zoneNamed(QStringLiteral("LEVEL")))->approximations.testFlag(GeoZonePathEvaluator::FlightLevel));
    QVERIFY2(check.warningText().contains(QStringLiteral("Approximate: ")), qPrintable(check.warningText()));
    QVERIFY(check.warningText().contains(QStringLiteral("Flight level limits converted with the QNH setting")));
}

void GeoZonePlanningTest::_testMissionPathTimes()
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

void GeoZonePlanningTest::_testFenceExcludes()
{
    const QGeoCoordinate center(47, 8);
    const auto local = [&center](double east, double north) {
        return center.atDistanceAndAzimuth(std::hypot(east, north), qRadiansToDegrees(std::atan2(east, north)));
    };
    ZoneOwner owner;
    const auto polygonZone = [&owner](const QList<QList<QGeoCoordinate>> &rings) {
        GeoZone::Data data;
        data.restriction = GeoZone::Prohibited;
        for (const QList<QGeoCoordinate> &ring : rings) {
            GeoZone::Volume volume;
            volume.rings = { GeoZone::toRing(ring) };
            data.volumes.append(volume);
        }
        return static_cast<const GeoZone*>(owner.add(data));
    };
    // A circle published as a polygon of 40 vertices, which simplifies, and a circle
    const GeoZone *const round = polygonZone({ _circleRing(center, 500, 40) });
    GeoZone::Volume circleVolume;
    circleVolume.isCircle = true;
    circleVolume.center = local(2000, 0);
    circleVolume.radius = 200;
    GeoZone::Data circleData;
    circleData.restriction = GeoZone::Prohibited;
    circleData.volumes = { circleVolume };
    const GeoZone *const circle = owner.add(circleData);

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);

    // Zones imported with or without simplification are excluded, the fences are those the importer makes
    for (const double tolerance : { 0.0, 25.0 }) {
        QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, round));
        QCOMPARE(importer.addExclusions({ round, circle }, tolerance), 2);
        const QList<QGeoCoordinate> fence = geoFenceController->polygons()->value<QGCFencePolygon*>(0)->coordinateList();
        QCOMPARE(fence, GeoZoneFences::zoneFences(round, tolerance).polygons.constFirst());
        QVERIFY((tolerance > 0) == (fence.count() < 40));
        QVERIFY(GeoZoneMissionCheck::fenceExcludes(geoFenceController, round));
        QVERIFY(GeoZoneMissionCheck::fenceExcludes(geoFenceController, circle));
        GeoZoneFenceTags::deleteFences(geoFenceController, false);
    }

    // Zones the importer has no fence for, like those without area, are not excluded whatever the fence
    geoFenceController->addInclusionCircle(local(-5000, 5000), local(5000, -5000));
    QCOMPARE(importer.addExclusions({ round }, 0), 1);
    const GeoZone *const line = polygonZone({ { local(-10, 0), local(10, 0) } });
    QVERIFY(GeoZoneFences::zoneFences(line).isEmpty());
    QCOMPARE(importer.addExclusions({ line }), 0);
    QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, line));
    QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, owner.add(GeoZone::Data())));
    // Nor are zones partly outside, like when one of their polygons isn't fenced
    const GeoZone *const twoParts = polygonZone({ _circleRing(center, 500, 40), _circleRing(local(0, 3000), 100, 8) });
    QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, twoParts));
    GeoZoneFenceTags::deleteFences(geoFenceController, false);

    // A fence around all vertices of a square zone whose notch cuts its north edge doesn't exclude it
    const GeoZone *const square = polygonZone({ { local(-100, 100), local(100, 100), local(100, -100), local(-100, -100) } });
    QGCFencePolygon *const notched = new QGCFencePolygon(false, geoFenceController);
    notched->appendVertices({ local(-200, -200), local(200, -200), local(200, 200), local(20, 200), local(20, 50), local(-20, 50), local(-20, 200), local(-200, 200) });
    geoFenceController->polygons()->append(notched);
    QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, square));
    notched->clear();
    notched->appendVertices({ local(-200, -200), local(200, -200), local(200, 200), local(-200, 200) });
    QVERIFY(GeoZoneMissionCheck::fenceExcludes(geoFenceController, square));
    GeoZoneFenceTags::deleteFences(geoFenceController, false);
}

void GeoZonePlanningTest::_testAntimeridianProfile()
{
    // A zone from 179.9 E to 179.9 W, see GeoZoneEvaluatorTest::_testAntimeridianQueries
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), { QGeoCoordinate(-16.6, 179.9), QGeoCoordinate(-16.4, 179.9),
                                                                             QGeoCoordinate(-16.4, -179.9), QGeoCoordinate(-16.6, -179.9) }),
    }), errorString), qPrintable(errorString));

    // The profile of a segment across the antimeridian passes through the zone
    const QGeoCoordinate from(-16.5, 179.8);
    const QGeoCoordinate to(-16.5, -179.8);
    const double entryDistance = from.distanceTo(QGeoCoordinate(-16.5, 179.9));
    GeoZoneMissionPath::Segment segment;
    segment.from = from;
    segment.to = to;
    segment.distance = from.distanceTo(to);
    const QList<GeoZoneProfile::Band> bands = GeoZoneProfile::computeBands({ segment }, zones.catalog());
    QCOMPARE(bands.count(), 1);
    QCOMPARE(bands[0].name, QStringLiteral("X"));
    QVERIFY(qAbs(bands[0].points.constFirst().distance - entryDistance) < 20);
    QVERIFY(qAbs(bands[0].points.constLast().distance - (segment.distance - entryDistance)) < 20);
}

void GeoZonePlanningTest::_testMissionCheck()
{
    // Before the temporary restriction of 2027
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(10, 0)));
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("C"), QStringLiteral("CONDITIONAL"), center.atDistanceAndAzimuth(2000, 0), 300),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(2000, 180), 300),
        // On the leg from 1000 m east to 2000 m north
        _circleFeature(QStringLiteral("U"), QStringLiteral("USPACE"), center.atDistanceAndAzimuth(1118, 26.57), 200),
    });
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));

    // Far above the planned altitudes
    QJsonObject high = _circleFeature(QStringLiteral("HIGH"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(500, 90), 300);
    QJsonArray geometry = high[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("lowerLimit")] = 5000;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
    layer[QStringLiteral("upperLimit")] = 6000;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AMSL");
    geometry[0] = layer;
    high[QStringLiteral("geometry")] = geometry;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("high.json"), QJsonArray{ high }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();

    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);

    // Passes the no restriction zone only, which is reported without a warning
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 180).atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 180).atDistanceAndAzimuth(1000, 90), 2);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Permitted).count(), 1);
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    QVERIFY(check.warningText().isEmpty());
    QVERIFY(check.zones(GeoZoneMissionCheck::USpace).isEmpty());
    QVERIFY(check.infoText().startsWith(QStringLiteral("Areas without restriction: N.")));

    // Through the prohibited zone and then the conditional one, listed most restrictive first
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 3);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 4);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 0), 5);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 2);
    QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed)[0]->identifier(), QStringLiteral("P"));
    QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed)[1]->identifier(), QStringLiteral("C"));
    QVERIFY(check.warningText().contains(QStringLiteral("P (Prohibited)")));
    // The path north also passes through the U-space airspace
    QCOMPARE(check.zones(GeoZoneMissionCheck::USpace).count(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("U-space airspace: U.")));
    QVERIFY(check.crossedZonesBoundingBox().contains(center));
    QVERIFY(check.crossedZonesBoundingBox().contains(center.atDistanceAndAzimuth(2000, 0)));

    // The fence must exclude the prohibited zones, circles and polygons added from them do
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    QVERIFY(geoFenceController->supported());
    check.setGeoFenceController(geoFenceController);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Unfenced).count(), 1);
    QVERIFY(check.fenceText().contains(QStringLiteral("P")));
    geoFenceController->addInclusionCircle(center.atDistanceAndAzimuth(1000, 315), center.atDistanceAndAzimuth(1000, 135));
    check.update();
    QCOMPARE(check.zones(GeoZoneMissionCheck::Unfenced).count(), 1);
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);
    QCOMPARE(importer.addZones(GeoZoneInfo::toVariantList(check.zones(GeoZoneMissionCheck::Unfenced))), 1);
    QTRY_VERIFY(check.zones(GeoZoneMissionCheck::Unfenced).isEmpty());
    QVERIFY(check.fenceText().isEmpty());
    GeoZoneFenceTags::deleteFences(geoFenceController, false);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Unfenced).count(), 1);
    // A polygon zone excluded by a simplified polygon fence
    const QList<QGeoCoordinate> squareZone = { center.atDistanceAndAzimuth(300, 315), center.atDistanceAndAzimuth(300, 45),
                                               center.atDistanceAndAzimuth(300, 135), center.atDistanceAndAzimuth(300, 225) };
    GeoZone::Data squareData;
    squareData.restriction = GeoZone::Prohibited;
    GeoZone::Volume squareVolume;
    squareVolume.rings = { GeoZone::toRing(squareZone) };
    squareData.volumes = { squareVolume };
    GeoZone square(squareData);
    QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, &square));
    QCOMPARE(importer.addExclusions({ &square }, 25), 1);
    QVERIFY(GeoZoneMissionCheck::fenceExcludes(geoFenceController, &square));
    GeoZoneFenceTags::deleteFences(geoFenceController, false);
    check.setGeoFenceController(nullptr);

    // A temporary restriction on the path counts at any time, or when it is active during the planned flight
    QJsonObject temporary = _circleFeature(QStringLiteral("T"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(700, 270), 100);
    temporary[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-01-01T10:00:00Z" }, { "endDateTime", "2027-01-01T12:00:00Z" } } };
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("temporary.json"), QJsonArray{ temporary }), errorString), qPrintable(errorString));
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 3);
    QCOMPARE(check.zones(GeoZoneMissionCheck::Inactive).count(), 0);
    manager.planning()->setPlannedStart(_utc(QDate(2027, 1, 1), QTime(8, 0)));
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 2);
    QCOMPARE(check.zones(GeoZoneMissionCheck::Inactive).count(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("1 zone(s) on the path are not active at the planned flight time.")));
    // The mission takes a few minutes, so a start just before the restriction overlaps it
    const QDateTime overlappingStart = _utc(QDate(2027, 1, 1), QTime(10, 0)).addSecs(-std::llround(missionController->missionTime() / 2));
    QVERIFY(missionController->missionTime() > 10);
    manager.planning()->setPlannedStart(overlappingStart);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 3);
    // A mission started now, like one on the vehicle, ignores the planned start
    const GeoZoneMonitor monitor(&manager);
    const QString startWarning = monitor.guidedCheck()->startWarning(missionController);
    QVERIFY2(startWarning.contains(QStringLiteral("P (Prohibited)")) && !startWarning.contains(QStringLiteral("T (Prohibited)")), qPrintable(startWarning));
    manager.planning()->clearPlannedStart();
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Inactive).count(), 0);
    QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 3);

    // A survey whose area covers a zone its flight lines miss
    const QGeoCoordinate surveyCenter = center.atDistanceAndAzimuth(5000, 180);
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("survey.json"),
        QJsonArray{ _circleFeature(QStringLiteral("S"), QStringLiteral("PROHIBITED"), surveyCenter.atDistanceAndAzimuth(395, 45), 2) }), errorString), qPrintable(errorString));
    const int surveyIndex = missionController->visualItems()->count();
    SurveyComplexItem *const survey = qobject_cast<SurveyComplexItem*>(missionController->insertComplexMissionItem(SurveyComplexItem::name, surveyCenter, surveyIndex));
    QVERIFY(survey);
    survey->surveyAreaPolygon()->clear();
    survey->surveyAreaPolygon()->appendVertices({ surveyCenter.atDistanceAndAzimuth(424, 315), surveyCenter.atDistanceAndAzimuth(424, 45),
                                                  surveyCenter.atDistanceAndAzimuth(424, 135), surveyCenter.atDistanceAndAzimuth(424, 225) });
    const auto surveyZoneCrossed = [&check]() {
        const QList<const GeoZone*> crossed = check.zones(GeoZoneMissionCheck::Crossed);
        return std::any_of(crossed.cbegin(), crossed.cend(), [](const GeoZone *zone) {
            return zone->identifier() == QStringLiteral("S");
        });
    };
    QTRY_VERIFY(surveyZoneCrossed());

    // Zones are released before the manager deletes them
    manager.repository()->clear();
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
}

void GeoZonePlanningTest::_testMissionControllerSwitch()
{
    const ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
    }), errorString), qPrintable(errorString));

    // A mission through the zone, and an empty one
    PlanMasterController crossing;
    crossing.setFlyView(false);
    crossing.start();
    (void) crossing.missionController()->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 1);
    (void) crossing.missionController()->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 2);
    QTRY_VERIFY(!GeoZoneMissionPath::fromMission(crossing.missionController()).paths().isEmpty());
    PlanMasterController empty;
    empty.setFlyView(false);
    empty.start();

    GeoZoneMissionCheck check(&manager);
    GeoZoneProfile profile(&manager);
    GeoZoneMissionWorker *const checkWorker = check.findChild<GeoZoneMissionWorker*>();
    GeoZoneMissionWorker *const profileWorker = profile.findChild<GeoZoneMissionWorker*>();
    QVERIFY(checkWorker && profileWorker);
    // Evaluations only start when triggered here
    checkWorker->setDelay(600000);
    profileWorker->setDelay(600000);
    check.setMissionController(crossing.missionController());
    profile.setMissionController(crossing.missionController());
    checkWorker->trigger();
    profileWorker->trigger();
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
    QTRY_COMPARE(profile.bands().count(), 1);

    // The results of the previous mission are cleared, and those of its evaluations in flight never applied
    checkWorker->trigger();
    profileWorker->trigger();
    QVERIFY(checkWorker->busy());
    QSignalSpy crossedSpy(&check, &GeoZoneMissionCheck::zonesChanged);
    QSignalSpy bandsSpy(&profile, &GeoZoneProfile::bandsChanged);
    check.setMissionController(empty.missionController());
    profile.setMissionController(empty.missionController());
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    QVERIFY(check.relevantZones().isEmpty());
    QVERIFY(profile.bands().isEmpty());
    QCOMPARE(crossedSpy.count(), 1);
    QCOMPARE(bandsSpy.count(), 1);
    QVERIFY(_settleWorkers());
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    QVERIFY(profile.bands().isEmpty());
    QCOMPARE(crossedSpy.count(), 1);
    QCOMPARE(bandsSpy.count(), 1);

    // Then the new mission is checked
    checkWorker->trigger();
    QVERIFY(checkWorker->busy());
    QTRY_VERIFY(!checkWorker->busy());
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    check.setMissionController(crossing.missionController());
    checkWorker->trigger();
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
}

void GeoZonePlanningTest::_testMissionCoverage()
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

/// Meters north and east of the coordinate
QGeoCoordinate offsetFrom(const QGeoCoordinate &coordinate, double north, double east)
{
    return coordinate.atDistanceAndAzimuth(std::hypot(north, east), qRadiansToDegrees(std::atan2(east, north)));
}

} // namespace

void GeoZonePlanningTest::_testMissionJumps()
{
    const ScopedEnvironment environment;
    // A loop north and east of home flown again by a DO_JUMP, then on east. J is on the jump back, away from the drawn path.
    const QGeoCoordinate home(47, 8);
    const QGeoCoordinate north = offsetFrom(home, 1000, 0);
    const QGeoCoordinate northEast = offsetFrom(home, 1000, 1000);
    const QGeoCoordinate east = offsetFrom(home, 0, 1000);
    const QGeoCoordinate farEast = offsetFrom(home, 0, 3000);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("J"), QStringLiteral("PROHIBITED"), offsetFrom(home, 500, 500), 50),
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
    QTRY_COMPARE(GeoZoneMissionPath::fromMission(missionController).segments().count(), 4);

    // The loop from the first item to the third and the jump back to the first are flown twice more
    const GeoZoneMissionPath path = GeoZoneMissionPath::fromMission(missionController);
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
    const GeoZoneMissionPath endless = GeoZoneMissionPath::fromMission(missionController);
    QVERIFY(endless.repeatsIndefinitely());
    QVERIFY(std::abs(endless.flightDistance() - (endless.totalDistance() + loop)) < 1);
    const auto endlessJump = std::find_if(endless.paths().cbegin(), endless.paths().cend(), [](const GeoZoneMissionPath::Path &flown) {
        return flown.label == QStringLiteral("the jump at item 4, repeated without end");
    });
    QVERIFY(endlessJump != endless.paths().cend());

    // Many repeats stop at the limit of the segments flown
    jumpItem->missionItem().setParam2(1000000);
    const GeoZoneMissionPath capped = GeoZoneMissionPath::fromMission(missionController);
    QVERIFY(capped.flightDistance() > (capped.totalDistance() + (100 * loop)));
    QVERIFY(capped.flightDistance() < (capped.totalDistance() + (((GeoZoneMissionPath::maxFlightSegments / 3) + 2) * loop)));

    // A jump past a return to launch: the mission goes on to the items after it, whose legs the mission controller
    // doesn't draw, and the vehicle doesn't return. S is on such a leg, R on the return the jump skips.
    const QGeoCoordinate skippedFrom = offsetFrom(home, 1000, -3000);
    const QGeoCoordinate afterReturn = offsetFrom(home, 1000, -1000);
    const QGeoCoordinate missionEnd = offsetFrom(home, 0, -1000);
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("skipped.json"), QJsonArray{
        _circleFeature(QStringLiteral("S"), QStringLiteral("PROHIBITED"), offsetFrom(home, 500, -1000), 50),
        _circleFeature(QStringLiteral("R"), QStringLiteral("PROHIBITED"), offsetFrom(home, 500, -1500), 50),
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
    QTRY_VERIFY(!GeoZoneMissionPath::fromMission(skipping).segments().isEmpty());
    const GeoZoneMissionPath skippedPath = GeoZoneMissionPath::fromMission(skipping);
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

void GeoZonePlanningTest::_testMissionJumpWalk()
{
    using Kind = GeoZoneMissionItem::Kind;
    // Home, then items 1 to 3 around a square, a DO_JUMP to item 1 and item 5
    const QGeoCoordinate home(47, 8);
    JumpMission mission;
    mission.addWaypoint(home);
    mission.addWaypoint(offsetFrom(home, 1000, 0));
    mission.addWaypoint(offsetFrom(home, 1000, 1000));
    mission.addWaypoint(offsetFrom(home, 0, 1000));
    mission.addCommand(Kind::Jump, 1, 2);
    mission.addWaypoint(offsetFrom(home, 0, 3000));
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
    patterns.addWaypoint(offsetFrom(home, 1000, 0));
    patterns.items[1].kind = Kind::Pattern;
    patterns.items[1].lastSequenceNumber = 9;
    patterns.items[1].exit.coordinate = offsetFrom(home, 1000, 500);
    patterns.itemSegments[1].segments = { static_cast<int>(patterns.segmentDistances.count()) };
    patterns.segmentDistances.append(500);
    patterns.addWaypoint(offsetFrom(home, 0, 500));
    patterns.items[2].sequenceNumber = patterns.items[2].lastSequenceNumber = 10;
    patterns.addCommand(Kind::Jump, 5, 1);
    patterns.items[3].sequenceNumber = patterns.items[3].lastSequenceNumber = 11;
    QCOMPARE(stepsText(patterns.walk()), QStringLiteral("0 1 2 2>1 1 2"));

    // The drawn flight path ends with the segment home before the first return to launch, which a jump skips here
    JumpMission skipped;
    skipped.addWaypoint(home);
    skipped.addWaypoint(offsetFrom(home, 1000, 0));
    skipped.addCommand(Kind::Jump, 4, 1);
    skipped.addCommand(Kind::ReturnToLaunch);
    skipped.addWaypoint(offsetFrom(home, 1000, 2000), false);
    skipped.addWaypoint(offsetFrom(home, 0, 2000), false);
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

void GeoZonePlanningTest::_testMissionContingencies()
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
        withAmslLimits(_circleFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), offsetFrom(home, 1000, 1000), 100), 440, 1000),
        _circleFeature(QStringLiteral("R"), QStringLiteral("PROHIBITED"), offsetFrom(home, 2000, 2500), 50),
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    (void) missionController->insertSimpleMissionItem(offsetFrom(home, 2000, 0), 1);
    (void) missionController->insertSimpleMissionItem(offsetFrom(home, 2000, 2000), 2);
    (void) missionController->insertSimpleMissionItem(offsetFrom(home, 0, 2000), 3);

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
    const GeoZoneMissionPath path = GeoZoneMissionPath::fromMission(missionController, true);
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
    rallyPointController->addPoint(offsetFrom(home, 2000, 3000));
    QTRY_COMPARE(zoneIdentifiers(check.zones(GeoZoneMissionCheck::Contingency)), QStringList{ QStringLiteral("R") });
    QVERIFY(check.entry(check.zones(GeoZoneMissionCheck::Contingency).first())->labels.first().startsWith(QStringLiteral("a return to rally point 1 assuming the PX4 default return altitude of 30 m from items")));
    // Moved away it isn't closer anymore
    rallyPointController->points()->value<RallyPoint*>(0)->setCoordinate(offsetFrom(home, 6000, 6000));
    QTRY_COMPARE(zoneIdentifiers(check.zones(GeoZoneMissionCheck::Contingency)), QStringList{ QStringLiteral("X") });

    // Zones are released before the manager deletes them
    manager.repository()->clear();
    QVERIFY(check.zones(GeoZoneMissionCheck::Contingency).isEmpty());
    QVERIFY(check.contingencyText().isEmpty());
}

void GeoZonePlanningTest::_testMissionReturnParameters()
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
        withAmslLimits(_circleFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), offsetFrom(home, 1000, 1000), 100), 470, 1000),
        _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), offsetFrom(home, 2000, -500), 50),
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    for (const QGeoCoordinate &coordinate : { offsetFrom(home, 2000, 0), offsetFrom(home, 2000, 2000), offsetFrom(home, 0, 2000) }) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
        QVERIFY(item);
        item->altitude()->setRawValue(30);
    }
    QTRY_COMPARE(GeoZoneMissionPath::fromMission(missionController).segments().count(), 3);
    const auto contingencies = [&manager, missionController](const GeoZoneReturnModel::Parameters &parameters) {
        return GeoZoneMissionCheck::evaluate(manager.catalog(), GeoZoneMissionPath::fromMission(missionController, parameters, true));
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
    masterController.geoFenceController()->setBreachReturnPoint(offsetFrom(home, 2000, -1000));
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
    masterController.rallyPointController()->addPoint(offsetFrom(home, 1000, 1000));
    SimpleMissionItem *const returnItem = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(home, missionController->visualItems()->count()));
    QVERIFY(returnItem);
    returnItem->setCommand(MAV_CMD_NAV_RETURN_TO_LAUNCH);
    evaluation = contingencies(copter);
    QCOMPARE(zoneIdentifiers(evaluation.zones), QStringList{ QStringLiteral("X") });
    QCOMPARE(evaluation.pathLabels.value(evaluation.zones.first()), QStringList{ QStringLiteral("the return to rally point 1") });
}

void GeoZonePlanningTest::_testAuthorisations()
{
    // Authorisations are for local days, the test stays within one
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(12, 0)));
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString storage = dir.filePath(QStringLiteral("storage"));
    GeoZoneManager manager(storage);
    QString errorString;
    QJsonObject controlZone = _circleFeature(QStringLiteral("CTR"), QStringLiteral("REQ_AUTHORISATION"), center, 300);
    controlZone[QStringLiteral("country")] = QStringLiteral("CHE");
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ controlZone }), errorString), qPrintable(errorString));
    GeoZone *const zone = manager.zoneList().constFirst();
    GeoZoneAuthorisations *const authorisations = manager.planning()->authorisations();
    const QVariant zoneInfo = QVariant::fromValue(GeoZoneInfo(zone));
    QCOMPARE(GeoZoneAuthorisations::zoneKey(zone), QStringLiteral("CHE/CTR"));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 2);
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);

    // Dates are local days, the last one included
    QVERIFY(!authorisations->authorise(zoneInfo, QString(), QStringLiteral("2026-13-40"), QString()).isEmpty());
    QVERIFY(!authorisations->authorise(zoneInfo, QString(), QStringLiteral("2026-11-02"), QStringLiteral("2026-11-01")).isEmpty());
    QCOMPARE(authorisations->count(), 0);

    // An authorisation without end is listed for information instead of warned about
    const QDate today = environment.now().toLocalTime().date();
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral(" P-123 "), QString(), QString()).isEmpty());
    QCOMPARE(authorisations->count(), 1);
    QCOMPARE(authorisations->authorisationOf(zoneInfo)[QStringLiteral("permit")].toString(), QStringLiteral("P-123"));
    QCOMPARE(authorisations->authorisationOf(zoneInfo)[QStringLiteral("statusText")].toString(), QStringLiteral("Valid"));
    QTRY_VERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    QCOMPARE(check.zones(GeoZoneMissionCheck::Authorised).count(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("Authorised: CTR.")));

    // Alerts and guided commands leave the zone out while authorised
    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 0);
    const QList<GeoZone::Position> path = { { center.atDistanceAndAzimuth(1000, 270) }, { center.atDistanceAndAzimuth(1000, 90) } };
    QVERIFY(monitor.guidedCheck()->pathWarning(path).isEmpty());

    // Expired authorisations don't count
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-122"), QString(), today.addDays(-1).toString(Qt::ISODate)).isEmpty());
    QCOMPARE(authorisations->count(), 1);
    QVERIFY(authorisations->authorisationOf(zoneInfo)[QStringLiteral("statusText")].toString().startsWith(QStringLiteral("Expired")));
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
    QVERIFY(!monitor.guidedCheck()->pathWarning(path).isEmpty());
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 1);

    // An authorisation from tomorrow covers a flight planned then, not one now
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-124"), today.addDays(1).toString(Qt::ISODate), today.addDays(3).toString(Qt::ISODate)).isEmpty());
    manager.planning()->setPlannedStart(QDateTime(today, QTime(12, 0)));
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
    manager.planning()->setPlannedStart(QDateTime(today.addDays(2), QTime(12, 0)));
    QTRY_VERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());

    // Plans entering the zone keep the planned start and the authorisation
    QJsonObject planJson;
    GeoZonePlanData::save(&masterController, planJson, &manager);
    const QJsonObject geoZones = planJson[QString(GeoZonePlanData::jsonKey)].toObject();
    QCOMPARE(QDateTime::fromString(geoZones[QStringLiteral("plannedStart")].toString(), Qt::ISODate), QDateTime(today.addDays(2), QTime(12, 0)).toUTC());
    QCOMPARE(geoZones[QStringLiteral("authorisations")].toArray().count(), 1);
    QCOMPARE(geoZones[QStringLiteral("authorisations")].toArray()[0].toObject()[QStringLiteral("permit")].toString(), QStringLiteral("P-124"));

    // Opening the plan offers its authorisations, which are only added when accepted
    authorisations->revoke(QStringLiteral("CHE/CTR"));
    manager.planning()->clearPlannedStart();
    QCOMPARE(authorisations->count(), 0);
    GeoZonePlanData::load(nullptr, planJson, &manager);
    QCOMPARE(manager.planning()->plannedStart(), QDateTime(today.addDays(2), QTime(12, 0)).toUTC());
    QCOMPARE(authorisations->count(), 0);
    QCOMPARE(authorisations->pendingAuthorisations().count(), 1);
    QCOMPARE(authorisations->pendingAuthorisations()[0].toMap()[QStringLiteral("permit")].toString(), QStringLiteral("P-124"));
    authorisations->dismissPending();
    QCOMPARE(authorisations->count(), 0);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());
    GeoZonePlanData::load(nullptr, planJson, &manager);
    authorisations->acceptPending();
    QCOMPARE(authorisations->count(), 1);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());
    // Authorisations already known aren't offered again
    GeoZonePlanData::load(nullptr, planJson, &manager);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());

    // Kept across restarts next to the stored sources
    GeoZoneManager restarted(storage);
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QCOMPARE(restarted.zoneCount(), 1);
    QCOMPARE(restarted.planning()->authorisations()->count(), 1);
    QVERIFY(restarted.planning()->authorisations()->find(restarted.zoneList().constFirst()));
    manager.planning()->clearPlannedStart();
}

void GeoZonePlanningTest::_testBriefingDocuments()
{
    // The briefing of GeoZoneBriefingTest::_testBriefing
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2027, 1, 1), QTime(8, 0)));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    BriefingMission mission(dir);
    QString errorString;
    QVERIFY2(mission.load(errorString), qPrintable(errorString));
    const GeoZone *const ctr = mission.zones.zoneList().constFirst();
    const GeoZoneBriefing briefing = mission.build();

    // JSON with its schema and version
    const QJsonObject json = GeoZoneBriefingExport::toJson(briefing);
    QCOMPARE(json[QStringLiteral("schema")].toString(), QStringLiteral("qgc-geozone-briefing"));
    QCOMPARE(json[QStringLiteral("version")].toInt(), 1);
    QCOMPARE(json[QStringLiteral("mission")].toObject()[QStringLiteral("name")].toString(), QStringLiteral("Survey"));
    QCOMPARE(json[QStringLiteral("mission")].toObject()[QStringLiteral("plannedStart")].toString(), QStringLiteral("2027-01-01T10:30:00Z"));
    const QJsonArray zones = json[QStringLiteral("zones")].toArray();
    QCOMPARE(zones.count(), 4);
    const QJsonObject ctrJson = zones[0].toObject();
    QCOMPARE(ctrJson[QStringLiteral("involvement")].toString(), QStringLiteral("entered"));
    QCOMPARE(ctrJson[QStringLiteral("restriction")].toString(), QStringLiteral("req_authorisation"));
    QCOMPARE(ctrJson[QStringLiteral("crossings")].toArray()[0].toObject()[QStringLiteral("entryItem")].toInt(), 1);
    QVERIFY(ctrJson[QStringLiteral("crossings")].toArray()[0].toObject()[QStringLiteral("zoneFloor")].isNull());
    QCOMPARE(ctrJson[QStringLiteral("activePeriods")].toArray()[0].toObject()[QStringLiteral("start")].toString(), QStringLiteral("2027-01-01T10:45:00Z"));
    QCOMPARE(ctrJson[QStringLiteral("authorisation")].toObject()[QStringLiteral("permit")].toString(), QStringLiteral("P-77"));
    QCOMPARE(ctrJson[QStringLiteral("contentHash")].toString(), QString::fromLatin1(ctr->data().contentHash.toHex()));
    QVERIFY(ctrJson[QStringLiteral("distance")].isNull());
    QCOMPARE(zones[2].toObject()[QStringLiteral("involvement")].toString(), QStringLiteral("contingency"));
    QCOMPARE(zones[3].toObject()[QStringLiteral("involvement")].toString(), QStringLiteral("nearby"));
    QVERIFY(zones[3].toObject()[QStringLiteral("distance")].isDouble());
    const QJsonObject sourceJson = json[QStringLiteral("sources")].toArray()[0].toObject();
    QCOMPARE(sourceJson[QStringLiteral("contentHash")].toString(), QString::fromLatin1(briefing.sources[0].contentHash.toHex()));
    QVERIFY(sourceJson.contains(QStringLiteral("airacCycle")));

    // The web page escapes the text of the zones
    const QString html = GeoZoneBriefingExport::toHtml(briefing);
    QVERIFY(html.startsWith(QStringLiteral("<!DOCTYPE html>")));
    QVERIFY(html.contains(QStringLiteral("name=\"viewport\"")));
    QVERIFY(!html.contains(QStringLiteral("<script")));
    QVERIFY(html.contains(QStringLiteral("CTR &lt;script&gt;alert(1)&lt;/script&gt;")));
    for (const QString &text : { QStringLiteral("Skyguide"), QStringLiteral("P-77"), QStringLiteral("Call the tower"), QStringLiteral("Prison"), QStringLiteral("NEAR"),
                                 QStringLiteral("RET"), QString::fromLatin1(briefing.sources[0].contentHash.toHex()) }) {
        QVERIFY2(html.contains(text), qPrintable(text));
    }
    QVERIFY(!html.contains(QStringLiteral("FAR")));
    // Counts without "(s)" plurals, English has no translation choosing the forms
    QVERIFY2(html.contains(QStringLiteral("Entered by the flight path: 2.<br>Of them restricted, active during the flight and not authorised: 1.<br>"
                                          "Entered only by returns: 1.<br>Restricted within 500 m: 1.")), qPrintable(html));
    QVERIFY(!html.contains(QStringLiteral("(s)")));

    // A4 PDF
    const QString pdfFile = dir.filePath(QStringLiteral("briefing.pdf"));
    QVERIFY2(GeoZoneBriefingExport::write(briefing, pdfFile, GeoZoneBriefingExport::Format::Pdf, errorString), qPrintable(errorString));
    QFile pdf(pdfFile);
    QVERIFY(pdf.open(QIODevice::ReadOnly));
    const QByteArray content = pdf.readAll();
    QVERIFY(content.startsWith("%PDF-"));
    QVERIFY(content.trimmed().endsWith("%%EOF"));
    QVERIFY(content.contains("/Type /Page"));
}

namespace {

/// Glyphs of the fonts of a PDF document written by Qt that the ToUnicode maps of the fonts don't map back to letters, so
/// that text extraction loses them or gives ligatures like U+FB01 instead of "fi". Qt numbers the glyphs of each font in
/// the order the document uses them, so the glyphs without characters are the gaps below the highest one mapped.
/// @param fontCount Fonts with a ToUnicode map
QStringList unmappedPdfGlyphs(const QByteArray &pdf, int &fontCount)
{
    static const QRegularExpression rangeExpression(QStringLiteral(R"(<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>\s*(\[[^\]]*\]|<[0-9A-Fa-f]+>))"));
    static const QRegularExpression charExpression(QStringLiteral(R"(<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>)"));
    static const QRegularExpression codeExpression(QStringLiteral(R"(<([0-9A-Fa-f]+)>)"));
    const auto sections = [](const QString &cmap, const QString &begin, const QString &end) {
        QStringList result;
        qsizetype from = 0;
        while ((from = cmap.indexOf(begin, from)) >= 0) {
            const qsizetype to = cmap.indexOf(end, from);
            if (to < 0) {
                break;
            }
            result.append(cmap.mid(from + begin.size(), to - from - begin.size()));
            from = to;
        }
        return result;
    };

    QStringList unmapped;
    fontCount = 0;
    qsizetype from = 0;
    while ((from = pdf.indexOf("begincmap", from)) >= 0) {
        const qsizetype to = pdf.indexOf("endcmap", from);
        if (to < 0) {
            break;
        }
        const QString cmap = QString::fromLatin1(pdf.mid(from, to - from));
        from = to;
        // Characters of the glyphs, 0 for none
        QHash<uint, uint> characters;
        for (const QString &section : sections(cmap, QStringLiteral("beginbfrange"), QStringLiteral("endbfrange"))) {
            for (const QRegularExpressionMatch &match : rangeExpression.globalMatch(section)) {
                const uint first = match.captured(1).toUInt(nullptr, 16);
                const uint last = match.captured(2).toUInt(nullptr, 16);
                const QString target = match.captured(3);
                if (target.startsWith(QLatin1Char('['))) {
                    uint code = first;
                    for (const QRegularExpressionMatch &character : codeExpression.globalMatch(target)) {
                        characters.insert(code++, character.captured(1).toUInt(nullptr, 16));
                    }
                } else {
                    const uint character = codeExpression.match(target).captured(1).toUInt(nullptr, 16);
                    for (uint code = first; code <= last; code++) {
                        characters.insert(code, (character == 0) ? 0 : (character + code - first));
                    }
                }
            }
        }
        for (const QString &section : sections(cmap, QStringLiteral("beginbfchar"), QStringLiteral("endbfchar"))) {
            for (const QRegularExpressionMatch &match : charExpression.globalMatch(section)) {
                characters.insert(match.captured(1).toUInt(nullptr, 16), match.captured(2).toUInt(nullptr, 16));
            }
        }
        uint highest = 0;
        for (auto it = characters.cbegin(); it != characters.cend(); ++it) {
            highest = (it.value() != 0) ? (std::max)(highest, it.key()) : highest;
        }
        for (uint code = 1; code <= highest; code++) {
            const uint character = characters.value(code);
            // Latin ligatures of the Alphabetic Presentation Forms
            const bool ligature = (character >= 0xFB00) && (character <= 0xFB06);
            if ((character == 0) || ligature) {
                unmapped.append(QStringLiteral("font %1 glyph %2 to U+%3").arg(fontCount).arg(code).arg(character, 4, 16, QLatin1Char('0')));
            }
        }
        fontCount++;
    }
    return unmapped;
}

} // namespace

void GeoZonePlanningTest::_testBriefingPrint()
{
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2027, 1, 1), QTime(8, 0)));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    BriefingMission mission(dir);
    QString errorString;
    QVERIFY2(mission.load(errorString), qPrintable(errorString));
    const GeoZoneBriefing briefing = mission.build();

    // The application palette follows the color theme of the desktop, the document stays dark on white with a dark one.
    // Its text can be searched and copied, also in fonts with ligatures like the application's Open Sans.
    const QPalette applicationPalette = QGuiApplication::palette();
    const QFont applicationFont = QGuiApplication::font();
    const auto restore = qScopeGuard([&applicationPalette, &applicationFont]() {
        QGuiApplication::setPalette(applicationPalette);
        QGuiApplication::setFont(applicationFont);
    });
    QGuiApplication::setFont(QFont(QStringLiteral("Open Sans")));
    const auto writePdf = [&briefing, &dir](const QColor &text, const QColor &window) {
        QGuiApplication::setPalette(QPalette(text, window, window.lighter(), window.darker(), window.darker(120), text, Qt::red, window, window));
        const QString fileName = dir.filePath(QStringLiteral("briefing-%1.pdf").arg(text.name().mid(1)));
        QString errorString;
        return GeoZoneBriefingExport::writePdf(briefing, fileName, errorString) ? fileName : QString();
    };
    const QString pdfFile = writePdf(Qt::white, QColor(0x30, 0x30, 0x30));
    QVERIFY(!pdfFile.isEmpty());
    QFile pdf(pdfFile);
    QVERIFY(pdf.open(QIODevice::ReadOnly));
    int fontCount = 0;
    const QStringList unmapped = unmappedPdfGlyphs(pdf.readAll(), fontCount);
    QVERIFY(fontCount > 0);
    QVERIFY2(unmapped.isEmpty(), qPrintable(unmapped.join(QStringLiteral(", "))));

    // With the text extraction and rendering of Poppler when installed
    const QString pdftotext = QStandardPaths::findExecutable(QStringLiteral("pdftotext"));
    if (!pdftotext.isEmpty()) {
        QProcess process;
        process.start(pdftotext, { QStringLiteral("-enc"), QStringLiteral("UTF-8"), pdfFile, QStringLiteral("-") });
        QVERIFY(process.waitForFinished(30000));
        const QString text = QString::fromUtf8(process.readAllStandardOutput());
        for (const QString &word : { QStringLiteral("Geozone briefing"), QStringLiteral("official"), QStringLiteral("flight path") }) {
            QVERIFY2(text.contains(word), qPrintable(word));
        }
    }
    const QString pdftoppm = QStandardPaths::findExecutable(QStringLiteral("pdftoppm"));
    if (!pdftoppm.isEmpty()) {
        // The first page at 100 dpi
        const auto firstPage = [&pdftoppm](const QString &fileName) {
            QProcess process;
            process.start(pdftoppm, { QStringLiteral("-r"), QStringLiteral("100"), QStringLiteral("-l"), QStringLiteral("1"), QStringLiteral("-singlefile"),
                                      QStringLiteral("-png"), fileName, fileName });
            return process.waitForFinished(30000) ? QImage(fileName + QStringLiteral(".png")).convertToFormat(QImage::Format_RGB32) : QImage();
        };
        const QImage onDarkTheme = firstPage(pdfFile);
        QVERIFY(!onDarkTheme.isNull());
        QCOMPARE(onDarkTheme, firstPage(writePdf(Qt::black, Qt::white)));
        QCOMPARE(onDarkTheme.pixel(0, onDarkTheme.height() - 1), qRgb(255, 255, 255));
        // The text of the headings and tables is dark gray to black, the notes gray
        qsizetype darkPixels = 0;
        for (int y = 0; y < onDarkTheme.height(); y++) {
            const QRgb *const line = reinterpret_cast<const QRgb*>(onDarkTheme.constScanLine(y));
            for (int x = 0; x < onDarkTheme.width(); x++) {
                const int red = qRed(line[x]);
                const int green = qGreen(line[x]);
                const int blue = qBlue(line[x]);
                const bool neutral = ((std::max)({ red, green, blue }) - (std::min)({ red, green, blue })) < 16;
                if (neutral && (qGray(line[x]) < 64)) {
                    darkPixels++;
                }
            }
        }
        QVERIFY2(darkPixels > 5000, qPrintable(QString::number(darkPixels)));
    }
}

void GeoZonePlanningTest::_testBriefingExport()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1000, 90), 200),
        _circleFeature(QStringLiteral("B"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(2500, 90), 200),
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(500, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(3500, 90), 2);

    // The flight path is updated after the items
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 2);

    GeoZoneBriefingExport exporter(&manager);
    exporter.setMissionController(missionController);
    QCOMPARE(exporter.defaultFileName(), QStringLiteral("geozone-briefing.pdf"));
    QVERIFY(!exporter.defaultFolder().isEmpty());

    // The zones the mission enters after its first item
    const QString jsonFile = dir.filePath(QStringLiteral("mission.json"));
    QVERIFY2(exporter.exportBriefing(jsonFile), qPrintable(exporter.errorString()));
    QFile file(jsonFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(json[QStringLiteral("version")].toInt(), GeoZoneBriefingExport::jsonVersion);
    const QJsonObject mission = json[QStringLiteral("mission")].toObject();
    QCOMPARE(mission[QStringLiteral("itemCount")].toInt(), 2);
    QVERIFY(mission[QStringLiteral("distanceMeters")].toDouble() > 3900);
    const QStringList returnAssumptions = mission[QStringLiteral("returnAssumptions")].toVariant().toStringList();
    QCOMPARE(returnAssumptions.value(0), QStringLiteral("Returns modeled on PX4 with its default parameters, as the vehicle's are unknown."));
    QCOMPARE(returnAssumptions.value(1), QStringLiteral("Return altitude: 30 m above the destination, or the current altitude when higher."));
    QVERIFY2(returnAssumptions.constLast().startsWith(QStringLiteral("Returns checked from positions along the mission: ")), qPrintable(returnAssumptions.constLast()));
    QVERIFY(mission[QStringLiteral("plannedStart")].isNull());
    QStringList identifiers;
    for (const QJsonValue &zone : json[QStringLiteral("zones")].toArray()) {
        if (zone.toObject()[QStringLiteral("involvement")].toString() == QStringLiteral("entered")) {
            identifiers.append(zone.toObject()[QStringLiteral("identifier")].toString());
            QCOMPARE(zone.toObject()[QStringLiteral("crossings")].toArray()[0].toObject()[QStringLiteral("entryItem")].toInt(), 1);
        }
    }
    QCOMPARE(identifiers, (QStringList{ QStringLiteral("A"), QStringLiteral("B") }));
    QCOMPARE(json[QStringLiteral("sources")].toArray().count(), 1);

    // PDF without an extension, errors reported
    QVERIFY2(exporter.exportBriefing(dir.filePath(QStringLiteral("briefing"))), qPrintable(exporter.errorString()));
    QVERIFY(QFileInfo(dir.filePath(QStringLiteral("briefing.pdf"))).size() > 0);
    QVERIFY2(exporter.exportBriefing(dir.filePath(QStringLiteral("briefing.html"))), qPrintable(exporter.errorString()));
    QVERIFY(!exporter.exportBriefing(dir.filePath(QStringLiteral("missing/briefing.json"))));
    QVERIFY(!exporter.errorString().isEmpty());
}

void GeoZonePlanningTest::_testPlanAudit()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    const QJsonObject zoneA = _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1000, 90), 200);
    const QJsonObject zoneB = _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2500, 90), 200);
    const QJsonObject zoneD = _circleFeature(QStringLiteral("D"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2000, 0), 200);
    const auto load = [&](const QJsonArray &features) {
        QString errorString;
        return manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString);
    };
    QVERIFY(load({ zoneA, zoneB, zoneD }));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(500, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(3500, 90), 2);
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    const auto relevant = [&check]() {
        QStringList identifiers;
        for (const GeoZone *zone : check.relevantZones()) {
            identifiers.append(zone->identifier());
        }
        identifiers.sort();
        return identifiers;
    };
    QTRY_COMPARE(relevant(), (QStringList{ QStringLiteral("A"), QStringLiteral("B") }));

    // Saved with the hashes of the zones the mission enters and the version of the source
    QJsonObject planJson;
    GeoZonePlanData::save(&masterController, planJson, &manager);
    const QJsonObject audit = planJson[QString(GeoZonePlanData::jsonKey)].toObject()[QStringLiteral("audit")].toObject();
    QCOMPARE(audit[QStringLiteral("version")].toInt(), GeoZonePlanAudit::version);
    QCOMPARE(audit[QStringLiteral("zones")].toArray().count(), 2);
    QVERIFY(!audit[QStringLiteral("zones")].toArray()[0].toObject()[QStringLiteral("contentHash")].toString().isEmpty());
    QCOMPARE(audit[QStringLiteral("sources")].toArray().count(), 1);
    QVERIFY(manager.planning()->planAudit().has_value());
    check.update();
    QVERIFY(check.auditText().isEmpty());

    // Reopened with the same data
    manager.planning()->setPlanAudit(std::nullopt);
    GeoZonePlanData::load(nullptr, planJson, &manager);
    QVERIFY(manager.planning()->planAudit().has_value());
    check.update();
    QVERIFY(check.auditText().isEmpty());

    // Editing the mission into a zone published before isn't a change of the data
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 0), 3);
    QTRY_VERIFY(relevant().contains(QStringLiteral("D")));
    QVERIFY(check.auditText().isEmpty());
    missionController->removeVisualItem(3);
    QTRY_COMPARE(relevant(), (QStringList{ QStringLiteral("A"), QStringLiteral("B") }));

    // A zone whose geometry changed
    const QJsonObject largerA = _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1000, 90), 250);
    QVERIFY(load({ largerA, zoneB, zoneD }));
    check.update();
    QVERIFY2(check.auditText().contains(QStringLiteral("1 zone(s) modified")), qPrintable(check.auditText()));
    QVERIFY(check.auditText().contains(QStringLiteral("1 source(s) updated")));
    QVERIFY2(check.auditDetails().contains(QStringLiteral("Modified: A (Authorisation required)")), qPrintable(check.auditDetails().join(QChar(u'\n'))));

    // A zone removed and a new one on the path
    const QJsonObject zoneE = _circleFeature(QStringLiteral("E"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1800, 90), 100);
    QVERIFY(load({ largerA, zoneD, zoneE }));
    check.update();
    QVERIFY(check.auditText().contains(QStringLiteral("1 new")));
    QVERIFY(check.auditText().contains(QStringLiteral("1 removed")));
    QVERIFY(check.auditDetails().contains(QStringLiteral("Removed: B (Prohibited)")));
    QVERIFY(check.auditDetails().contains(QStringLiteral("New: E (Prohibited)")));

    // Acknowledged until the plan is opened again, the file keeps the data it was saved with
    check.acknowledgeAudit();
    check.update();
    QVERIFY(check.auditText().isEmpty());
    GeoZonePlanData::load(nullptr, planJson, &manager);
    check.update();
    QVERIFY(!check.auditText().isEmpty());

    // Saving again records the current data
    QJsonObject resaved;
    GeoZonePlanData::save(&masterController, resaved, &manager);
    check.update();
    QVERIFY(check.auditText().isEmpty());
    GeoZonePlanData::load(nullptr, resaved, &manager);
    check.update();
    QVERIFY(check.auditText().isEmpty());

    // Plans without the audit load silently
    QJsonObject legacy = planJson;
    QJsonObject legacyZones = legacy[QString(GeoZonePlanData::jsonKey)].toObject();
    legacyZones.remove(QStringLiteral("audit"));
    legacy[QString(GeoZonePlanData::jsonKey)] = legacyZones;
    GeoZonePlanData::load(nullptr, legacy, &manager);
    QVERIFY(!manager.planning()->planAudit().has_value());
    check.update();
    QVERIFY(check.auditText().isEmpty());
    GeoZonePlanData::load(nullptr, planJson, &manager);
    GeoZonePlanData::load(nullptr, QJsonObject(), &manager);
    QVERIFY(!manager.planning()->planAudit().has_value());
}

void GeoZonePlanningTest::_testPlanSave()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/zones", server.body);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    GeoZoneSourceRepository *const repository = manager.repository();
    repository->setFollowArea(true);

    // A service downloading its zones for the area of interest, around the vehicle for now
    repository->setVehiclePosition(QGeoCoordinate(47.0, 8.0));
    const QString url = server.url(QStringLiteral("/zones?bbox={bbox}"));
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    manager.addService(url, GeoZoneService().toVariantMap(), QString());
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(manager.repository()->source(url)->followsArea());

    // A mission away from the vehicle, whose zones the service would download for a check following it
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(QGeoCoordinate(47.5, 8.5), 1);
    (void) missionController->insertSimpleMissionItem(QGeoCoordinate(47.52, 8.5), 2);
    QTRY_VERIFY(!GeoZoneMissionPath::fromMission(missionController).paths().isEmpty());

    // Saving neither sets the area of interest nor downloads, and records the audit
    const QGeoRectangle area = repository->areaOfInterest();
    const int requests = server.requestCount;
    QJsonObject planJson;
    GeoZonePlanData::save(&masterController, planJson, &manager);
    QVERIFY(!manager.repository()->busy());
    QCOMPARE(repository->areaOfInterest(), area);
    QTest::qWait(200);
    QCOMPARE(server.requestCount, requests);
    const std::optional<GeoZonePlanAudit::Record> saved =
        GeoZonePlanAudit::Record::fromJson(planJson[QString(GeoZonePlanData::jsonKey)].toObject()[QStringLiteral("audit")].toObject());
    QVERIFY(saved.has_value());
    QCOMPARE(manager.planning()->planAudit(), saved);

    // The Plan View's check follows the mission, the plan is saved with its result once settled, the same
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QVERIFY(!check.settled());
    QVERIFY(!GeoZoneMissionCheck::settledCheck(&manager, missionController));
    QTRY_VERIFY(!manager.repository()->busy() && check.settled());
    QCOMPARE(GeoZoneMissionCheck::settledCheck(&manager, missionController), &check);
    QVERIFY(repository->areaOfInterest().contains(QGeoCoordinate(47.51, 8.5)));
    QJsonObject resaved;
    GeoZonePlanData::save(&masterController, resaved, &manager);
    QCOMPARE(resaved[QString(GeoZonePlanData::jsonKey)].toObject()[QStringLiteral("audit")].toObject(), check.auditRecord().toJson());
    // While a change waits to be checked, the plan is evaluated for saving, with the result of the check
    check.setUpdateDelay(600000);
    (void) missionController->insertSimpleMissionItem(QGeoCoordinate(47.54, 8.5), 3);
    QTRY_VERIFY(!check.settled());
    QVERIFY(!GeoZoneMissionCheck::settledCheck(&manager, missionController));
    QJsonObject edited;
    GeoZonePlanData::save(&masterController, edited, &manager);
    QVERIFY(!manager.repository()->busy());
    check.update();
    QVERIFY(check.settled());
    QCOMPARE(edited[QString(GeoZonePlanData::jsonKey)].toObject()[QStringLiteral("audit")].toObject(), check.auditRecord().toJson());
    QTRY_VERIFY(!manager.repository()->busy());
    manager.repository()->clear();
}

void GeoZonePlanningTest::_testUploadReasons()
{
    const ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    const QJsonObject prohibited = _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 200);
    const QJsonObject authorisation = _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(600, 90), 200);
    const auto load = [&](const QJsonArray &features) {
        QString errorString;
        return manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString);
    };
    QVERIFY(load({ prohibited, authorisation }));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 2);
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    check.setGeoFenceController(masterController.geoFenceController());
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 2);

    // The prohibited zone the fence doesn't exclude, not the zone requiring an authorisation
    QStringList reasons = check.uploadReasons();
    QCOMPARE(reasons.count(), 1);
    QVERIFY2(reasons[0].startsWith(QStringLiteral("The mission path enters prohibited geozones: P.")), qPrintable(reasons[0]));
    QVERIFY(reasons[0].contains(QStringLiteral("doesn't exclude 1 of them")));
    // Nor once authorised
    GeoZone *const zone = manager.zoneList().constFirst()->identifier() == QStringLiteral("P") ? manager.zoneList().constFirst() : manager.zoneList().constLast();
    QCOMPARE(zone->identifier(), QStringLiteral("P"));
    QVERIFY(manager.planning()->authorisations()->authorise(QVariant::fromValue(GeoZoneInfo(zone)), QStringLiteral("P-1"), QString(), QString()).isEmpty());
    QTRY_VERIFY(check.uploadReasons().isEmpty());
    QCOMPARE(check.zones(GeoZoneMissionCheck::Authorised).count(), 1);

    // Geozone data changed since the plan was saved, until acknowledged
    QJsonObject planJson;
    GeoZonePlanData::save(&masterController, planJson, &manager);
    QVERIFY(load({ prohibited, _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(600, 90), 250) }));
    QTRY_VERIFY(!check.auditText().isEmpty());
    reasons = check.uploadReasons();
    QCOMPARE(reasons.count(), 1);
    QVERIFY2(reasons[0].startsWith(check.auditText()), qPrintable(reasons[0]));
    QVERIFY2(reasons[0].contains(QStringLiteral("Modified: A (Authorisation required)")), qPrintable(reasons[0]));
    check.acknowledgeAudit();
    QTRY_VERIFY(check.uploadReasons().isEmpty());
    manager.repository()->clear();
}

void GeoZonePlanningTest::_testPlannedStartPerPlan()
{
    // Plan files are loaded and saved with the application's manager
    GeoZonePlanning *const planning = GeoZoneManager::instance()->planning();
    const QDateTime start = _utc(QDate(2027, 1, 1), QTime(10, 0));
    const GeoZonePlanAudit::Record record = GeoZonePlanAudit::Record::make({}, {}, start);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString withStart = dir.filePath(QStringLiteral("with-start.plan"));
    const QString withoutStart = dir.filePath(QStringLiteral("without-start.plan"));
    {
        PlanMasterController masterController;
        masterController.setFlyView(false);
        masterController.start();
        (void) masterController.missionController()->insertSimpleMissionItem(QGeoCoordinate(47, 8), 1);
        planning->setPlannedStart(start);
        masterController.saveToFile(withStart);
        planning->clearPlannedStart();
        masterController.saveToFile(withoutStart);
    }

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();

    // A plan without a planned start doesn't keep the one of the plan before
    masterController.loadFromFile(withStart);
    QCOMPARE(planning->plannedStart(), start);
    masterController.loadFromFile(withoutStart);
    QVERIFY(!planning->plannedStart().isValid());

    // A new plan has neither the planned start nor the audit record of the previous one
    masterController.loadFromFile(withStart);
    QCOMPARE(planning->plannedStart(), start);
    planning->setPlanAudit(record);
    masterController.removeAll();
    QVERIFY(!planning->plannedStart().isValid());
    QVERIFY(!planning->planAudit().has_value());
    // Nor is the start of a new plan kept when items are added and removed
    planning->setPlannedStart(start);
    (void) masterController.missionController()->insertSimpleMissionItem(QGeoCoordinate(47, 8), 1);
    masterController.missionController()->removeVisualItem(1);
    QCOMPARE(planning->plannedStart(), start);

    // The Fly View follows the mission on the vehicle, which isn't the plan
    PlanMasterController flyController;
    flyController.start();
    flyController.removeAll();
    QCOMPARE(planning->plannedStart(), start);

    // Nor does a plan downloaded from the vehicle
    _connectMockLink(MAV_AUTOPILOT_PX4);
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->initialPlanRequestComplete(), 30000);
    QTRY_COMPARE(masterController.managerVehicle(), vehicle);
    QTRY_VERIFY(!masterController.syncInProgress());
    planning->setPlannedStart(start);
    planning->setPlanAudit(record);
    masterController.loadFromVehicle();
    QTRY_VERIFY_WITH_TIMEOUT(!planning->plannedStart().isValid(), 15000);
    QVERIFY(!planning->planAudit().has_value());
    QTRY_VERIFY(!masterController.syncInProgress());
    _disconnectMockLink();
}

void GeoZonePlanningTest::_testRoutePlanner()
{
    ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate west = center.atDistanceAndAzimuth(2000, 270);
    const QGeoCoordinate east = center.atDistanceAndAzimuth(2000, 90);
    QJsonObject raised = _circleFeature(QStringLiteral("R"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1200, 90), 200);
    QJsonArray geometry = raised[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = 30;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AGL");
    geometry[0] = layer;
    raised[QStringLiteral("geometry")] = geometry;
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"),
        QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300), raised }), errorString), qPrintable(errorString));
    const GeoZone *const prohibited = manager.searchZones(QStringLiteral("P")).isEmpty() ? nullptr : GeoZoneInfo::fromVariant(manager.searchZones(QStringLiteral("P")).first());
    QVERIFY(prohibited && (prohibited->identifier() == QStringLiteral("P")));

    // Around the zone, keeping the margin
    const std::optional<QList<QGeoCoordinate>> detour = GeoZoneRoutePlanner::detourAround(west, east, { prohibited });
    QVERIFY(detour && !detour->isEmpty());
    QList<QGeoCoordinate> path = { west };
    path.append(*detour);
    path.append(east);
    QVERIFY(!prohibited->intersectsPath(path));
    for (qsizetype i = 1; i < path.count(); i++) {
        for (int step = 0; step <= 10; step++) {
            const QGeoCoordinate point = path[i - 1].atDistanceAndAzimuth(path[i - 1].distanceTo(path[i]) * step / 10, path[i - 1].azimuthTo(path[i]));
            QVERIFY2(prohibited->distanceTo(point) > (GeoZoneRoutePlanner::detourMargin / 2), qPrintable(QString::number(prohibited->distanceTo(point))));
        }
    }
    // Not needed, or impossible from inside
    QVERIFY(GeoZoneRoutePlanner::detourAround(west, center.atDistanceAndAzimuth(2000, 0), { prohibited })->isEmpty());
    QVERIFY(!GeoZoneRoutePlanner::detourAround(center.atDistanceAndAzimuth(250, 0), east, { prohibited }));
    // From within the margin, leaving the zone no closer than the start
    const QGeoCoordinate near = center.atDistanceAndAzimuth(350, 0);
    const std::optional<QList<QGeoCoordinate>> fromNear = GeoZoneRoutePlanner::detourAround(near, east, { prohibited });
    QVERIFY(fromNear && !fromNear->isEmpty());
    QList<QGeoCoordinate> nearPath = { near };
    nearPath.append(*fromNear);
    nearPath.append(east);
    QVERIFY(_pathClearance(nearPath, { prohibited }) > 40);
    QVERIFY(_pathClearance(nearPath.mid(1), { prohibited }) > (GeoZoneRoutePlanner::detourMargin - 1));

    // Suggestions for a mission through both zones at 50 m
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    for (const QGeoCoordinate &coordinate : { west, east }) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
        QVERIFY(item);
        item->altitude()->setRawValue(50);
    }
    GeoZoneRoutePlanner planner(&manager);
    planner.setMissionController(missionController);
    planner.update();
    // Without the altitudes of the leg, which need the home altitude, only a detour avoids the zones, and the dialog tells
    // why the other ways don't
    QStringList texts;
    int detourIndex = -1;
    for (int i = 0; i < planner.suggestions().count(); i++) {
        const QVariantMap suggestion = planner.suggestions()[i].toMap();
        texts.append(suggestion[QStringLiteral("text")].toString());
        texts.append(suggestion[QStringLiteral("notes")].toStringList());
        if (suggestion[QStringLiteral("canApply")].toBool()) {
            detourIndex = i;
            QCOMPARE(suggestion[QStringLiteral("kind")].toString(), QStringLiteral("detour"));
            QVERIFY(suggestion[QStringLiteral("recommended")].toBool());
            QVERIFY(suggestion[QStringLiteral("first")].toBool());
            QCOMPARE(suggestion[QStringLiteral("legText")].toString(), QStringLiteral("Leg to item 2"));
        }
    }
    QCOMPARE(planner.suggestions().count(), 1);
    QVERIFY2(detourIndex == 0, qPrintable(texts.join(QStringLiteral(" | "))));
    QVERIFY(texts[0].contains(QStringLiteral("detour around P, R")));
    QVERIFY2(texts.contains(QStringLiteral("No climb: the altitudes of the leg aren't known.")), qPrintable(texts.join(QStringLiteral(" | "))));
    QVERIFY2(texts.contains(QStringLiteral("No descent: the altitudes of the leg aren't known.")), qPrintable(texts.join(QStringLiteral(" | "))));

    // Inserting the detour leaves the zones
    const int itemCount = missionController->visualItems()->count();
    QVERIFY(planner.applySuggestion(detourIndex));
    QVERIFY(missionController->visualItems()->count() > itemCount);
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_VERIFY_WITH_TIMEOUT(check.zones(GeoZoneMissionCheck::Crossed).isEmpty(), 5000);
    QTRY_VERIFY(!planner.busy());
    QVERIFY(planner.suggestions().isEmpty());
    QVERIFY(!planner.applySuggestion(0));

    // Zones on either side of a leg are passed between instead of around both
    ZoneOwner owner;
    const auto circleZone = [&owner](const QGeoCoordinate &zoneCenter, double radius) {
        GeoZone::Volume volume;
        volume.isCircle = true;
        volume.center = zoneCenter;
        volume.radius = radius;
        GeoZone::Data data;
        data.restriction = GeoZone::Prohibited;
        data.volumes.append(volume);
        return static_cast<const GeoZone*>(owner.add(data));
    };
    const QList<const GeoZone*> sides = { circleZone(center.atDistanceAndAzimuth(1000, 270).atDistanceAndAzimuth(300, 0), 400),
                                          circleZone(center.atDistanceAndAzimuth(1000, 90).atDistanceAndAzimuth(300, 180), 400) };
    const std::optional<QList<QGeoCoordinate>> weave = GeoZoneRoutePlanner::detourAround(west, east, sides);
    QVERIFY(weave && !weave->isEmpty());
    QList<QGeoCoordinate> weavePath = { west };
    weavePath.append(*weave);
    weavePath.append(east);
    for (qsizetype i = 1; i < weavePath.count(); i++) {
        QVERIFY2(weavePath[i].distanceTo(QGeoCoordinate(center.latitude(), weavePath[i].longitude())) < 400, qPrintable(QString::number(i)));
        for (int step = 0; step <= 20; step++) {
            const QGeoCoordinate point = weavePath[i - 1].atDistanceAndAzimuth(weavePath[i - 1].distanceTo(weavePath[i]) * step / 20, weavePath[i - 1].azimuthTo(weavePath[i]));
            for (const GeoZone *side : sides) {
                QVERIFY(side->distanceTo(point) > (GeoZoneRoutePlanner::detourMargin / 2));
            }
        }
    }

    // A leg through a zone above the sea can be lowered below its floor, where the terrain is known whether or not
    // the terrain tiles are received
    environment.setTerrain(0.0);
    const QGeoCoordinate seaCenter(0.5, -25);
    const auto seaZone = [&seaCenter](const QString &identifier, double floor) {
        QJsonObject feature = _circleFeature(identifier, QStringLiteral("REQ_AUTHORISATION"), seaCenter, 200);
        QJsonArray seaGeometry = feature[QStringLiteral("geometry")].toArray();
        QJsonObject seaLayer = seaGeometry[0].toObject();
        seaLayer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
        seaLayer[QStringLiteral("lowerLimit")] = floor;
        seaLayer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
        seaGeometry[0] = seaLayer;
        feature[QStringLiteral("geometry")] = seaGeometry;
        return feature;
    };
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("sea.json"), QJsonArray{ seaZone(QStringLiteral("S"), 40) }), errorString), qPrintable(errorString));
    PlanMasterController lowerController;
    lowerController.setFlyView(false);
    lowerController.start();
    MissionController *const lowerMission = lowerController.missionController();
    const QGeoCoordinate legStart = seaCenter.atDistanceAndAzimuth(800, 270);
    lowerMission->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(legStart.latitude(), legStart.longitude(), 0));
    QList<SimpleMissionItem*> legItems;
    for (const QGeoCoordinate &coordinate : { legStart, seaCenter.atDistanceAndAzimuth(800, 90) }) {
        legItems.append(qobject_cast<SimpleMissionItem*>(lowerMission->insertSimpleMissionItem(coordinate, lowerMission->visualItems()->count())));
        legItems.last()->altitude()->setRawValue(50);
    }
    GeoZoneRoutePlanner lowerPlanner(&manager);
    lowerPlanner.setMissionController(lowerMission);
    const auto floorSuggestion = [&lowerPlanner]() {
        lowerPlanner.update();
        for (int i = 0; i < lowerPlanner.suggestions().count(); i++) {
            const QVariantMap suggestion = lowerPlanner.suggestions()[i].toMap();
            if (suggestion[QStringLiteral("canApply")].toBool() && (suggestion[QStringLiteral("applyText")].toString() == QStringLiteral("Apply"))) {
                return i;
            }
        }
        return -1;
    };
    const auto suggestionTexts = [&lowerPlanner]() {
        QStringList texts;
        for (const QVariant &suggestion : lowerPlanner.suggestions()) {
            texts.append(suggestion.toMap()[QStringLiteral("text")].toString());
            texts.append(suggestion.toMap()[QStringLiteral("notes")].toStringList());
        }
        return texts.join(QStringLiteral(" | "));
    };
    QTRY_VERIFY2(floorSuggestion() >= 0, qPrintable(suggestionTexts()));
    QVERIFY2(suggestionTexts().contains(QStringLiteral("Leg to item 2: descend to 30 m above home under S, lowering 2 waypoint(s) by up to 20 m.")),
             qPrintable(suggestionTexts()));
    QVERIFY(lowerPlanner.applySuggestion(floorSuggestion()));
    QCOMPARE(legItems[0]->altitude()->rawValue().toDouble(), 30.0);
    QCOMPARE(legItems[1]->altitude()->rawValue().toDouble(), 30.0);
    QTRY_VERIFY2((lowerPlanner.update(), lowerPlanner.suggestions().isEmpty()), qPrintable(suggestionTexts()));

    // Not when the lowered leg would come too close to the sea
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("sea_low.json"), QJsonArray{ seaZone(QStringLiteral("T"), 15) }), errorString), qPrintable(errorString));
    QTRY_VERIFY2((lowerPlanner.update(), suggestionTexts().contains(QStringLiteral("No descent: flying under T leaves 5 m above the terrain, less than 10 m."))),
                 qPrintable(suggestionTexts()));
    QCOMPARE(floorSuggestion(), -1);
}

void GeoZonePlanningTest::_testRoutePlannerWorker()
{
    ScopedEnvironment environment;
    environment.setTerrain(0.0);
    // Zones between and around the waypoints of a mission
    const QList<QGeoCoordinate> waypoints = { QGeoCoordinate(47.0, 8.0), QGeoCoordinate(47.05, 8.06), QGeoCoordinate(47.0, 8.12), QGeoCoordinate(47.06, 8.16) };
    QRandomGenerator random(20261102);
    QJsonArray features;
    for (int i = 0; features.count() < 80; i++) {
        const QGeoCoordinate zoneCenter(46.97 + (random.generateDouble() * 0.12), 7.97 + (random.generateDouble() * 0.22));
        const double radius = 150 + random.bounded(500);
        if (std::any_of(waypoints.cbegin(), waypoints.cend(), [&](const QGeoCoordinate &waypoint) { return waypoint.distanceTo(zoneCenter) < (radius + 300); })) {
            continue;
        }
        QList<QGeoCoordinate> ring;
        const int vertexCount = 5 + random.bounded(10);
        for (int vertex = 0; vertex < vertexCount; vertex++) {
            ring.append(zoneCenter.atDistanceAndAzimuth(radius * (0.5 + (0.5 * random.generateDouble())), vertex * 360.0 / vertexCount));
        }
        QJsonObject feature = (i % 2) ? _circleFeature(QStringLiteral("Z%1").arg(i), QStringLiteral("PROHIBITED"), zoneCenter, radius)
                                      : _polygonFeature(QStringLiteral("Z%1").arg(i), QStringLiteral("REQ_AUTHORISATION"), ring);
        if (i % 3 == 0) {
            feature = _withLimits(feature, 0, QStringLiteral("AGL"), 80, QStringLiteral("AGL"));
        }
        features.append(feature);
    }
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(47.0, 8.0, 0));
    for (const QGeoCoordinate &coordinate : waypoints) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count()));
        QVERIFY(item);
        item->altitude()->setRawValue(40);
    }
    QTRY_VERIFY(!qIsNaN(GeoZoneMissionPath::fromMission(missionController).legs().constLast().segment.toAltitudeAMSL));

    GeoZoneRoutePlanner planner(&manager);
    planner.setMissionController(missionController);
    planner.update();
    const QVariantList synchronous = planner.suggestions();
    QVERIFY(synchronous.count() >= 3);
    QVERIFY(!planner.busy());

    // On a worker thread, with the same result
    QSignalSpy busySpy(&planner, &GeoZoneRoutePlanner::busyChanged);
    planner.startUpdate();
    QVERIFY(planner.busy());
    QVERIFY(planner.suggestions().isEmpty());
    QTRY_VERIFY(!planner.busy());
    QCOMPARE(busySpy.count(), 2);
    QCOMPARE(planner.suggestions(), synchronous);
    QCOMPARE(planner.progress(), 1.0);

    // A new search replaces the one in progress
    QSignalSpy suggestionsSpy(&planner, &GeoZoneRoutePlanner::suggestionsChanged);
    planner.startUpdate();
    planner.startUpdate();
    QTRY_VERIFY(!planner.busy());
    QCOMPARE(planner.suggestions(), synchronous);
    QVERIFY(_settleWorkers());
    QCOMPARE(suggestionsSpy.count(), 2);

    // Cancelled, nothing comes later
    planner.startUpdate();
    planner.cancel();
    QVERIFY(!planner.busy());
    QVERIFY(planner.suggestions().isEmpty());
    QVERIFY(planner.statusText().contains(QStringLiteral("cancelled")));
    QVERIFY(_settleWorkers());
    QVERIFY(planner.suggestions().isEmpty());

    // While active, the suggestions follow the settings
    planner.setActive(true);
    QTRY_VERIFY(!planner.busy());
    QVERIFY(planner.statusText().isEmpty());
    QCOMPARE(planner.suggestions(), synchronous);
    planner.setActive(false);
}

void GeoZonePlanningTest::_testProfileBands()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const auto withLimits = [](QJsonObject feature, double lower, const QString &lowerReference, double upper, const QString &upperReference, const QString &units) {
        QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
        QJsonObject layer = geometry[0].toObject();
        layer[QStringLiteral("lowerLimit")] = lower;
        layer[QStringLiteral("lowerVerticalReference")] = lowerReference;
        layer[QStringLiteral("upperLimit")] = upper;
        layer[QStringLiteral("upperVerticalReference")] = upperReference;
        layer[QStringLiteral("uomDimensions")] = units;
        geometry[0] = layer;
        feature[QStringLiteral("geometry")] = geometry;
        return feature;
    };
    // From the ground to 120 m above it in the middle, 1000 to 2000 ft AMSL over the east end, a zone without
    // restriction and one away from the path
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        withLimits(_circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300), 0, QStringLiteral("AGL"), 120, QStringLiteral("AGL"), QStringLiteral("M")),
        // ED-269 circle radii are in the layer units too, 200 m
        withLimits(_circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1000, 90), 200 / 0.3048), 1000, QStringLiteral("AMSL"), 2000, QStringLiteral("AMSL"), QStringLiteral("FT")),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center, 500),
        _circleFeature(QStringLiteral("F"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 0), 300),
        _circleFeature(QStringLiteral("END"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(179000, 90), 300),
    });
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));

    // West to east over flat ground at 400 m, then a pattern without a flight path, then east again without terrain
    GeoZoneMissionPath::Segment west;
    west.from = center.atDistanceAndAzimuth(1000, 270);
    west.to = center;
    west.distance = 1000;
    west.terrainSpacing = 100;
    west.terrainAMSL = QList<double>(11, 400);
    GeoZoneMissionPath::Segment east = west;
    east.from = center;
    east.to = center.atDistanceAndAzimuth(1000, 90);
    GeoZoneMissionPath::Segment gap;
    gap.distance = 250;
    GeoZoneMissionPath::Segment beyond;
    beyond.from = center.atDistanceAndAzimuth(1000, 90);
    beyond.to = center.atDistanceAndAzimuth(1300, 90);
    beyond.distance = 300;

    const QVariantList bands = GeoZoneProfile::bandsForSegments({ west, east, gap, beyond }, manager.catalog());
    // Bands with unbounded limits compare equal, so recomputing unchanged bands doesn't repaint
    QCOMPARE(GeoZoneProfile::computeBands({ west, east, gap, beyond }, manager.catalog()), GeoZoneProfile::computeBands({ west, east, gap, beyond }, manager.catalog()));
    QStringList names;
    for (const QVariant &band : bands) {
        names.append(band.toMap()[QStringLiteral("name")].toString());
    }
    // Most restrictive last, then along the path. The gap splits the stretch through A.
    QCOMPARE(names, QStringList({ QStringLiteral("A"), QStringLiteral("A"), QStringLiteral("P") }));

    const auto points = [&bands](int index) { return bands[index].toMap()[QStringLiteral("points")].toList(); };
    const auto value = [](const QVariant &point, const char *key) { return point.toMap()[QString::fromLatin1(key)].toDouble(); };

    // Continuous across the segments through the middle, from the ground to 120 m above it
    const QVariantList prohibited = points(2);
    QVERIFY(qAbs(value(prohibited.first(), "distance") - 700) <= GeoZoneProfile::minSampleSpacing);
    QVERIFY(qAbs(value(prohibited.last(), "distance") - 1300) <= GeoZoneProfile::minSampleSpacing);
    QVERIFY(qIsNaN(value(prohibited.first(), "floor")));
    QCOMPARE(value(prohibited.first(), "ceiling"), 520.0);

    const QVariantList authorisation = points(0);
    QVERIFY(qAbs(value(authorisation.first(), "distance") - 1800) <= GeoZoneProfile::minSampleSpacing);
    QCOMPARE(value(authorisation.last(), "distance"), 2000.0);
    QCOMPARE(value(authorisation.first(), "floor"), 1000 * 0.3048);
    QCOMPARE(value(authorisation.first(), "ceiling"), 2000 * 0.3048);
    // After the gap, beyond the end of the segment through its center
    const QVariantList afterGap = points(1);
    QCOMPARE(value(afterGap.first(), "distance"), 2250.0);
    QVERIFY(qAbs(value(afterGap.last(), "distance") - 2450) <= GeoZoneProfile::minSampleSpacing);

    // Limits above unknown terrain are unknown
    const QVariantList unknownTerrain = GeoZoneProfile::bandsForSegments({ beyond }, manager.catalog());
    QCOMPARE(unknownTerrain.count(), 1);
    beyond.from = center.atDistanceAndAzimuth(100, 270);
    beyond.to = center.atDistanceAndAzimuth(100, 90);
    const QVariantList noTerrain = GeoZoneProfile::bandsForSegments({ beyond }, manager.catalog());
    QCOMPARE(noTerrain.count(), 1);
    QVERIFY(qIsNaN(value(noTerrain[0].toMap()[QStringLiteral("points")].toList().first(), "ceiling")));

    // Long paths are sampled more sparsely up to their end
    QList<GeoZoneMissionPath::Segment> longPath;
    for (int i = 0; i < 60; i++) {
        GeoZoneMissionPath::Segment leg;
        leg.from = center.atDistanceAndAzimuth(i * 3000, 90);
        leg.to = center.atDistanceAndAzimuth((i + 1) * 3000, 90);
        leg.distance = 3000;
        longPath.append(leg);
    }
    const QVariantList longBands = GeoZoneProfile::bandsForSegments(longPath, manager.catalog());
    const auto end = std::find_if(longBands.cbegin(), longBands.cend(), [](const QVariant &band) {
        return band.toMap()[QStringLiteral("name")].toString() == QStringLiteral("END");
    });
    QVERIFY(end != longBands.cend());
    const QVariantList endPoints = end->toMap()[QStringLiteral("points")].toList();
    const double longSpacing = 180000.0 / GeoZoneProfile::maxSamples;
    QVERIFY(qAbs(value(endPoints.first(), "distance") - 178700) <= longSpacing + 1);
    QVERIFY(qAbs(value(endPoints.last(), "distance") - 179300) <= longSpacing + 1);
}

void GeoZonePlanningTest::_testGeoFenceImport()
{
    const GeoZoneParser::Result ed269 = _parseResource(QStringLiteral("ed269_test.json"));
    const GeoZoneParser::Result ed318 = _parseResource(QStringLiteral("ed318_test.json"));
    QCOMPARE(ed269.zones.count(), 3);
    QCOMPARE(ed318.zones.count(), 3);

    ZoneOwner zoneParent;
    const QList<const GeoZone*> zones = {
        zoneParent.add(ed269.zones[0]),   // One polygon after merging identical footprints
        zoneParent.add(ed269.zones[1]),   // Circle
        zoneParent.add(ed318.zones[1]),   // Two polygons
    };

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    geoFenceController->setDirty(false);
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);

    QCOMPARE(importer.addExclusions(zones), 4);
    QCOMPARE(geoFenceController->polygons()->count(), 3);
    QCOMPARE(geoFenceController->circles()->count(), 1);
    QVERIFY(geoFenceController->dirty());

    const QGCFencePolygon *const polygon = geoFenceController->polygons()->value<QGCFencePolygon*>(0);
    QVERIFY(!polygon->inclusion());
    QCOMPARE(polygon->count(), 4);

    QGCFenceCircle *const circle = geoFenceController->circles()->value<QGCFenceCircle*>(0);
    QVERIFY(!circle->inclusion());
    QCOMPARE(circle->center(), QGeoCoordinate(47.5, 8.5));
    QCOMPARE(circle->radius()->rawValue().toDouble(), 1000 * 0.3048);

    // Fences are named after their zones and marked as added from geozones
    const auto zoneName = [](const GeoZone *zone) { return zone->name().isEmpty() ? zone->identifier() : zone->name(); };
    QCOMPARE(GeoZoneFenceTag::find(polygon)->name(), zoneName(zones[0]));
    QVERIFY(GeoZoneFenceTag::isGeoZone(polygon));
    QCOMPARE(GeoZoneFenceTag::find(circle)->name(), zoneName(zones[1]));
    QVERIFY(GeoZoneFenceTag::isGeoZone(circle));

    // Which the plan file keeps in the entries of the fences, while drawn fences have no tags there
    geoFenceController->addInclusionCircle(QGeoCoordinate(47.1, 7.9), QGeoCoordinate(46.9, 8.1));
    (void) GeoZoneFenceTag::of(geoFenceController->circles()->get(1));
    const QJsonObject planJson = masterController.saveToJson().object();
    const QJsonObject fenceJson = planJson[PlanMasterController::kJsonGeoFenceObjectKey].toObject();
    const QJsonObject polygonJson = fenceJson[QStringLiteral("polygons")].toArray()[0].toObject();
    QCOMPARE(polygonJson[QStringLiteral("name")].toString(), zoneName(zones[0]));
    QVERIFY(polygonJson[QStringLiteral("geoZone")].toBool());
    const QJsonArray circlesJson = fenceJson[QStringLiteral("circles")].toArray();
    QCOMPARE(circlesJson.count(), 2);
    QCOMPARE(circlesJson[0].toObject()[QStringLiteral("name")].toString(), zoneName(zones[1]));
    QVERIFY(circlesJson[0].toObject()[QStringLiteral("geoZone")].toBool());
    QVERIFY(!circlesJson[1].toObject().contains(QStringLiteral("name")) && !circlesJson[1].toObject().contains(QStringLiteral("geoZone")));
    geoFenceController->deleteCircle(1);

    // Loading the plan restores the tags and leaves the fence clean, while fences without them load untagged
    PlanMasterController loadedController;
    loadedController.setFlyView(false);
    loadedController.start();
    GeoFenceController *const loadedFence = loadedController.geoFenceController();
    QString errorString;
    QVERIFY2(loadedFence->load(fenceJson, errorString), qPrintable(errorString));
    loadedFence->setDirty(false);
    GeoZonePlanData::load(&loadedController, planJson);
    QCOMPARE(loadedFence->polygons()->count(), 3);
    QCOMPARE(GeoZoneFenceTag::find(loadedFence->polygons()->get(0))->name(), zoneName(zones[0]));
    QVERIFY(GeoZoneFenceTag::isGeoZone(loadedFence->circles()->get(0)));
    QVERIFY(!GeoZoneFenceTag::find(loadedFence->circles()->get(1)));
    QVERIFY(!loadedFence->dirty());
    QJsonObject untaggedPolygon = polygonJson;
    untaggedPolygon.remove(QStringLiteral("name"));
    untaggedPolygon.remove(QStringLiteral("geoZone"));
    QJsonArray untaggedPolygons = fenceJson[QStringLiteral("polygons")].toArray();
    untaggedPolygons[0] = untaggedPolygon;
    QJsonObject untaggedFence = fenceJson;
    untaggedFence[QStringLiteral("polygons")] = untaggedPolygons;
    QJsonObject untaggedPlan = planJson;
    untaggedPlan[PlanMasterController::kJsonGeoFenceObjectKey] = untaggedFence;
    QVERIFY2(loadedFence->load(untaggedFence, errorString), qPrintable(errorString));
    GeoZonePlanData::load(&loadedController, untaggedPlan);
    QVERIFY(!GeoZoneFenceTag::find(loadedFence->polygons()->get(0)));
    QVERIFY(GeoZoneFenceTag::isGeoZone(loadedFence->polygons()->get(1)));

    // Importing the same zones again does not duplicate fences
    QCOMPARE(importer.addExclusions(zones), 0);
    QCOMPARE(geoFenceController->polygons()->count(), 3);
    QCOMPARE(geoFenceController->circles()->count(), 1);

    // Deleting the fences from geozones keeps the drawn ones, in one reset per list
    geoFenceController->setDirty(false);
    geoFenceController->addInclusionCircle(QGeoCoordinate(47.1, 7.9), QGeoCoordinate(46.9, 8.1));
    QCOMPARE(geoFenceController->circles()->count(), 2);
    QSignalSpy resetSpy(geoFenceController->polygons(), &QAbstractItemModel::modelReset);
    GeoZoneFenceTags::deleteFences(geoFenceController, true);
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(geoFenceController->polygons()->count(), 0);
    QCOMPARE(geoFenceController->circles()->count(), 1);
    QVERIFY(!GeoZoneFenceTag::isGeoZone(geoFenceController->circles()->get(0)));
    QVERIFY(geoFenceController->dirty());
    GeoZoneFenceTags::deleteFences(geoFenceController, false);
    QCOMPARE(geoFenceController->circles()->count(), 0);
}

void GeoZonePlanningTest::_testGeoFenceSelection()
{
    // Two circular zones published as 40 vertex polygons, and a circle
    ZoneOwner zoneParent;
    QList<const GeoZone*> zones;
    for (int i = 0; i < 2; i++) {
        GeoZone::Volume volume;
        volume.rings.append(GeoZone::toRing(_circleRing(QGeoCoordinate(47, 8 + (i * 0.1)), 500, 40)));
        GeoZone::Data data;
        data.identifier = QStringLiteral("Z%1").arg(i);
        data.restriction = GeoZone::Prohibited;
        data.volumes.append(volume);
        zones.append(zoneParent.add(data));
    }
    GeoZone::Volume circleVolume;
    circleVolume.isCircle = true;
    circleVolume.center = QGeoCoordinate(47, 8.2);
    circleVolume.radius = 300;
    GeoZone::Data circleData;
    circleData.identifier = QStringLiteral("C");
    circleData.restriction = GeoZone::Prohibited;
    circleData.volumes.append(circleVolume);
    zones.append(zoneParent.add(circleData));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);
    QVERIFY(!importer.upload()->canUpload());

    // Without a vehicle the fence fits PX4 and ArduPilot copters, with the circle as a polygon. 80 polygon points don't.
    QSignalSpy selectionSpy(&importer, &GeoZoneFenceImporter::selectionChanged);
    importer.select(zones);
    QCOMPARE(selectionSpy.count(), 1);
    QCOMPARE(importer.capacity().maxItems, GeoZoneFences::Capacity::px4MaxItems);
    QVERIFY(!importer.capacity().circles);
    QCOMPARE(importer.selectedCount(), 2);
    QVariantMap capacity = importer.capacityInfo();
    QVERIFY(capacity[QStringLiteral("estimated")].toBool());
    QVERIFY(capacity[QStringLiteral("text")].toString().startsWith(QStringLiteral("Vehicle fence: %1 of 64 points").arg(capacity[QStringLiteral("usedItems")].toInt())));
    QVERIFY(capacity[QStringLiteral("note")].toString().contains(QStringLiteral("No vehicle")));
    QList<GeoZoneFenceChoiceModel::Row> choices = importer.choices()->rows();
    QCOMPARE(choices.count(), 3);
    QVERIFY(choices[0].selected);
    QVERIFY(!choices[2].selected);
    QCOMPARE(choices[2].reason, QStringLiteral("No room left in the vehicle fence"));

    // Simplified, all of them fit
    importer.select(zones, 25);
    QCOMPARE(importer.selectedCount(), 3);
    QVERIFY(importer.capacity().fits(importer.selection().usage));

    // Drawn fences and the breach return point are kept and take room, fences added from geozones before are replaced
    geoFenceController->addInclusionCircle(QGeoCoordinate(47.1, 7.9), QGeoCoordinate(46.9, 8.3));
    geoFenceController->setBreachReturnPoint(QGeoCoordinate(47, 7.95));
    QCOMPARE(importer.addExclusions({ zones[0] }), 1);
    QCOMPARE(GeoZoneFenceImporter::keptUsage(geoFenceController).items, 2);
    importer.select(zones, 25);
    QCOMPARE(importer.capacityInfo()[QStringLiteral("keptItems")].toInt(), 2);
    QCOMPARE(importer.applySelection(), 3);
    QCOMPARE(geoFenceController->polygons()->count(), 3);
    QCOMPARE(geoFenceController->circles()->count(), 1);
    QCOMPARE(importer.applySelection(), 3);
    QCOMPARE(geoFenceController->polygons()->count(), 3);
    int points = 2;
    for (int i = 0; i < geoFenceController->polygons()->count(); i++) {
        QVERIFY(GeoZoneFenceTag::isGeoZone(geoFenceController->polygons()->get(i)));
        points += geoFenceController->polygons()->value<QGCFencePolygon*>(i)->count();
    }
    QCOMPARE(points, importer.selection().usage.items);
    // The mission check finds the zones excluded by the fences of the selection
    for (const GeoZone *zone : std::as_const(zones)) {
        QVERIFY(GeoZoneMissionCheck::fenceExcludes(geoFenceController, zone));
    }

    // Zones left out by the user
    importer.setZoneIncluded(zones[0]->key(), false);
    QCOMPARE(importer.selectedCount(), 2);
    choices = importer.choices()->rows();
    const auto leftOut = std::find_if(choices.cbegin(), choices.cend(), [&zones](const GeoZoneFenceChoiceModel::Row &row) { return row.key == zones[0]->key(); });
    QVERIFY(leftOut != choices.cend());
    QVERIFY(!leftOut->selected);
    QVERIFY(!leftOut->included);
    QCOMPARE(leftOut->reason, QStringLiteral("Left out by you"));
    QVERIFY(leftOut->includable);
    importer.setZoneIncluded(zones[0]->key(), true);
    QCOMPARE(importer.selectedCount(), 3);

    // Zones of the manager are prepared on the thread pool, and the lists change in place when zones are left out
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager *const manager = GeoZoneManager::instance();
    const auto clearZones = qScopeGuard([manager]() { manager->repository()->clear(); });
    QString errorString;
    QVERIFY2(manager->repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47, 8), 300),
        _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47, 8.1), 300),
        _circleFeature(QStringLiteral("FAR"), QStringLiteral("PROHIBITED"), QGeoCoordinate(50, 15), 300),
    }), errorString), qPrintable(errorString));
    QTRY_COMPARE(manager->zoneCount(), 3);
    importer.updateSelection({ GeoZone::Prohibited }, QGeoRectangle(QGeoCoordinate(47.1, 7.9), QGeoCoordinate(46.9, 8.2)), -1, 25);
    QVERIFY(importer.busy());
    QTRY_VERIFY(!importer.busy());
    QCOMPARE(importer.selectedCount(), 2);
    QCOMPARE(importer.leftOutCount(), 0);
    QSignalSpy resetSpy(importer.choices(), &QAbstractItemModel::modelReset);
    QSignalSpy changedSpy(importer.choices(), &QAbstractItemModel::dataChanged);
    const QString first = importer.choices()->rows().constFirst().key;
    importer.setZoneIncluded(first, false);
    QCOMPARE(importer.selectedCount(), 1);
    QCOMPARE(importer.choices()->rows().constFirst().key, first);
    QVERIFY(!importer.choices()->rows().constFirst().selected);
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(resetSpy.count(), 0);
    // Applying while the zones are prepared applies the selection of the new zones, leaving out those left out before
    importer.updateSelection({ GeoZone::Prohibited }, QGeoRectangle(), -1, 25);
    QVERIFY(importer.busy());
    QCOMPARE(importer.applySelection(), 2);
    QVERIFY(!importer.busy());
    QCOMPARE(importer.leftOutCount(), 1);
}

void GeoZonePlanningTest::_testGeoFenceUpload()
{
    ZoneOwner zoneParent;
    GeoZone::Volume polygonVolume;
    polygonVolume.rings.append(GeoZone::toRing(_circleRing(QGeoCoordinate(47, 8), 500, 40)));
    GeoZone::Data polygonData;
    polygonData.identifier = QStringLiteral("P");
    polygonData.restriction = GeoZone::Prohibited;
    polygonData.volumes.append(polygonVolume);
    GeoZone::Volume circleVolume;
    circleVolume.isCircle = true;
    circleVolume.center = QGeoCoordinate(47, 8.05);
    circleVolume.radius = 300;
    GeoZone::Data circleData;
    circleData.identifier = QStringLiteral("C");
    circleData.restriction = GeoZone::ReqAuthorisation;
    circleData.volumes.append(circleVolume);
    const QList<const GeoZone*> zones = { zoneParent.add(polygonData), zoneParent.add(circleData) };

    for (const MAV_AUTOPILOT autopilot : { MAV_AUTOPILOT_PX4, MAV_AUTOPILOT_ARDUPILOTMEGA }) {
        _connectMockLink(autopilot);
        Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
        QVERIFY(vehicle);
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->initialPlanRequestComplete(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->parameterManager()->parametersReady(), 30000);

        PlanMasterController masterController;
        masterController.setFlyView(false);
        masterController.start();
        QCOMPARE(masterController.managerVehicle(), vehicle);
        GeoFenceController *const geoFenceController = masterController.geoFenceController();
        GeoZoneFenceImporter importer;
        importer.setGeoFenceController(geoFenceController);
        GeoZoneFenceUpload *const upload = importer.upload();
        QVERIFY(upload->canUpload());

        // PX4 stores 64 points, the MockLink ArduCopter is estimated with 16 KB storage. Both store circles.
        importer.select(zones, 25);
        const GeoZoneFences::Capacity capacity = importer.capacity();
        QVERIFY(capacity.circles);
        if (autopilot == MAV_AUTOPILOT_PX4) {
            QCOMPARE(capacity.maxItems, GeoZoneFences::Capacity::px4MaxItems);
            QVERIFY(!capacity.estimated);
            QVERIFY(importer.capacityInfo()[QStringLiteral("warning")].toString().isEmpty());
        } else {
            QCOMPARE(capacity.storageBytes, GeoZoneFences::Capacity::arduPilotCopterBytes);
            QVERIFY(capacity.estimated);
            QVERIFY(importer.capacityInfo()[QStringLiteral("text")].toString().contains(QStringLiteral("of 560 bytes")));
            // The MockLink ArduCopter has its fence disabled
            QVERIFY(importer.capacityInfo()[QStringLiteral("warning")].toString().contains(QStringLiteral("FENCE_ENABLE")));
        }
        QCOMPARE(importer.selectedCount(), 2);
        geoFenceController->setBreachReturnPoint(QGeoCoordinate(47, 7.95, 50));

        // The fence is sent and read back
        importer.uploadToVehicle();
        QCOMPARE(upload->state(), GeoZoneFenceUpload::Sending);
        QTRY_COMPARE_WITH_TIMEOUT(upload->state(), GeoZoneFenceUpload::Verified, 15000);
        QVERIFY2(upload->message().startsWith(QStringLiteral("Verified")), qPrintable(upload->message()));
        GeoFenceManager *const manager = vehicle->geoFenceManager();
        QCOMPARE(manager->polygons().count(), 1);
        QCOMPARE(manager->circles().count(), 1);
        QVERIFY(!manager->polygons().first().inclusion());
        QCOMPARE(manager->polygons().first().count(), importer.selection().choices.first().fences.polygons.first().count());
        // The plan keeps its fences from geozones
        QCOMPARE(geoFenceController->polygons()->count(), 1);
        QVERIFY(GeoZoneFenceTag::isGeoZone(geoFenceController->polygons()->get(0)));
        QVERIFY(!geoFenceController->dirty());

        // A vehicle without room for the fence rejects it
        _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailWriteRequest0ErrorAck, MAV_MISSION_NO_SPACE);
        importer.uploadToVehicle();
        QTRY_COMPARE_WITH_TIMEOUT(upload->state(), GeoZoneFenceUpload::Failed, 15000);
        QVERIFY2(upload->message().contains(QStringLiteral("no room")), qPrintable(upload->message()));
        _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailNone, MAV_MISSION_ACCEPTED);

        _disconnectMockLink();
        QTRY_VERIFY(!upload->canUpload());
        importer.uploadToVehicle();
        QCOMPARE(upload->state(), GeoZoneFenceUpload::Failed);
    }

    // Fences read back are compared within the tolerance, in any order
    GeoZoneFenceUpload::FenceSnapshot sent;
    sent.polygons.append({ false, _circleRing(QGeoCoordinate(47, 8), 500, 8) });
    sent.polygons.append({ true, _circleRing(QGeoCoordinate(47, 8), 5000, 4) });
    sent.circles.append({ false, QGeoCircle(QGeoCoordinate(47, 8.05), 300) });
    sent.returnPoint = QGeoCoordinate(47, 7.95, 50);
    GeoZoneFenceUpload::FenceSnapshot received = sent;
    std::swap(received.polygons[0], received.polygons[1]);
    received.circles[0].circle.setRadius(300.1);
    received.returnPoint.setAltitude(0);
    QVERIFY(GeoZoneFenceUpload::compareFences(sent, received).isEmpty());
    received.circles[0].circle.setRadius(302);
    QVERIFY(GeoZoneFenceUpload::compareFences(sent, received).contains(QStringLiteral("Fences differing from the plan: 1.")));
    received.circles.clear();
    QVERIFY(GeoZoneFenceUpload::compareFences(sent, received).contains(QStringLiteral("0 circle(s)")));
    received = sent;
    received.returnPoint = QGeoCoordinate();
    QVERIFY(!GeoZoneFenceUpload::compareFences(sent, received).isEmpty());
}

void GeoZonePlanningTest::_testGeoFenceUploadChanges()
{
    ZoneOwner zoneParent;
    GeoZone::Volume volume;
    volume.rings.append(GeoZone::toRing(_circleRing(QGeoCoordinate(47, 8), 500, 40)));
    GeoZone::Data data;
    data.identifier = QStringLiteral("P");
    data.restriction = GeoZone::Prohibited;
    data.volumes.append(volume);
    const QList<const GeoZone*> zones = { zoneParent.add(data) };

    _connectMockLink(MAV_AUTOPILOT_PX4);
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->initialPlanRequestComplete(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->parameterManager()->parametersReady(), 30000);

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);
    GeoZoneFenceUpload *const upload = importer.upload();
    importer.select(zones, 25);
    QCOMPARE(importer.selectedCount(), 1);
    const auto moveVertex = [geoFenceController]() {
        QGCFencePolygon *const polygon = geoFenceController->polygons()->value<QGCFencePolygon*>(0);
        polygon->adjustVertex(0, polygon->vertexCoordinate(0).atDistanceAndAzimuth(50, 0));
    };

    // An edit while sending isn't on the vehicle, so the fence stays dirty once the transfer is done
    importer.uploadToVehicle();
    QCOMPARE(upload->state(), GeoZoneFenceUpload::Sending);
    moveVertex();
    QCOMPARE(upload->state(), GeoZoneFenceUpload::Aborted);
    QTRY_VERIFY_WITH_TIMEOUT(!geoFenceController->syncInProgress(), 15000);
    QVERIFY(geoFenceController->dirty());
    QCOMPARE(upload->state(), GeoZoneFenceUpload::Aborted);

    // An edit while verifying: the fence read back matches the one sent, not the plan's
    {
        QObject context;
        (void) connect(upload, &GeoZoneFenceUpload::stateChanged, &context, [upload, &moveVertex]() {
            if (upload->state() == GeoZoneFenceUpload::Verifying) {
                moveVertex();
            }
        });
        upload->start();
        QTRY_COMPARE_WITH_TIMEOUT(upload->state(), GeoZoneFenceUpload::Aborted, 15000);
    }
    QVERIFY2(upload->message().contains(QStringLiteral("changed")), qPrintable(upload->message()));
    QVERIFY(geoFenceController->dirty());
    QTRY_VERIFY_WITH_TIMEOUT(!geoFenceController->syncInProgress(), 15000);
    QCOMPARE(upload->state(), GeoZoneFenceUpload::Aborted);

    // Clearing the plan
    upload->start();
    QCOMPARE(upload->state(), GeoZoneFenceUpload::Sending);
    masterController.removeAll();
    QCOMPARE(upload->state(), GeoZoneFenceUpload::Aborted);
    QTRY_VERIFY_WITH_TIMEOUT(!geoFenceController->syncInProgress(), 15000);

    // Unchanged, the upload is verified and the fence clean
    importer.uploadToVehicle();
    QTRY_COMPARE_WITH_TIMEOUT(upload->state(), GeoZoneFenceUpload::Verified, 15000);
    QVERIFY(!geoFenceController->dirty());

    // The vehicle disconnecting while the fence is read back fails the upload
    _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailReadRequestListNoResponse, MAV_MISSION_ACCEPTED);
    upload->start();
    QTRY_COMPARE_WITH_TIMEOUT(upload->state(), GeoZoneFenceUpload::Verifying, 15000);
    _disconnectMockLink();
    QTRY_COMPARE(upload->state(), GeoZoneFenceUpload::Failed);
    QVERIFY2(upload->message().contains(QStringLiteral("disconnected")), qPrintable(upload->message()));
}

void GeoZonePlanningTest::_testMapModel()
{
    const QGeoRectangle view(QGeoCoordinate(47.1, 7.85), QGeoCoordinate(46.9, 8.15));
    const QJsonArray features = {
        _polygonFeature(QStringLiteral("BIG"), QStringLiteral("PROHIBITED"), _circleRing(QGeoCoordinate(47.0, 8.0), 4000, 2000)),
        _circleFeature(QStringLiteral("TINY"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.01, 8.01), 20),
        _circleFeature(QStringLiteral("NEAR"), QStringLiteral("CONDITIONAL"), QGeoCoordinate(47.0, 8.2), 500),     // Within the view margin
        _circleFeature(QStringLiteral("FAR"), QStringLiteral("PROHIBITED"), QGeoCoordinate(50.0, 15.0), 1000),
    };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString), qPrintable(errorString));

    GeoZoneMapModel model(&manager);
    QSignalSpy shapesSpy(&model, &GeoZoneMapModel::shapesChanged);
    const auto identifiers = [&model]() {
        QStringList result;
        for (const GeoZoneShape *shape : model.shapes()) {
            result.append(shape->zone()->identifier());
        }
        result.sort();
        return result;
    };
    const auto polygon = [&model]() {
        const QSet<const GeoZoneShape*> &shapes = model.shapes();
        const auto it = std::find_if(shapes.cbegin(), shapes.cend(), [](const GeoZoneShape *shape) { return !shape->isCircle(); });
        return (it != shapes.cend()) ? *it : nullptr;
    };

    // Only shapes in or near the view
    model.setView(view, 10, false);
    QCOMPARE(identifiers(), QStringList({ QStringLiteral("BIG"), QStringLiteral("NEAR"), QStringLiteral("TINY") }));
    QCOMPARE(model.detailLevel(), 10);
    QCOMPARE(model.hiddenZoneCount(), 0);
    QCOMPARE(model.labelZones().count(), 0);
    const GeoZoneShape *const polygonShape = polygon();
    QVERIFY(polygonShape);

    // Zooming in keeps the shapes and adds detail. Only zones centered in the view are labelled.
    shapesSpy.clear();
    model.setView(view, 16.5, true);
    QCOMPARE(shapesSpy.count(), 1);
    QCOMPARE(model.detailLevel(), 16);
    QCOMPARE(polygon(), polygonShape);
    QCOMPARE(model.labelZones().count(), 2);
    model.setView(view, 16, true);
    QCOMPARE(shapesSpy.count(), 1);

    // Another file adds shapes, removing a source removes its shapes
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("other.json"),
        { _circleFeature(QStringLiteral("OTHER"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.0, 8.0), 1000) }), errorString), qPrintable(errorString));
    QVERIFY(identifiers().contains(QStringLiteral("OTHER")));
    // Shapes of the other sources are kept when a source reloads
    QCOMPARE(polygon(), polygonShape);
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("other.json"),
        { _circleFeature(QStringLiteral("OTHER"), QStringLiteral("CONDITIONAL"), QGeoCoordinate(47.0, 8.0), 1000) }), errorString), qPrintable(errorString));
    QCOMPARE(polygon(), polygonShape);
    QVERIFY(identifiers().contains(QStringLiteral("OTHER")));
    // Shapes are left out before their source deletes them
    shapesSpy.clear();
    qsizetype changesBeforeRemoval = -1;
    const QMetaObject::Connection removal = connect(&manager, &GeoZoneManager::zonesAboutToChange, &model, [&]() { changesBeforeRemoval = shapesSpy.count(); });
    manager.repository()->removeSource(0);
    (void) disconnect(removal);
    QCOMPARE(changesBeforeRemoval, 1);
    QCOMPARE(identifiers(), QStringList({ QStringLiteral("OTHER") }));

    // Restrictions hidden on maps leave out their shapes and labels
    const auto restoreRestrictions = qScopeGuard([&manager]() { manager.display()->setRestrictionVisible(GeoZone::Conditional, true); });
    QVERIFY(!model.labelZones().isEmpty());
    manager.display()->setRestrictionVisible(GeoZone::Conditional, false);
    QVERIFY(!manager.display()->restrictionVisible(GeoZone::Conditional));
    QVERIFY(model.shapes().isEmpty());
    QVERIFY(model.labelZones().isEmpty());
    manager.display()->setRestrictionVisible(GeoZone::Conditional, true);
    QCOMPARE(identifiers(), QStringList({ QStringLiteral("OTHER") }));

    model.setView(QGeoRectangle(), 16, true);
    QVERIFY(model.shapes().isEmpty());
    QCOMPARE(model.labelZones().count(), 0);

    model.setView(view, 16, true);
    manager.repository()->clear();
    QVERIFY(model.shapes().isEmpty());
}

void GeoZonePlanningTest::_testMapModelShapeLimit()
{
    // More zones in view than can be shown, with growing radius
    QJsonArray features;
    const int extraZones = 10;
    for (int i = 0; i < GeoZoneMapModel::maxShapeCount + extraZones; i++) {
        const QGeoCoordinate center(47.0 + (i / 50) * 0.005, 8.0 + (i % 50) * 0.005);
        features.append(_circleFeature(QStringLiteral("Z%1").arg(i), QStringLiteral("PROHIBITED"), center, 100 + i));
    }
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString), qPrintable(errorString));

    // The smallest zones are dropped and not labelled
    GeoZoneMapModel model(&manager);
    model.setView(manager.boundingBox(), 12, true);
    QCOMPARE(model.shapes().count(), GeoZoneMapModel::maxShapeCount);
    QCOMPARE(model.hiddenZoneCount(), extraZones);
    for (const GeoZoneShape *shape : model.shapes()) {
        QVERIFY(shape->radius() >= 100 + extraZones);
    }
    QVERIFY(model.labelZones().count() > 0);
    for (int i = 0; i < model.labelZones().count(); i++) {
        QVERIFY(GeoZoneInfo::fromVariant(model.labelZones().at(i))->identifier().mid(1).toInt() >= extraZones);
    }
}

void GeoZonePlanningTest::_testMapModelFilters()
{
    ScopedEnvironment environment;
    const QDateTime now = _utc(QDate(2026, 6, 15), QTime(10, 0));
    environment.setNow(now);
    const QGeoCoordinate center(47, 8);
    const auto withLimits = [](QJsonObject feature, double lower, const QString &lowerReference, double upper, const QString &upperReference) {
        QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
        QJsonObject layer = geometry[0].toObject();
        layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
        layer[QStringLiteral("lowerLimit")] = lower;
        layer[QStringLiteral("lowerVerticalReference")] = lowerReference;
        layer[QStringLiteral("upperLimit")] = upper;
        layer[QStringLiteral("upperVerticalReference")] = upperReference;
        geometry[0] = layer;
        feature[QStringLiteral("geometry")] = geometry;
        return feature;
    };
    QJsonObject later = _circleFeature(QStringLiteral("LATER"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2000, 270), 300);
    later[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" },
        { "startDateTime", now.addSecs(6 * 3600).toString(Qt::ISODate) }, { "endDateTime", now.addSecs(8 * 3600).toString(Qt::ISODate) } } };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        withLimits(_circleFeature(QStringLiteral("LOW"), QStringLiteral("PROHIBITED"), center, 300), 0, QStringLiteral("AGL"), 60, QStringLiteral("AGL")),
        withLimits(_circleFeature(QStringLiteral("HIGH"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2000, 90), 300),
                   1500, QStringLiteral("AMSL"), 3000, QStringLiteral("AMSL")),
        _circleFeature(QStringLiteral("ALL"), QStringLiteral("CONDITIONAL"), center.atDistanceAndAzimuth(2000, 0), 300),
        later,
    }), errorString), qPrintable(errorString));
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("other.json"), QJsonArray{
        _circleFeature(QStringLiteral("OTHER"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(2000, 180), 300),
    }), errorString), qPrintable(errorString));

    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    GeoZoneDisplay *const display = manager.display();
    const auto restoreFilters = qScopeGuard([display]() { display->resetFilters(); });

    GeoZoneMapModel model(&manager);
    const QGeoRectangle view(center.atDistanceAndAzimuth(5000, 315), center.atDistanceAndAzimuth(5000, 135));
    model.setView(view, 12, false);
    const auto shown = [&model]() {
        QStringList identifiers;
        for (const GeoZoneShape *shape : model.shapes()) {
            identifiers.append(shape->zone()->identifier());
        }
        identifiers.sort();
        return identifiers.join(QLatin1Char(' '));
    };
    const auto zone = [&manager](const QString &identifier) {
        const QList<GeoZone*> &zones = manager.zoneList();
        return *std::find_if(zones.cbegin(), zones.cend(), [&identifier](const GeoZone *z) { return z->identifier() == identifier; });
    };
    const auto altitudes = [](double altitudeAMSL, double terrainAMSL) {
        GeoZoneAltitudeRange range;
        range.add(GeoZone::Position{ QGeoCoordinate(47, 8), altitudeAMSL, terrainAMSL });
        return range;
    };
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));
    QCOMPARE(model.filteredZoneCount(), 0);
    QVERIFY(!display->filtered());

    // Restrictions hidden in the legend, except kept and highlighted zones, also at map clicks
    display->setRestrictionVisible(GeoZone::Prohibited, false);
    QVERIFY(display->filtered());
    QCOMPARE(shown(), QStringLiteral("ALL OTHER"));
    QCOMPARE(model.filteredZoneCount(), 3);
    QCOMPARE(model.zonesAt(center).count(), 0);
    model.setKeptZones(GeoZoneInfo::toVariantList(QList<const GeoZone*>{ zone(QStringLiteral("LOW")) }));
    model.setHighlightedZones(GeoZoneInfo::toVariantList(QList<const GeoZone*>{ zone(QStringLiteral("HIGH")) }));
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LOW OTHER"));
    QCOMPARE(model.filteredZoneCount(), 1);
    QCOMPARE(model.zonesAt(center).count(), 1);
    model.setKeptZones({});
    model.setHighlightedZones({});
    display->resetFilters();
    QVERIFY(!display->filtered());
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));

    // Altitudes: all zones without them, then those within the margin of the vehicle above the terrain or sea level
    settings->altitudeFilter()->setRawValue(true);
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));
    model.setAltitudeRange(altitudes(500, 450));
    QCOMPARE(shown(), QStringLiteral("ALL LATER LOW OTHER"));
    QCOMPARE(model.filteredZoneCount(), 1);
    model.setAltitudeRange(altitudes(1450, 450));
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER OTHER"));
    model.setAltitudeRange(altitudes(1500 - GeoZoneMapFilter::altitudeMargin - 2, 450));
    QCOMPARE(shown(), QStringLiteral("ALL LATER OTHER"));
    // Limits above ground are kept without the terrain
    model.setAltitudeRange(altitudes(1450, qQNaN()));
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));
    // A range of altitudes, like those of a mission climbing from the ground
    GeoZoneAltitudeRange mission = altitudes(450, 450);
    mission.add(GeoZone::Position{ center, 1450, 450 });
    model.setAltitudeRange(mission);
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));
    model.setAltitudeRange(altitudes(500, 450));

    // Schedules: active now, or within the hours ahead
    settings->scheduleFilter()->setRawValue(GeoZoneMapFilter::ActiveNow);
    QCOMPARE(shown(), QStringLiteral("ALL LOW OTHER"));
    QCOMPARE(model.filteredZoneCount(), 2);
    settings->scheduleFilter()->setRawValue(GeoZoneMapFilter::ActiveSoon);
    QCOMPARE(shown(), QStringLiteral("ALL LATER LOW OTHER"));
    // LATER becomes active later than the hours ahead
    environment.setNow(now.addSecs(qint64{ 6 - GeoZoneMapFilter::scheduleHours - 1 } * 3600));
    settings->scheduleFilter()->setRawValue(GeoZoneMapFilter::AnyTime);
    settings->scheduleFilter()->setRawValue(GeoZoneMapFilter::ActiveSoon);
    QCOMPARE(shown(), QStringLiteral("ALL LOW OTHER"));

    // Sources, with the other filters
    settings->mapSource()->setRawValue(zone(QStringLiteral("OTHER"))->sourceKey());
    QCOMPARE(shown(), QStringLiteral("OTHER"));
    QCOMPARE(model.filteredZoneCount(), 4);
    settings->mapSource()->setRawValue(zone(QStringLiteral("LOW"))->sourceKey());
    QCOMPARE(shown(), QStringLiteral("ALL LOW"));
    display->setRestrictionVisible(GeoZone::Conditional, false);
    QCOMPARE(shown(), QStringLiteral("LOW"));
    // The key of a source which isn't loaded doesn't hide the others
    settings->mapSource()->setRawValue(QStringLiteral("removed.json"));
    QCOMPARE(shown(), QStringLiteral("LOW OTHER"));

    display->resetFilters();
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));
    QCOMPARE(model.filteredZoneCount(), 0);

    // The altitudes of a mission, read again when its items change
    settings->altitudeFilter()->setRawValue(true);
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    const QGeoCoordinate home = center.atDistanceAndAzimuth(1000, 270);
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    QList<SimpleMissionItem*> items;
    for (const QGeoCoordinate &coordinate : { home, center.atDistanceAndAzimuth(1000, 90) }) {
        items.append(qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count())));
        items.last()->altitude()->setRawValue(50);
    }
    model.setMissionController(missionController);
    // The home altitude may change to the terrain elevation once it is received, the zone floor is far above both
    QTRY_VERIFY_WITH_TIMEOUT(model.altitudeRange().isValid(), 5000);
    QCOMPARE(qRound(model.altitudeRange().maxAMSL - model.altitudeRange().minAMSL), 0);
    QCOMPARE(shown(), QStringLiteral("ALL LATER LOW OTHER"));
    items.last()->altitude()->setRawValue(1450);
    QTRY_COMPARE_WITH_TIMEOUT(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"), 5000);
    QCOMPARE(qRound(model.altitudeRange().maxAMSL - model.altitudeRange().minAMSL), 1400);
    model.setMissionController(nullptr);
}

GEOZONE_TEST(GeoZonePlanningTest)
