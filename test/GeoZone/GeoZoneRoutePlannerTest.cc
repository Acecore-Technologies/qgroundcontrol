/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRoutePlannerTest.h"
#include "GeoZoneMissionReader.h"
#include "GeoZoneTestBase.h"
#include "GeoZoneTestHelpers.h"
#include "SettingsManager.h"
#include "UnitsSettings.h"

#include <QtCore/QScopeGuard>

#include <algorithm>

using namespace GeoZoneTestHelpers;

GeoZoneRoutePlannerTest::GeoZoneRoutePlannerTest() = default;

GeoZoneRoutePlannerTest::~GeoZoneRoutePlannerTest() = default;

void GeoZoneRoutePlannerTest::initTestCase()
{
    _metricUnits = std::make_unique<ScopedMetricUnits>();
}

void GeoZoneRoutePlannerTest::cleanupTestCase()
{
    _metricUnits.reset();
}

void GeoZoneRoutePlannerTest::_testRoutePlanner()
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

    // A climb above the maximum altitude is suggested with the altitude it needs
    manager.repository()->clear();
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("sea_high.json"), QJsonArray{
        _withLimits(_circleFeature(QStringLiteral("H"), QStringLiteral("PROHIBITED"), seaCenter, 200), 0, QStringLiteral("AMSL"), 200, QStringLiteral("AMSL")) }), errorString), qPrintable(errorString));
    lowerPlanner.update();
    const double suggestedMaxAltitude = lowerPlanner.suggestions().isEmpty() ? qQNaN() :
                                            lowerPlanner.suggestions().constLast().toMap()[QStringLiteral("suggestedMaxAltitude")].toDouble();
    QVERIFY2((suggestedMaxAltitude >= 210) && (suggestedMaxAltitude <= 211), qPrintable(suggestionTexts()));
    lowerPlanner.setMaxAltitude(suggestedMaxAltitude);
    lowerPlanner.update();
    const QVariantList climbSuggestions = lowerPlanner.suggestions();
    QVERIFY2(std::any_of(climbSuggestions.cbegin(), climbSuggestions.cend(), [](const QVariant &suggestion) {
        return suggestion.toMap()[QStringLiteral("kind")].toString() == QStringLiteral("climb");
    }), qPrintable(suggestionTexts()));
    QVERIFY(!climbSuggestions.constLast().toMap().contains(QStringLiteral("suggestedMaxAltitude")));
}

namespace {

/// @return Index of the first suggestion of the kind which can be applied, -1 without
int applicableSuggestion(const GeoZoneRoutePlanner &planner, const QString &kind)
{
    const QVariantList suggestions = planner.suggestions();
    for (int i = 0; i < suggestions.count(); i++) {
        const QVariantMap suggestion = suggestions[i].toMap();
        if (suggestion[QStringLiteral("canApply")].toBool() && (suggestion[QStringLiteral("kind")].toString() == kind)) {
            return i;
        }
    }
    return -1;
}

/// @return true once the altitudes of the legs of the mission are known
bool legAltitudesKnown(MissionController *missionController)
{
    const QList<GeoZoneMissionPath::Leg> legs = GeoZoneMissionReader::path(missionController).legs();
    return !legs.isEmpty() && !qIsNaN(legs.constLast().segment.fromAltitudeAMSL) && !qIsNaN(legs.constLast().segment.toAltitudeAMSL);
}

} // namespace

