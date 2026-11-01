/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePlanningTest.h"
#include "GeoZoneTestHelpers.h"

using namespace GeoZoneTestHelpers;

void GeoZonePlanningTest::_testPathHits()
{
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate square = center.atDistanceAndAzimuth(3000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("B"), QStringLiteral("CONDITIONAL"), { square.atDistanceAndAzimuth(282.8, 45), square.atDistanceAndAzimuth(282.8, 135),
                                                                              square.atDistanceAndAzimuth(282.8, 225), square.atDistanceAndAzimuth(282.8, 315) }),
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1000, 90), 200),
        // Unpublished restriction types can't be assumed harmless
        _circleFeature(QStringLiteral("X"), QStringLiteral("MYSTERY"), center.atDistanceAndAzimuth(2000, 90), 100),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(500, 90), 100),
    }), errorString), qPrintable(errorString));

    // Each zone once, in the order the path enters them, with the distance to the entry
    const QList<GeoZone::Position> east = { GeoZone::Position{ center }, GeoZone::Position{ center.atDistanceAndAzimuth(2000, 90) },
                                            GeoZone::Position{ center.atDistanceAndAzimuth(4000, 90) }, GeoZone::Position{ center } };
    const QList<GeoZoneCatalog::PathHit> hits = manager.catalog().pathHits(east, GeoZonePolicy::alerts());
    QCOMPARE(hits.count(), 3);
    QCOMPARE(hits[0].zone->identifier(), QStringLiteral("A"));
    QCOMPARE(hits[1].zone->identifier(), QStringLiteral("X"));
    QCOMPARE(hits[2].zone->identifier(), QStringLiteral("B"));
    QVERIFY(qAbs(hits[0].distance - 800) < 1);
    QVERIFY(qAbs(hits[1].distance - 1900) < 1);
    QCOMPARE(hits[1].entry.segment, 0);
    QVERIFY(qAbs(hits[2].distance - 2800) < 1);
    QCOMPARE(hits[2].entry.segment, 1);
    QVERIFY(qAbs(center.distanceTo(hits[2].entry.position.coordinate) - 2800) < 1);

    // The first path entering a zone reports it
    const QList<GeoZoneCatalog::PathHit> pathHits = manager.catalog().pathHits({ { GeoZone::Position{ center.atDistanceAndAzimuth(1000, 0) }, GeoZone::Position{ center.atDistanceAndAzimuth(1000, 0).atDistanceAndAzimuth(4000, 90) } }, east },
                                                                               GeoZonePolicy({ GeoZone::Prohibited }));
    QCOMPARE(pathHits.count(), 1);
    QCOMPARE(pathHits[0].path, 1);

    // Restricted zones and unknown restrictions are warned about, others are information
    GeoZoneMonitor monitor(&manager);
    const QString warning = monitor.guidedCheck()->pathWarning(east);
    QVERIFY(warning.contains(QStringLiteral("A (Prohibited)")));
    QVERIFY(warning.contains(QStringLiteral("X (Unknown)")));
    QVERIFY(!warning.contains(QStringLiteral("N (")));
    QVERIFY(GeoZonePolicy::isRestricted(GeoZone::Unknown));
    QVERIFY(!GeoZonePolicy::isRestricted(GeoZone::USpace));
}

