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
    squareVolume.rings = { squareZone };
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
    manager.setPlannedStart(_utc(QDate(2027, 1, 1), QTime(8, 0)));
    QTRY_COMPARE(check.crossedZones().count(), 2);
    QCOMPARE(check.inactiveZoneCount(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("1 zone(s) on the path are not active at the planned flight time.")));
    // The mission takes a few minutes, so a start just before the restriction overlaps it
    const QDateTime overlappingStart = _utc(QDate(2027, 1, 1), QTime(10, 0)).addSecs(-std::llround(missionController->missionTime() / 2));
    QVERIFY(missionController->missionTime() > 10);
    manager.setPlannedStart(overlappingStart);
    QTRY_COMPARE(check.crossedZones().count(), 3);
    // A mission started now, like one on the vehicle, ignores the planned start
    check.setFlightNow(true);
    QTRY_COMPARE(check.crossedZones().count(), 2);
    QVERIFY(!check.infoText().contains(QStringLiteral("planned flight time")));
    check.setFlightNow(false);
    manager.clearPlannedStart();
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
        const QList<GeoZone*> crossed = check.crossedZones();
        return std::any_of(crossed.cbegin(), crossed.cend(), [](const GeoZone *zone) {
            return zone->identifier() == QStringLiteral("S");
        });
    };
    QTRY_VERIFY(surveyZoneCrossed());

    // Zones are released before the manager deletes them
    manager.clear();
    QVERIFY(check.crossedZones().isEmpty());
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
    GeoZoneProfile::Segment west;
    west.from = center.atDistanceAndAzimuth(1000, 270);
    west.to = center;
    west.distance = 1000;
    west.terrainSpacing = 100;
    west.terrainAMSL = QList<double>(11, 400);
    GeoZoneProfile::Segment east = west;
    east.from = center;
    east.to = center.atDistanceAndAzimuth(1000, 90);
    GeoZoneProfile::Segment gap;
    gap.distance = 250;
    GeoZoneProfile::Segment beyond;
    beyond.from = center.atDistanceAndAzimuth(1000, 90);
    beyond.to = center.atDistanceAndAzimuth(1300, 90);
    beyond.distance = 300;

    const QVariantList bands = GeoZoneProfile::bandsForSegments({ west, east, gap, beyond }, &manager);
    // Bands with unbounded limits compare equal, so recomputing unchanged bands doesn't repaint
    QCOMPARE(GeoZoneProfile::computeBands({ west, east, gap, beyond }, &manager), GeoZoneProfile::computeBands({ west, east, gap, beyond }, &manager));
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
    const QVariantList unknownTerrain = GeoZoneProfile::bandsForSegments({ beyond }, &manager);
    QCOMPARE(unknownTerrain.count(), 1);
    beyond.from = center.atDistanceAndAzimuth(100, 270);
    beyond.to = center.atDistanceAndAzimuth(100, 90);
    const QVariantList noTerrain = GeoZoneProfile::bandsForSegments({ beyond }, &manager);
    QCOMPARE(noTerrain.count(), 1);
    QVERIFY(qIsNaN(value(noTerrain[0].toMap()[QStringLiteral("points")].toList().first(), "ceiling")));

    // Long paths are sampled more sparsely up to their end
    QList<GeoZoneProfile::Segment> longPath;
    for (int i = 0; i < 60; i++) {
        GeoZoneProfile::Segment leg;
        leg.from = center.atDistanceAndAzimuth(i * 3000, 90);
        leg.to = center.atDistanceAndAzimuth((i + 1) * 3000, 90);
        leg.distance = 3000;
        longPath.append(leg);
    }
    const QVariantList longBands = GeoZoneProfile::bandsForSegments(longPath, &manager);
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
    const QList<GeoZone*> zones = {
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
    QList<GeoZone*> zones;
    for (int i = 0; i < 2; i++) {
        GeoZone::Volume volume;
        volume.rings.append(_circleRing(QGeoCoordinate(47, 8 + (i * 0.1)), 500, 40));
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
    const qsizetype lowDetailCount = QGeoPolygon(polygonItem->geoShape()).perimeter().count();
    QVERIFY(lowDetailCount < 2000);

    // Zooming in keeps the items and adds detail. Only zones centered in the view are labelled.
    model.setView(view, 16, true);
    QCOMPARE(model.polygons()->value<GeoZoneMapItem*>(0), polygonItem);
    QCOMPARE(model.labelZones().count(), 2);
    QVERIFY(QGeoPolygon(polygonItem->geoShape()).perimeter().count() > lowDetailCount);

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