void GeoZoneRoutePlannerTest::_testMissionChanges()
{
    ScopedEnvironment environment;
    environment.setTerrain(0.0);
    // A leg at 450 m above the sea through a zone up to 500 m, which a detour or a climb avoids
    const QGeoCoordinate center(0.5, -25);
    const QGeoCoordinate west = center.atDistanceAndAzimuth(2000, 270);
    const QGeoCoordinate east = center.atDistanceAndAzimuth(2000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _withLimits(_circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300), 0, QStringLiteral("AMSL"), 500, QStringLiteral("AMSL")) }), errorString),
             qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    QmlObjectListModel *const visualItems = missionController->visualItems();
    visualItems->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(west.latitude(), west.longitude(), 400));
    const auto addWaypoint = [missionController, visualItems](const QGeoCoordinate &coordinate) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, visualItems->count()));
        if (item) {
            item->setAltitudeMode(QGroundControlQmlGlobal::AltitudeModeAbsolute);
            item->altitude()->setRawValue(450);
        }
        return item;
    };
    QVERIFY(addWaypoint(west));
    QVERIFY(addWaypoint(east));
    QTRY_VERIFY(legAltitudesKnown(missionController));
    QCOMPARE(visualItems->count(), 3);

    GeoZoneRoutePlanner planner(&manager);
    planner.setMissionController(missionController);
    planner.setMaxAltitude(1000);
    planner.update();
    QVERIFY(applicableSuggestion(planner, QStringLiteral("detour")) >= 0);
    QVERIFY(applicableSuggestion(planner, QStringLiteral("climb")) >= 0);

    // The suggestions refer to the items of the mission by index: an item added before them drops them at once, before
    // the mission controller recalculates the flight path
    QVERIFY(missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(3000, 0), 1));
    QVERIFY(planner.suggestions().isEmpty());
    QVERIFY(!planner.applySuggestion(0));
    QCOMPARE(visualItems->count(), 4);
    missionController->removeVisualItem(1);
    QCOMPARE(visualItems->count(), 3);
    QTRY_VERIFY(legAltitudesKnown(missionController));

    // A search in progress is of the mission as it was: its suggestions never come
    planner.startUpdate();
    QVERIFY(planner.busy());
    missionController->removeVisualItem(2);
    QVERIFY(!planner.busy());
    QVERIFY(_settleWorkers());
    QVERIFY(planner.suggestions().isEmpty());
    QVERIFY(addWaypoint(east));
    QTRY_VERIFY(legAltitudesKnown(missionController));

    // While the suggestions are shown, they are searched again once the changes settle, and the dialog shows it meanwhile
    planner.setActive(true);
    QTRY_VERIFY(!planner.busy());
    QVERIFY(applicableSuggestion(planner, QStringLiteral("detour")) >= 0);
    qobject_cast<SimpleMissionItem*>(visualItems->get(2))->altitude()->setRawValue(460);
    QTRY_VERIFY(planner.busy());
    QVERIFY(planner.suggestions().isEmpty());
    QTRY_VERIFY(!planner.busy());
    QVERIFY(applicableSuggestion(planner, QStringLiteral("detour")) >= 0);
    planner.setActive(false);

    // Should a change of the mission go unnoticed, a suggestion is still only applied to the items it was made for
    planner.update();
    const int climb = applicableSuggestion(planner, QStringLiteral("climb"));
    const int detour = applicableSuggestion(planner, QStringLiteral("detour"));
    QVERIFY((climb >= 0) && (detour >= 0));
    QSignalSpy failedSpy(&planner, &GeoZoneRoutePlanner::applyFailed);
    {
        const QSignalBlocker missionBlocker(missionController);
        const QSignalBlocker itemsBlocker(visualItems);
        missionController->removeVisualItem(1);
    }
    QCOMPARE(visualItems->count(), 2);
    QCOMPARE(planner.suggestions().count() > climb, true);
    SimpleMissionItem *const remaining = qobject_cast<SimpleMissionItem*>(visualItems->get(1));
    QVERIFY(remaining);
    const double altitude = remaining->altitude()->rawValue().toDouble();
    QVERIFY(!planner.applySuggestion(climb));
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(!planner.applySuggestion(detour));
    QCOMPARE(failedSpy.count(), 2);
    QVERIFY2(failedSpy[1][0].toString().contains(QStringLiteral("The mission changed")), qPrintable(failedSpy[1][0].toString()));
    QCOMPARE(visualItems->count(), 2);
    QCOMPARE(remaining->altitude()->rawValue().toDouble(), altitude);
}