void GeoZonePlanningTest::_testAntimeridianQueries()
{
    // A zone from 179.9 E to 179.9 W, one on the other side of the world and one along the equator from 0 to 160 W,
    // spanning more than 180 degrees of longitude
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), { QGeoCoordinate(-16.6, 179.9), QGeoCoordinate(-16.4, 179.9),
                                                                             QGeoCoordinate(-16.4, -179.9), QGeoCoordinate(-16.6, -179.9) }),
        _circleFeature(QStringLiteral("FAR"), QStringLiteral("PROHIBITED"), QGeoCoordinate(-16.5, 0), 5000),
        _polygonFeature(QStringLiteral("WIDE"), QStringLiteral("PROHIBITED"), { QGeoCoordinate(-1, 0), QGeoCoordinate(-1, 50), QGeoCoordinate(-1, 100),
                                                                                QGeoCoordinate(-1, 150), QGeoCoordinate(-1, -160), QGeoCoordinate(1, -160),
                                                                                QGeoCoordinate(1, 150), QGeoCoordinate(1, 100), QGeoCoordinate(1, 50),
                                                                                QGeoCoordinate(1, 0) }),
    }), errorString), qPrintable(errorString));
    const GeoZoneCatalog &catalog = manager.catalog();
    const GeoZonePolicy policy({ GeoZone::Prohibited });
    const auto identifiers = [](const QList<const GeoZone*> &zones) {
        QStringList result;
        for (const GeoZone *zone : zones) {
            result.append(zone->identifier());
        }
        return result;
    };

    // Points on both sides of the antimeridian
    QCOMPARE(identifiers(catalog.zonesAt(QGeoCoordinate(-16.5, 179.95))), QStringList({ QStringLiteral("X") }));
    QCOMPARE(identifiers(catalog.zonesAt(QGeoCoordinate(-16.5, -179.95))), QStringList({ QStringLiteral("X") }));
    QCOMPARE(identifiers(catalog.zonesAt(QGeoCoordinate(-16.5, 180))), QStringList({ QStringLiteral("X") }));
    QVERIFY(catalog.zonesAt(QGeoCoordinate(-16.5, 179.8)).isEmpty());

    // Areas across the antimeridian, and the rest of the world between the same longitudes
    QCOMPARE(identifiers(catalog.zonesInArea(GeoZoneBox(-17, 179.8, -16, -179.8), policy)), QStringList({ QStringLiteral("X") }));
    QCOMPARE(identifiers(catalog.zonesInArea(QGeoRectangle(QGeoCoordinate(-16, 179.8), QGeoCoordinate(-17, -179.8)), policy)), QStringList({ QStringLiteral("X") }));
    QCOMPARE(identifiers(catalog.zonesInArea(GeoZoneBox(-17, -179.8, -16, 179.8), policy)), QStringList({ QStringLiteral("FAR") }));

    // Survey areas across the antimeridian and on the other side of the world
    const GeoZone::Area area{ { QGeoCoordinate(-16.6, 179.8), QGeoCoordinate(-16.4, 179.8), QGeoCoordinate(-16.4, -179.8), QGeoCoordinate(-16.6, -179.8) }, {}, {} };
    QCOMPARE(identifiers(catalog.zonesIntersectingAreas({ area }, policy)), QStringList({ QStringLiteral("X") }));
    const GeoZone::Area farArea{ { QGeoCoordinate(-16.6, -0.1), QGeoCoordinate(-16.4, -0.1), QGeoCoordinate(-16.4, 0.1), QGeoCoordinate(-16.6, 0.1) }, {}, {} };
    QCOMPARE(identifiers(catalog.zonesIntersectingAreas({ farArea }, policy)), QStringList({ QStringLiteral("FAR") }));

    // A mission path across the antimeridian enters the zone at its west edge
    const QGeoCoordinate from(-16.5, 179.8);
    const QGeoCoordinate to(-16.5, -179.8);
    const double entryDistance = from.distanceTo(QGeoCoordinate(-16.5, 179.9));
    const QList<GeoZoneCatalog::PathHit> hits = catalog.pathHits({ GeoZone::Position{ from }, GeoZone::Position{ to } }, policy);
    QCOMPARE(hits.count(), 1);
    QCOMPARE(hits[0].zone->identifier(), QStringLiteral("X"));
    QVERIFY(qAbs(hits[0].distance - entryDistance) < (entryDistance * 0.01));

    // The profile of a segment across the antimeridian passes through the zone
    GeoZoneMissionPath::Segment segment;
    segment.from = from;
    segment.to = to;
    segment.distance = from.distanceTo(to);
    const QList<GeoZoneProfile::Band> bands = GeoZoneProfile::computeBands({ segment }, catalog);
    QCOMPARE(bands.count(), 1);
    QCOMPARE(bands[0].name, QStringLiteral("X"));
    QVERIFY(qAbs(bands[0].points.constFirst().distance - entryDistance) < 20);
    QVERIFY(qAbs(bands[0].points.constLast().distance - (segment.distance - entryDistance)) < 20);

    // Vehicles past the antimeridian more than 180 degrees from the west end of the wide zone are alerted inside it
    // and near its east end
    const GeoZoneAlertTracker::Limits limits{ 100, 100, 0 };
    GeoZoneAlertTracker::Update update = GeoZoneAlertTracker().update(catalog, policy, GeoZone::Position{ QGeoCoordinate(0, -170) }, {}, limits);
    QCOMPARE(update.alerts.count(), 1);
    QCOMPARE(update.alerts[0].zone->identifier(), QStringLiteral("WIDE"));
    QVERIFY(update.alerts[0].inside);
    const QGeoCoordinate nearEastEnd(0, -159.9995);
    update = GeoZoneAlertTracker().update(catalog, policy, GeoZone::Position{ nearEastEnd }, {}, limits);
    QCOMPARE(update.alerts.count(), 1);
    QCOMPARE(update.alerts[0].zone->identifier(), QStringLiteral("WIDE"));
    QVERIFY(!update.alerts[0].inside);
    QVERIFY(qAbs(update.alerts[0].distance - nearEastEnd.distanceTo(QGeoCoordinate(0, -160))) < 1);
}

void GeoZonePlanningTest::_testMissionCheck()
{
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
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));

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
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("high.json"), QJsonArray{ high }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();

    GeoZoneMissionCheck check(&manager);
    check.setMissionController(missionController);

    // Passes the no restriction zone only, which is reported without a warning
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 180).atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 180).atDistanceAndAzimuth(1000, 90), 2);
    QTRY_COMPARE(check.permittedZones().count(), 1);
    QVERIFY(check.crossedZones().isEmpty());
    QVERIFY(check.warningText().isEmpty());
    QVERIFY(check.uSpaceZones().isEmpty());
    QVERIFY(check.infoText().startsWith(QStringLiteral("Areas without restriction: N.")));

    // Through the prohibited zone and then the conditional one, listed most restrictive first
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 3);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 4);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 0), 5);
    QTRY_COMPARE(check.crossedZones().count(), 2);
    QCOMPARE(check.crossedZones()[0]->identifier(), QStringLiteral("P"));
    QCOMPARE(check.crossedZones()[1]->identifier(), QStringLiteral("C"));
    QVERIFY(check.warningText().contains(QStringLiteral("P (Prohibited)")));
    // The path north also passes through the U-space airspace
    QCOMPARE(check.uSpaceZones().count(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("U-space airspace: U.")));
    QVERIFY(check.crossedZonesBoundingBox().contains(center));
    QVERIFY(check.crossedZonesBoundingBox().contains(center.atDistanceAndAzimuth(2000, 0)));

    // The fence must exclude the prohibited zones, circles and polygons added from them do
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    QVERIFY(geoFenceController->supported());
    check.setGeoFenceController(geoFenceController);
    QTRY_COMPARE(check.unfencedZones().count(), 1);
    QVERIFY(check.fenceText().contains(QStringLiteral("P")));
    geoFenceController->addInclusionCircle(center.atDistanceAndAzimuth(1000, 315), center.atDistanceAndAzimuth(1000, 135));
    check.update();
    QCOMPARE(check.unfencedZones().count(), 1);
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);
    QCOMPARE(importer.addZones(check.property("unfencedZones").toList()), 1);
    QTRY_VERIFY(check.unfencedZones().isEmpty());
    QVERIFY(check.fenceText().isEmpty());
    geoFenceController->deleteFences(false);
    QTRY_COMPARE(check.unfencedZones().count(), 1);
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
    geoFenceController->deleteFences(false);
    check.setGeoFenceController(nullptr);

    // A temporary restriction on the path counts at any time, or when it is active during the planned flight
    QJsonObject temporary = _circleFeature(QStringLiteral("T"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(700, 270), 100);
    temporary[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-01-01T10:00:00Z" }, { "endDateTime", "2027-01-01T12:00:00Z" } } };
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("temporary.json"), QJsonArray{ temporary }), errorString), qPrintable(errorString));
    QTRY_COMPARE(check.crossedZones().count(), 3);
    QCOMPARE(check.inactiveZoneCount(), 0);
    manager.planning()->setPlannedStart(_utc(QDate(2027, 1, 1), QTime(8, 0)));
    QTRY_COMPARE(check.crossedZones().count(), 2);
    QCOMPARE(check.inactiveZoneCount(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("1 zone(s) on the path are not active at the planned flight time.")));
    // The mission takes a few minutes, so a start just before the restriction overlaps it
    const QDateTime overlappingStart = _utc(QDate(2027, 1, 1), QTime(10, 0)).addSecs(-std::llround(missionController->missionTime() / 2));
    QVERIFY(missionController->missionTime() > 10);
    manager.planning()->setPlannedStart(overlappingStart);
    QTRY_COMPARE(check.crossedZones().count(), 3);
    // A mission started now, like one on the vehicle, ignores the planned start
    check.setFlightNow(true);
    QTRY_COMPARE(check.crossedZones().count(), 2);
    QVERIFY(!check.infoText().contains(QStringLiteral("planned flight time")));
    check.setFlightNow(false);
    manager.planning()->clearPlannedStart();
    QTRY_COMPARE(check.inactiveZoneCount(), 0);
    QCOMPARE(check.crossedZones().count(), 3);

    // A survey whose area covers a zone its flight lines miss
    const QGeoCoordinate surveyCenter = center.atDistanceAndAzimuth(5000, 180);
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("survey.json"),
        QJsonArray{ _circleFeature(QStringLiteral("S"), QStringLiteral("PROHIBITED"), surveyCenter.atDistanceAndAzimuth(395, 45), 2) }), errorString), qPrintable(errorString));
    const int surveyIndex = missionController->visualItems()->count();
    SurveyComplexItem *const survey = qobject_cast<SurveyComplexItem*>(missionController->insertComplexMissionItem(SurveyComplexItem::name, surveyCenter, surveyIndex));
    QVERIFY(survey);
    survey->surveyAreaPolygon()->clear();
    survey->surveyAreaPolygon()->appendVertices({ surveyCenter.atDistanceAndAzimuth(424, 315), surveyCenter.atDistanceAndAzimuth(424, 45),
                                                  surveyCenter.atDistanceAndAzimuth(424, 135), surveyCenter.atDistanceAndAzimuth(424, 225) });
    const auto surveyZoneCrossed = [&check]() {
        const QList<const GeoZone*> crossed = check.crossedZones();
        return std::any_of(crossed.cbegin(), crossed.cend(), [](const GeoZone *zone) {
            return zone->identifier() == QStringLiteral("S");
        });
    };
    QTRY_VERIFY(surveyZoneCrossed());

    // Zones are released before the manager deletes them
    manager.clear();
    QVERIFY(check.crossedZones().isEmpty());
}