void GeoZoneRoutePlannerTest::_testDetourAltitudes()
{
    // The altitude of an item in each altitude mode
    const double nan = qQNaN();
    const auto inMode = [nan](int mode, double altitudeAMSL, double homeAltitudeAMSL, double terrainAMSL) {
        return GeoZoneRoutePlanner::altitudeInMode(mode, altitudeAMSL, homeAltitudeAMSL, terrainAMSL).value_or(nan);
    };
    QCOMPARE(inMode(QGroundControlQmlGlobal::AltitudeModeAbsolute, 450, nan, nan), 450.0);
    QCOMPARE(inMode(QGroundControlQmlGlobal::AltitudeModeRelative, 450, 400, nan), 50.0);
    QCOMPARE(inMode(QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain, 450, 400, 420), 30.0);
    QCOMPARE(inMode(QGroundControlQmlGlobal::AltitudeModeTerrainFrame, 450, 400, 420), 30.0);
    // Unknown where what the mode refers to is, and in the modes which aren't those of an item, like one read from a file
    QVERIFY(qIsNaN(inMode(QGroundControlQmlGlobal::AltitudeModeAbsolute, nan, 400, 420)));
    QVERIFY(qIsNaN(inMode(QGroundControlQmlGlobal::AltitudeModeRelative, 450, nan, 420)));
    QVERIFY(qIsNaN(inMode(QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain, 450, 400, nan)));
    QVERIFY(qIsNaN(inMode(QGroundControlQmlGlobal::AltitudeModeTerrainFrame, 450, 400, nan)));
    QVERIFY(qIsNaN(inMode(QGroundControlQmlGlobal::AltitudeModeMixed, 450, 400, 420)));
    QVERIFY(qIsNaN(inMode(QGroundControlQmlGlobal::AltitudeModeNone, 450, 400, 420)));
    QVERIFY(qIsNaN(inMode(42, 450, 400, 420)));
    QVERIFY(qIsNaN(inMode(-1, 450, 400, 420)));

    ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate west = center.atDistanceAndAzimuth(2000, 270);
    const QGeoCoordinate east = center.atDistanceAndAzimuth(2000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"),
        QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300) }), errorString), qPrintable(errorString));

    // A leg climbing from 450 m to 490 m above mean sea level, in a mission whose new waypoints are above the terrain,
    // which isn't known
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    QmlObjectListModel *const visualItems = missionController->visualItems();
    missionController->setGlobalAltitudeMode(QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain);
    for (const auto &[coordinate, altitude] : { std::make_pair(west, 450.0), std::make_pair(east, 490.0) }) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, visualItems->count()));
        QVERIFY(item);
        item->setAltitudeMode(QGroundControlQmlGlobal::AltitudeModeAbsolute);
        item->altitude()->setRawValue(altitude);
    }
    QTRY_VERIFY(legAltitudesKnown(missionController));

    GeoZoneRoutePlanner planner(&manager);
    planner.setMissionController(missionController);
    planner.update();
    int detour = applicableSuggestion(planner, QStringLiteral("detour"));
    QVERIFY(detour >= 0);

    // The waypoints can't be given the altitudes the detour was checked at: none is inserted, and the dialog tells why
    QSignalSpy failedSpy(&planner, &GeoZoneRoutePlanner::applyFailed);
    QVERIFY(!planner.applySuggestion(detour));
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY2(failedSpy[0][0].toString().contains(QStringLiteral("The detour wasn't inserted")), qPrintable(failedSpy[0][0].toString()));
    QCOMPARE(visualItems->count(), 3);

    // With the terrain at the waypoints they are, above it
    environment.setTerrain(420.0);
    planner.update();
    detour = applicableSuggestion(planner, QStringLiteral("detour"));
    QVERIFY(detour >= 0);
    QVERIFY(planner.applySuggestion(detour));
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(visualItems->count() > 3);
    double previous = 450;
    for (int i = 2; i < (visualItems->count() - 1); i++) {
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(visualItems->get(i));
        QVERIFY(item);
        QCOMPARE(item->altitudeMode(), QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain);
        // Between those of the ends of the leg, rising along the detour
        const double altitudeAMSL = item->altitude()->rawValue().toDouble() + 420;
        QVERIFY2((altitudeAMSL > previous) && (altitudeAMSL < 490), qPrintable(QString::number(altitudeAMSL)));
        previous = altitudeAMSL;
    }
}

void GeoZoneRoutePlannerTest::_testMaxAltitudeText()
{
    // In feet, which meters aren't whole numbers of
    Fact *const units = SettingsManager::instance()->unitsSettings()->verticalDistanceUnits();
    const QVariant previousUnits = units->rawValue();
    const auto restoreUnits = qScopeGuard([units, previousUnits]() { units->setRawValue(previousUnits); });
    units->setRawValue(UnitsSettings::VerticalDistanceUnitsFeet);

    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    GeoZoneRoutePlanner planner(&manager);

    // The limit a climb needs, 410.1 ft
    planner.setMaxAltitude(125);
    QCOMPARE(planner.maxAltitudeText(), QStringLiteral("410"));
    // The field gives its text back when it loses the focus: the rounded text doesn't replace the limit, which would
    // then be below what the climb needs
    QSignalSpy maxAltitudeSpy(&planner, &GeoZoneRoutePlanner::maxAltitudeChanged);
    planner.setMaxAltitudeText(planner.maxAltitudeText());
    planner.setMaxAltitudeText(QStringLiteral(" 410 "));
    QCOMPARE(planner.maxAltitude(), 125.0);
    QCOMPARE(maxAltitudeSpy.count(), 0);

    // What the pilot types is the limit
    planner.setMaxAltitudeText(QStringLiteral("400"));
    QCOMPARE(maxAltitudeSpy.count(), 1);
    QVERIFY(_near(planner.maxAltitude(), 400 * 0.3048, 1e-9));
    QCOMPARE(planner.maxAltitudeText(), QStringLiteral("400"));
    // Unless it isn't a number of at least 1
    for (const QString &text : { QStringLiteral("abc"), QString(), QStringLiteral("0"), QStringLiteral("-20") }) {
        planner.setMaxAltitudeText(text);
    }
    QCOMPARE(maxAltitudeSpy.count(), 1);
    QCOMPARE(planner.maxAltitudeText(), QStringLiteral("400"));
}

void GeoZoneRoutePlannerTest::_testRoutePlannerWorker()
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
    QTRY_VERIFY(!qIsNaN(GeoZoneMissionReader::path(missionController).legs().constLast().segment.toAltitudeAMSL));

    GeoZoneRoutePlanner planner(&manager);
    planner.setMissionController(missionController);
    planner.update();
    const QVariantList synchronous = planner.suggestions();
    QVERIFY(synchronous.count() >= 3);
    QVERIFY(!planner.busy());
    // The mission stays as it is: what the mission controller recalculates later would drop the searches as outdated
    const QSignalBlocker missionBlocker(missionController);

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

GEOZONE_TEST(GeoZoneRoutePlannerTest)