void GeoZonePlanningTest::_testMissionCoverage()
{
    // L under a loiter circle away from the legs, V past a back transition, and R on the way home above the mission
    const QGeoCoordinate home(47, 8);
    const QGeoCoordinate loiter = home.atDistanceAndAzimuth(2000, 90);
    const QGeoCoordinate last = loiter.atDistanceAndAzimuth(2000, 0);
    QJsonObject returnZone = _circleFeature(QStringLiteral("R"), QStringLiteral("PROHIBITED"), home.atDistanceAndAzimuth(home.distanceTo(last) / 2, home.azimuthTo(last)), 100);
    QJsonArray geometry = returnZone[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = 1000;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
    layer[QStringLiteral("upperLimit")] = 2000;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AMSL");
    geometry[0] = layer;
    returnZone[QStringLiteral("geometry")] = geometry;
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
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

    // The drawn flight path enters none of them, the vehicle's return altitude is unknown offline
    GeoZoneMissionCheck check(&manager);
    check.setMissionController(missionController);
    QTRY_COMPARE(check.crossedZones().count(), 3);
    const auto zone = [&check](const QString &identifier) {
        const auto it = std::find_if(check.crossedZones().cbegin(), check.crossedZones().cend(), [&identifier](const GeoZone *crossed) { return crossed->identifier() == identifier; });
        return (it != check.crossedZones().cend()) ? *it : nullptr;
    };
    QCOMPARE(check.pathLabels(zone(QStringLiteral("L"))), QStringList({ QStringLiteral("the loiter at item 1") }));
    QCOMPARE(check.pathLabels(zone(QStringLiteral("V"))), QStringList({ QStringLiteral("the VTOL transition at item 3") }));
    QCOMPARE(check.pathLabels(zone(QStringLiteral("R"))), QStringList({ QStringLiteral("the return to launch at an unknown altitude") }));
    QVERIFY2(check.warningText().contains(QStringLiteral("Only the loiter at item 1 enters L.")), qPrintable(check.warningText()));

    // A smaller loiter stays clear of L
    loiterItem->setRadius(100);
    QTRY_VERIFY(!zone(QStringLiteral("L")));
    QCOMPARE(check.crossedZones().count(), 2);
}

void GeoZonePlanningTest::_testAuthorisations()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString storage = dir.filePath(QStringLiteral("storage"));
    GeoZoneManager manager(storage);
    QString errorString;
    QJsonObject controlZone = _circleFeature(QStringLiteral("CTR"), QStringLiteral("REQ_AUTHORISATION"), center, 300);
    controlZone[QStringLiteral("country")] = QStringLiteral("CHE");
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ controlZone }), errorString), qPrintable(errorString));
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
    check.setMissionController(missionController);
    QTRY_COMPARE(check.crossedZones().count(), 1);

    // Dates are local days, the last one included
    QVERIFY(!authorisations->authorise(zoneInfo, QString(), QStringLiteral("2026-13-40"), QString()).isEmpty());
    QVERIFY(!authorisations->authorise(zoneInfo, QString(), QStringLiteral("2026-11-02"), QStringLiteral("2026-11-01")).isEmpty());
    QCOMPARE(authorisations->count(), 0);

    // An authorisation without end is listed for information instead of warned about
    const QDate today = QDate::currentDate();
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral(" P-123 "), QString(), QString()).isEmpty());
    QCOMPARE(authorisations->count(), 1);
    QCOMPARE(authorisations->authorisationOf(zoneInfo)[QStringLiteral("permit")].toString(), QStringLiteral("P-123"));
    QCOMPARE(authorisations->authorisationOf(zoneInfo)[QStringLiteral("statusText")].toString(), QStringLiteral("Valid"));
    QTRY_VERIFY(check.crossedZones().isEmpty());
    QCOMPARE(check.authorisedZones().count(), 1);
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
    QTRY_COMPARE(check.crossedZones().count(), 1);
    QVERIFY(!monitor.guidedCheck()->pathWarning(path).isEmpty());
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 1);

    // An authorisation from tomorrow covers a flight planned then, not one now
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-124"), today.addDays(1).toString(Qt::ISODate), today.addDays(3).toString(Qt::ISODate)).isEmpty());
    manager.planning()->setPlannedStart(QDateTime(today, QTime(12, 0)));
    QTRY_COMPARE(check.crossedZones().count(), 1);
    manager.planning()->setPlannedStart(QDateTime(today.addDays(2), QTime(12, 0)));
    QTRY_VERIFY(check.crossedZones().isEmpty());

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
    GeoZonePlanData::load(planJson, &manager);
    QCOMPARE(manager.planning()->plannedStart(), QDateTime(today.addDays(2), QTime(12, 0)).toUTC());
    QCOMPARE(authorisations->count(), 0);
    QCOMPARE(authorisations->pendingAuthorisations().count(), 1);
    QCOMPARE(authorisations->pendingAuthorisations()[0].toMap()[QStringLiteral("permit")].toString(), QStringLiteral("P-124"));
    authorisations->dismissPending();
    QCOMPARE(authorisations->count(), 0);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());
    GeoZonePlanData::load(planJson, &manager);
    authorisations->acceptPending();
    QCOMPARE(authorisations->count(), 1);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());
    // Authorisations already known aren't offered again
    GeoZonePlanData::load(planJson, &manager);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());

    // Kept across restarts next to the stored sources
    GeoZoneManager restarted(storage);
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QCOMPARE(restarted.zoneCount(), 1);
    QCOMPARE(restarted.planning()->authorisations()->count(), 1);
    QVERIFY(restarted.planning()->authorisations()->find(restarted.zoneList().constFirst()));
    manager.planning()->clearPlannedStart();
}

void GeoZonePlanningTest::_testRoutePlanner()
{
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
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"),
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
    QVERIFY(!GeoZoneRoutePlanner::detourAround(center.atDistanceAndAzimuth(350, 0), east, { prohibited }));

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
    QStringList texts;
    int detourIndex = -1;
    for (int i = 0; i < planner.suggestions().count(); i++) {
        const QVariantMap suggestion = planner.suggestions()[i].toMap();
        texts.append(suggestion[QStringLiteral("text")].toString());
        if (suggestion[QStringLiteral("canApply")].toBool()) {
            detourIndex = i;
        }
    }
    QVERIFY2(texts.filter(QStringLiteral("fly below the floor of R")).count() == 1, qPrintable(texts.join(QStringLiteral(" | "))));
    QVERIFY2(detourIndex >= 0, qPrintable(texts.join(QStringLiteral(" | "))));
    QVERIFY(texts[detourIndex].contains(QStringLiteral("detour around P, R")));

    // Inserting the detour leaves the zones
    const int itemCount = missionController->visualItems()->count();
    QVERIFY(planner.applySuggestion(detourIndex));
    QVERIFY(missionController->visualItems()->count() > itemCount);
    GeoZoneMissionCheck check(&manager);
    check.setMissionController(missionController);
    QTRY_VERIFY_WITH_TIMEOUT(check.crossedZones().isEmpty(), 5000);
    QTRY_VERIFY(planner.suggestions().isEmpty());
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
    GeoZoneTerrain::setLookup([](const QGeoCoordinate &) { return 0.0; });
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
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("sea.json"), QJsonArray{ seaZone(QStringLiteral("S"), 40) }), errorString), qPrintable(errorString));
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
        }
        return texts.join(QStringLiteral(" | "));
    };
    QTRY_VERIFY2(floorSuggestion() >= 0, qPrintable(suggestionTexts()));
    QVERIFY(suggestionTexts().contains(QStringLiteral("Apply lowers 2 waypoint(s) to 10 m below it.")));
    QVERIFY(lowerPlanner.applySuggestion(floorSuggestion()));
    QCOMPARE(legItems[0]->altitude()->rawValue().toDouble(), 30.0);
    QCOMPARE(legItems[1]->altitude()->rawValue().toDouble(), 30.0);
    QTRY_VERIFY2((lowerPlanner.update(), lowerPlanner.suggestions().isEmpty()), qPrintable(suggestionTexts()));

    // Not when the lowered leg would come too close to the sea
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("sea_low.json"), QJsonArray{ seaZone(QStringLiteral("T"), 15) }), errorString), qPrintable(errorString));
    QTRY_VERIFY((lowerPlanner.update(), suggestionTexts().contains(QStringLiteral("floor of T"))));
    QCOMPARE(floorSuggestion(), -1);
    GeoZoneTerrain::setLookup({});
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
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));

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
    QCOMPARE(polygon->name(), zoneName(zones[0]));
    QVERIFY(polygon->geoZone());
    QCOMPARE(circle->name(), zoneName(zones[1]));
    QVERIFY(circle->geoZone());

    // Which the plan file keeps, while fences of other plans load without them
    QString errorString;
    QJsonObject polygonJson;
    const_cast<QGCFencePolygon*>(polygon)->saveToJson(polygonJson);
    QGCFencePolygon loadedPolygon(true);
    QVERIFY2(loadedPolygon.loadFromJson(polygonJson, true, errorString), qPrintable(errorString));
    QCOMPARE(loadedPolygon.name(), polygon->name());
    QVERIFY(loadedPolygon.geoZone());
    // Loading an unchanged exclusion polygon leaves it clean
    QGCFencePolygon reloadedPolygon(false);
    QVERIFY2(reloadedPolygon.loadFromJson(polygonJson, true, errorString), qPrintable(errorString));
    QVERIFY(!reloadedPolygon.dirty());
    QJsonObject circleJson;
    circle->saveToJson(circleJson);
    QGCFenceCircle loadedCircle;
    QVERIFY2(loadedCircle.loadFromJson(circleJson, errorString), qPrintable(errorString));
    QCOMPARE(loadedCircle.name(), circle->name());
    QVERIFY(loadedCircle.geoZone());
    polygonJson.remove(QStringLiteral("name"));
    polygonJson.remove(QStringLiteral("geoZone"));
    QVERIFY2(loadedPolygon.loadFromJson(polygonJson, true, errorString), qPrintable(errorString));
    QVERIFY(loadedPolygon.name().isEmpty());
    QVERIFY(!loadedPolygon.geoZone());
    QJsonObject drawnJson;
    loadedPolygon.saveToJson(drawnJson);
    QVERIFY(!drawnJson.contains(QStringLiteral("name")) && !drawnJson.contains(QStringLiteral("geoZone")));

    // Importing the same zones again does not duplicate fences
    QCOMPARE(importer.addExclusions(zones), 0);
    QCOMPARE(geoFenceController->polygons()->count(), 3);
    QCOMPARE(geoFenceController->circles()->count(), 1);

    // Deleting the fences from geozones keeps the drawn ones, in one reset per list
    geoFenceController->setDirty(false);
    geoFenceController->addInclusionCircle(QGeoCoordinate(47.1, 7.9), QGeoCoordinate(46.9, 8.1));
    QCOMPARE(geoFenceController->circles()->count(), 2);
    QSignalSpy resetSpy(geoFenceController->polygons(), &QAbstractItemModel::modelReset);
    geoFenceController->deleteFences(true);
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(geoFenceController->polygons()->count(), 0);
    QCOMPARE(geoFenceController->circles()->count(), 1);
    QVERIFY(!geoFenceController->circles()->value<QGCFenceCircle*>(0)->geoZone());
    QVERIFY(geoFenceController->dirty());
    geoFenceController->deleteFences(false);
    QCOMPARE(geoFenceController->circles()->count(), 0);
}

void GeoZonePlanningTest::_testGeoFenceImportPreview()
{
    // Two circular zones published as 40 vertex polygons
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

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    QVERIFY(masterController.managerVehicle()->px4Firmware());
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);

    // Exceeds the PX4 fence storage
    QVariantMap preview = importer.exclusionsPreview(zones);
    QCOMPARE(preview[QStringLiteral("fenceCount")].toInt(), 2);
    QCOMPARE(preview[QStringLiteral("pointCount")].toInt(), 80);
    QCOMPARE(preview[QStringLiteral("totalPointCount")].toInt(), 80);
    QVERIFY(!preview[QStringLiteral("limitWarning")].toString().isEmpty());

    // Simplified polygons fit
    preview = importer.exclusionsPreview(zones, 25);
    const int simplifiedPointCount = preview[QStringLiteral("pointCount")].toInt();
    QCOMPARE(preview[QStringLiteral("fenceCount")].toInt(), 2);
    QVERIFY(simplifiedPointCount < GeoZoneFenceImporter::px4MaxFencePoints);
    QVERIFY(preview[QStringLiteral("limitWarning")].toString().isEmpty());

    QCOMPARE(importer.addExclusions(zones, 25), 2);
    QCOMPARE(geoFenceController->polygons()->value<QGCFencePolygon*>(0)->count() + geoFenceController->polygons()->value<QGCFencePolygon*>(1)->count(), simplifiedPointCount);

    // Existing fences count towards the total but are not added again
    preview = importer.exclusionsPreview(zones, 25);
    QCOMPARE(preview[QStringLiteral("fenceCount")].toInt(), 0);
    QCOMPARE(preview[QStringLiteral("totalPointCount")].toInt(), simplifiedPointCount);
    preview = importer.exclusionsPreview(zones);
    QCOMPARE(preview[QStringLiteral("totalPointCount")].toInt(), simplifiedPointCount + 80);
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
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString), qPrintable(errorString));

    GeoZoneMapModel model(&manager);
    const auto identifiers = [](const QmlObjectListModel *list) {
        QStringList result;
        for (int i = 0; i < list->count(); i++) {
            result.append(list->value<GeoZoneMapItem*>(i)->zone()->identifier());
        }
        result.sort();
        return result;
    };

    // Only shapes in or near the view
    model.setView(view, 10, false);
    QCOMPARE(identifiers(model.polygons()), QStringList({ QStringLiteral("BIG") }));
    QCOMPARE(identifiers(model.circles()), QStringList({ QStringLiteral("NEAR"), QStringLiteral("TINY") }));
    QCOMPARE(model.hiddenZoneCount(), 0);
    QCOMPARE(model.labelZones().count(), 0);

    GeoZoneMapItem *const polygonItem = model.polygons()->value<GeoZoneMapItem*>(0);
    const qsizetype lowDetailCount = polygonItem->mercatorRings().first().count();
    QVERIFY(lowDetailCount < 2000);

    // Zooming in keeps the items and adds detail. Only zones centered in the view are labelled.
    model.setView(view, 16, true);
    QCOMPARE(model.polygons()->value<GeoZoneMapItem*>(0), polygonItem);
    QCOMPARE(model.labelZones().count(), 2);
    QVERIFY(polygonItem->mercatorRings().first().count() > lowDetailCount);

    // Another file adds items, removing a source removes its items
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("other.json"),
        { _circleFeature(QStringLiteral("OTHER"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.0, 8.0), 1000) }), errorString), qPrintable(errorString));
    QCOMPARE(model.polygons()->count(), 1);
    QVERIFY(identifiers(model.circles()).contains(QStringLiteral("OTHER")));
    // Items of the other sources are kept when a source reloads
    QCOMPARE(model.polygons()->value<GeoZoneMapItem*>(0), polygonItem);
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("other.json"),
        { _circleFeature(QStringLiteral("OTHER"), QStringLiteral("CONDITIONAL"), QGeoCoordinate(47.0, 8.0), 1000) }), errorString), qPrintable(errorString));
    QCOMPARE(model.polygons()->value<GeoZoneMapItem*>(0), polygonItem);
    QVERIFY(identifiers(model.circles()).contains(QStringLiteral("OTHER")));
    manager.removeSource(0);
    QCOMPARE(model.polygons()->count(), 0);
    QCOMPARE(identifiers(model.circles()), QStringList({ QStringLiteral("OTHER") }));

    // Restrictions hidden on maps leave out their shapes and labels
    const auto restoreRestrictions = qScopeGuard([&manager]() { manager.display()->setRestrictionVisible(GeoZone::Conditional, true); });
    QVERIFY(!model.labelZones().isEmpty());
    manager.display()->setRestrictionVisible(GeoZone::Conditional, false);
    QVERIFY(!manager.display()->restrictionVisible(GeoZone::Conditional));
    QCOMPARE(model.circles()->count(), 0);
    QVERIFY(model.labelZones().isEmpty());
    manager.display()->setRestrictionVisible(GeoZone::Conditional, true);
    QCOMPARE(identifiers(model.circles()), QStringList({ QStringLiteral("OTHER") }));

    model.setView(QGeoRectangle(), 16, true);
    QCOMPARE(model.circles()->count(), 0);
    QCOMPARE(model.labelZones().count(), 0);

    model.setView(view, 16, true);
    manager.clear();
    QCOMPARE(model.circles()->count(), 0);
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
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString), qPrintable(errorString));

    // The smallest zones are dropped and not labelled
    GeoZoneMapModel model(&manager);
    model.setView(manager.boundingBox(), 12, true);
    QCOMPARE(model.circles()->count(), GeoZoneMapModel::maxShapeCount);
    QCOMPARE(model.hiddenZoneCount(), extraZones);
    for (int i = 0; i < model.circles()->count(); i++) {
        QVERIFY(model.circles()->value<GeoZoneMapItem*>(i)->radius() >= 100 + extraZones);
    }
    QVERIFY(model.labelZones().count() > 0);
    for (int i = 0; i < model.labelZones().count(); i++) {
        QVERIFY(GeoZoneInfo::fromVariant(model.labelZones().at(i))->identifier().mid(1).toInt() >= extraZones);
    }
}
