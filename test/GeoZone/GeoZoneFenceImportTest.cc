/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFenceImportTest.h"
#include "GeoZoneFenceTags.h"
#include "GeoZoneFenceWarning.h"
#include "GeoZoneMonitor.h"
#include "GeoZonePreflightCheck.h"
#include "GeoZoneTestBase.h"
#include "GeoZoneTestHelpers.h"
#include "GeoFenceManager.h"
#include "MockLink.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "Vehicle.h"

#include <QtCore/QScopeGuard>

#include <algorithm>

using namespace GeoZoneTestHelpers;

GeoZoneFenceImportTest::GeoZoneFenceImportTest() = default;

GeoZoneFenceImportTest::~GeoZoneFenceImportTest() = default;

void GeoZoneFenceImportTest::initTestCase()
{
    _metricUnits = std::make_unique<ScopedMetricUnits>();
}

void GeoZoneFenceImportTest::cleanupTestCase()
{
    _metricUnits.reset();
}

void GeoZoneFenceImportTest::_testFenceExcludes()
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

void GeoZoneFenceImportTest::_testGeoFenceImport()
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

void GeoZoneFenceImportTest::_testGeoFenceSelection()
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

void GeoZoneFenceImportTest::_testGeoFenceNotFenced()
{
    // Three zones of 40 points, of which one fits without simplification
    ZoneOwner zoneParent;
    QList<const GeoZone*> zones;
    for (int i = 0; i < 3; i++) {
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
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);

    // The selection names the zones it doesn't fence and why
    importer.select(zones);
    QCOMPARE(importer.selectedCount(), 1);
    QCOMPARE(importer.notFencedCount(), 2);
    QCOMPARE(importer.notFencedText(), QStringLiteral("2 geozone(s) are not fenced: Z1 (No room left in the vehicle fence), Z2 (No room left in the vehicle fence)."));
    QVERIFY(importer.fenceNote().isEmpty());

    // Applied, the plan keeps them while its fences from geozones are those added
    QSignalSpy noteSpy(&importer, &GeoZoneFenceImporter::fenceNoteChanged);
    QCOMPARE(importer.applySelection(), 1);
    QCOMPARE(importer.fenceNote(), importer.notFencedText());
    QCOMPARE(importer.fenceNoteCount(), 2);
    QCOMPARE(importer.fenceNoteZones(), QStringList({ QStringLiteral("Z1: No room left in the vehicle fence"), QStringLiteral("Z2: No room left in the vehicle fence") }));
    QCOMPARE(noteSpy.count(), 1);
    // ... also in the plan file
    const QJsonObject planJson = masterController.saveToJson().object();
    const QJsonObject notFencedJson = planJson[PlanMasterController::kJsonGeoFenceObjectKey].toObject()[QStringLiteral("geoZoneImport")].toObject()[QStringLiteral("notFenced")].toObject();
    QCOMPARE(notFencedJson[QStringLiteral("count")].toInt(), 2);
    QCOMPARE(notFencedJson[QStringLiteral("zones")].toArray().count(), 2);
    GeoZoneFenceTags::setImportSettings(geoFenceController, QJsonObject());
    geoFenceController->setDirty(false);
    QTRY_VERIFY(importer.fenceNote().isEmpty());
    GeoZoneFenceTags::load(geoFenceController, planJson);
    geoFenceController->setDirty(true);
    QTRY_COMPARE(importer.fenceNoteCount(), 2);

    // The text names a few of many zones
    QList<GeoZoneFenceTags::NotFenced> many;
    for (int i = 0; i < 7; i++) {
        many.append({ QStringLiteral("Z%1").arg(i), QStringLiteral("why") });
    }
    QCOMPARE(GeoZoneFenceTags::notFencedText(many, 120, 2), QStringLiteral("120 geozone(s) are not fenced: Z0 (why), Z1 (why) and 118 more."));
    QVERIFY(GeoZoneFenceTags::notFencedText({}, 0).isEmpty());

    // The pre-flight checklist warns about them on the vehicle whose fence an upload verified
    GeoZonePreflightCheck::FenceInput input;
    input.vehicleFence = GeoZoneFenceUpload::snapshot(geoFenceController);
    GeoZoneFenceUpload::Verification verification;
    verification.fence = input.vehicleFence;
    verification.geoZoneFences = 1;
    verification.notFenced = GeoZoneFenceTags::notFenced(geoFenceController, &verification.notFencedCount);
    input.verification = verification;
    const GeoZonePreflightCheck::Item item = GeoZonePreflightCheck::fenceItem(input);
    QCOMPARE(item.state, GeoZonePreflightCheck::Warning);
    QCOMPARE(item.text, importer.fenceNote());

    // Editing the fences from geozones ends the note, as does deleting them
    QGCFencePolygon *const polygon = geoFenceController->polygons()->value<QGCFencePolygon*>(0);
    polygon->adjustVertex(0, polygon->vertexCoordinate(0).atDistanceAndAzimuth(50, 0));
    QTRY_VERIFY(importer.fenceNote().isEmpty());
    QCOMPARE(importer.fenceNoteCount(), 0);
    QVERIFY(GeoZoneFenceTags::notFenced(geoFenceController).isEmpty());
    QCOMPARE(importer.applySelection(), 1);
    QCOMPARE(importer.fenceNoteCount(), 2);
    GeoZoneFenceTags::deleteFences(geoFenceController, true);
    QTRY_VERIFY(importer.fenceNote().isEmpty());

    // A selection leaving out all zones is kept too, there is no fence from geozones to change
    for (const GeoZone *zone : std::as_const(zones)) {
        importer.setZoneIncluded(zone->key(), false);
    }
    QCOMPARE(importer.selectedCount(), 0);
    QCOMPARE(importer.applySelection(), 0);
    QCOMPARE(importer.fenceNoteCount(), 3);
    QVERIFY(importer.fenceNote().contains(QStringLiteral("Z0 (Left out by you)")));
    QVERIFY(masterController.saveToJson().object()[PlanMasterController::kJsonGeoFenceObjectKey].toObject().contains(QStringLiteral("geoZoneImport")));
}

void GeoZoneFenceImportTest::_testGeoFenceActiveTime()
{
    // A permanent zone, one ending in an hour and one starting in half an hour
    ScopedEnvironment environment;
    const QDateTime now = _utc(QDate(2026, 6, 15), QTime(12, 0));
    environment.setNow(now);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager *const manager = GeoZoneManager::instance();
    const auto clearZones = qScopeGuard([manager]() { manager->repository()->clear(); });
    const auto scheduled = [](const QString &name, double longitude, const QDateTime &start, const QDateTime &end) {
        QJsonObject feature = _circleFeature(name, QStringLiteral("PROHIBITED"), QGeoCoordinate(47, longitude), 300);
        QJsonObject period{ { QStringLiteral("startDateTime"), start.toString(Qt::ISODate) } };
        if (end.isValid()) {
            period[QStringLiteral("endDateTime")] = end.toString(Qt::ISODate);
        }
        feature[QStringLiteral("applicability")] = QJsonArray{ period };
        return feature;
    };
    QString errorString;
    QVERIFY2(manager->repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47, 8), 300),
        scheduled(QStringLiteral("ENDS"), 8.1, now.addDays(-1), now.addSecs(3600)),
        scheduled(QStringLiteral("STARTS"), 8.2, now.addSecs(1800), QDateTime()),
    }), errorString), qPrintable(errorString));
    QTRY_COMPARE(manager->zoneCount(), 3);

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(masterController.geoFenceController());
    GeoZonePlanning *const planning = manager->planning();
    const auto clearPlannedStart = qScopeGuard([planning]() { planning->clearPlannedStart(); });
    // The selected zones, then those left out with why, which can't be included
    const auto select = [&importer](int activeWithinHours) {
        importer.updateSelection({ GeoZone::Prohibited }, QGeoRectangle(), activeWithinHours, 25);
        (void) QTest::qWaitFor([&importer]() { return !importer.busy(); }, 5000);
        QStringList selected;
        QStringList leftOut;
        for (const GeoZoneFenceChoiceModel::Row &row : importer.choices()->rows()) {
            (row.selected ? selected : leftOut).append(row.selected ? row.name : QStringLiteral("%1: %2%3").arg(row.name, row.reason, row.includable ? QStringLiteral(" (includable)") : QString()));
        }
        selected.sort();
        leftOut.sort();
        return selected + QStringList{ QStringLiteral("|") } + leftOut;
    };
    const QString partTime = QStringLiteral("Active only part of the flight, QGroundControl warns instead");

    // The flight from 12:20 to 12:20, widened by 15 minutes: the zone starting at 12:30 is left out, since its fence
    // would make the vehicle act once it becomes active. The zone ending at 13:00 is active all the time.
    planning->setPlannedStart(now.addSecs(20 * 60));
    QCOMPARE(select(GeoZoneFenceImporter::AtFlightTime), QStringList({ QStringLiteral("A"), QStringLiteral("ENDS"), QStringLiteral("|"), QStringLiteral("STARTS: ") + partTime }));
    // From 12:50: the zone ending at 13:00 is left out instead
    planning->setPlannedStart(now.addSecs(50 * 60));
    QCOMPARE(select(GeoZoneFenceImporter::AtFlightTime), QStringList({ QStringLiteral("A"), QStringLiteral("STARTS"), QStringLiteral("|"), QStringLiteral("ENDS: ") + partTime }));
    QCOMPARE(importer.notFencedCount(), 1);
    // In the next three hours both change, now only the zone starting later is inactive and not a candidate
    QCOMPARE(select(3), QStringList({ QStringLiteral("A"), QStringLiteral("|"), QStringLiteral("ENDS: ") + partTime, QStringLiteral("STARTS: ") + partTime }));
    QCOMPARE(select(0), QStringList({ QStringLiteral("A"), QStringLiteral("ENDS"), QStringLiteral("|") }));
    // Any time fences them all, whatever their schedule
    QCOMPARE(select(GeoZoneFenceImporter::AnyTime), QStringList({ QStringLiteral("A"), QStringLiteral("ENDS"), QStringLiteral("STARTS"), QStringLiteral("|") }));
}

void GeoZoneFenceImportTest::_testGeoFenceStale()
{
    // A permanent zone and one ending in an hour, selected from the zones of the manager
    ScopedEnvironment environment;
    const QDateTime now = _utc(QDate(2026, 6, 15), QTime(12, 0));
    environment.setNow(now);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager *const manager = GeoZoneManager::instance();
    const auto clearZones = qScopeGuard([manager]() { manager->repository()->clear(); });
    QJsonObject temporary = _circleFeature(QStringLiteral("TMP"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47, 8.1), 300);
    temporary[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { QStringLiteral("startDateTime"), now.addDays(-1).toString(Qt::ISODate) },
                                                                          { QStringLiteral("endDateTime"), now.addSecs(3600).toString(Qt::ISODate) } } };
    const auto load = [&](const QJsonArray &features) {
        QString errorString;
        return manager->repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString);
    };
    QVERIFY(load({ _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47, 8), 300), temporary }));
    QTRY_COMPARE(manager->zoneCount(), 2);

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);
    importer.updateSelection({ GeoZone::Prohibited }, QGeoRectangle(), -1, 25);
    QCOMPARE(importer.applySelection(), 2);
    const int temporaryIndex = (GeoZoneFenceTag::find(geoFenceController->polygons()->get(0))->name() == QStringLiteral("TMP")) ? 0 : 1;
    const GeoZoneFenceTag *const tag = GeoZoneFenceTag::find(geoFenceController->polygons()->get(temporaryIndex));
    const GeoZone *const temporaryZone = manager->zoneList()[(manager->zoneList()[0]->identifier() == QStringLiteral("TMP")) ? 0 : 1];
    QCOMPARE(tag->name(), QStringLiteral("TMP"));
    QCOMPARE(tag->zoneKey(), temporaryZone->key());
    QCOMPARE(tag->contentHash(), temporaryZone->data().contentHash);
    QCOMPARE(tag->validTo(), now.addSecs(3600));
    QVERIFY(importer.staleText().isEmpty());

    // Plans keep the zones of the fences and the settings they were selected with
    const QJsonObject fenceJson = masterController.saveToJson().object()[PlanMasterController::kJsonGeoFenceObjectKey].toObject();
    const QJsonObject polygonJson = fenceJson[QStringLiteral("polygons")].toArray()[temporaryIndex].toObject();
    QCOMPARE(polygonJson[QStringLiteral("zoneKey")].toString(), tag->zoneKey());
    QCOMPARE(polygonJson[QStringLiteral("validTo")].toString(), now.addSecs(3600).toString(Qt::ISODate));
    QCOMPARE(fenceJson[QStringLiteral("geoZoneImport")].toObject()[QStringLiteral("maxSimplifyTolerance")].toDouble(), 25.0);

    // A zone changing or no longer loaded makes the fences outdated, also for the upload, until they are rebuilt
    QVERIFY(load({ _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47, 8), 350) }));
    QTRY_VERIFY2(importer.staleText().contains(QStringLiteral("A (changed)")) && importer.staleText().contains(QStringLiteral("TMP (no longer loaded)")),
                 qPrintable(importer.staleText()));
    QVERIFY(importer.staleText().startsWith(QStringLiteral("Geozone fences are outdated: ")));
    GeoZoneMissionCheck check(manager);
    check.setGeoFenceController(geoFenceController);
    QVERIFY(check.uploadReasons().contains(importer.staleText()));
    QCOMPARE(importer.rebuild(), 1);
    QVERIFY(importer.staleText().isEmpty());
    QVERIFY(check.uploadReasons().isEmpty());

    // Fences of zones whose last period ended are outdated, while new zones don't make them so
    QVERIFY(load({ _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47, 8), 350), temporary }));
    QTRY_COMPARE(manager->zoneCount(), 2);
    QVERIFY(importer.staleText().isEmpty());
    QCOMPARE(importer.rebuild(), 2);
    QVERIFY(GeoZoneFenceTags::staleZones(geoFenceController, manager->catalog().zones(), now.addSecs(1800)).isEmpty());
    QCOMPARE(GeoZoneFenceTags::staleZones(geoFenceController, manager->catalog().zones(), now.addSecs(3600)),
             (QList<GeoZoneFenceTags::StaleZone>{ { QStringLiteral("TMP"), GeoZoneFenceTags::StaleZone::Reason::Expired } }));

    // Fences of plans saved without the settings are rebuilt from their zones still loaded
    GeoZoneFenceTags::setImportSettings(geoFenceController, QJsonObject());
    QVERIFY(load({ _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47, 8), 400) }));
    QTRY_VERIFY(!importer.staleText().isEmpty());
    QCOMPARE(importer.rebuild(), 1);
    QVERIFY(importer.staleText().isEmpty());

    // Zones active at the planned flight time, which rebuilding follows
    QVERIFY(load({ _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47, 8), 400), temporary }));
    QTRY_COMPARE(manager->zoneCount(), 2);
    GeoZonePlanning *const planning = manager->planning();
    const auto clearPlannedStart = qScopeGuard([planning]() { planning->clearPlannedStart(); });
    planning->setPlannedStart(now.addSecs(2 * 3600));
    importer.updateSelection({ GeoZone::Prohibited }, QGeoRectangle(), GeoZoneFenceImporter::AtFlightTime, 25);
    QCOMPARE(importer.applySelection(), 1);
    planning->setPlannedStart(now.addSecs(1800));
    QCOMPARE(importer.rebuild(), 2);
}

void GeoZoneFenceImportTest::_testGeoFenceUpload()
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
        // The pre-flight checklist finds the fence verified, with both fences from geozones
        const std::optional<GeoZoneFenceUpload::Verification> verification = GeoZoneFenceUpload::verification(vehicle);
        QVERIFY(verification);
        QCOMPARE(verification->geoZoneFences, 2);
        QVERIFY(GeoZoneFenceUpload::compareFences(verification->fence, GeoZoneFenceUpload::snapshot(manager->polygons(), manager->circles(), manager->breachReturnPoint())).isEmpty());
        QCOMPARE(verification->notFencedCount, 0);

        // The zones left out are named once the upload is verified, and kept for the pre-flight checklist
        importer.setZoneIncluded(zones[1]->key(), false);
        importer.uploadToVehicle();
        QTRY_COMPARE_WITH_TIMEOUT(upload->state(), GeoZoneFenceUpload::Verified, 15000);
        QVERIFY2(upload->message().endsWith(QStringLiteral(" 1 geozone(s) are not fenced: C (Left out by you).")), qPrintable(upload->message()));
        QCOMPARE(GeoZoneFenceUpload::verification(vehicle)->notFencedCount, 1);
        importer.setZoneIncluded(zones[1]->key(), true);
        importer.uploadToVehicle();
        QTRY_COMPARE_WITH_TIMEOUT(upload->state(), GeoZoneFenceUpload::Verified, 15000);
        QCOMPARE(GeoZoneFenceUpload::verification(vehicle)->notFencedCount, 0);

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

void GeoZoneFenceImportTest::_testGeoFenceUploadChanges()
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

void GeoZoneFenceImportTest::_testFenceWarningVehicle()
{
    // A prohibited zone 500 m east of the MockLink vehicle, as an exclusion fence of the vehicle
    const QGeoCoordinate mockPosition(47.397, 8.5455);
    ZoneOwner zoneParent;
    GeoZone::Volume volume;
    volume.rings.append(GeoZone::toRing(_circleRing(mockPosition.atDistanceAndAzimuth(600, 90), 100, 8)));
    GeoZone::Data data;
    data.identifier = QStringLiteral("P");
    data.restriction = GeoZone::Prohibited;
    data.volumes.append(volume);
    const QList<const GeoZone*> zones = { zoneParent.add(data) };

    for (const MAV_AUTOPILOT autopilot : { MAV_AUTOPILOT_PX4, MAV_AUTOPILOT_ARDUPILOTMEGA }) {
        const bool px4 = autopilot == MAV_AUTOPILOT_PX4;
        _connectMockLink(autopilot);
        Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
        QVERIFY(vehicle);
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->initialPlanRequestComplete(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->parameterManager()->parametersReady(), 30000);
        // The circle fence of ArduPilot is around home
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->homePosition().isValid(), 15000);
        GeoZoneMonitor monitor(GeoZoneManager::instance());
        GeoZoneVehicleMonitor *const vehicleMonitor = monitor.addVehicle(vehicle);
        GeoZoneFenceWarning *const warning = vehicleMonitor->fenceWarning();
        QVERIFY(warning);

        // The fence action from the parameters: PX4 GF_ACTION 1 only warns, the MockLink ArduCopter has FENCE_ACTION 1
        // with its fence disabled
        QCOMPARE(warning->config().firmware, px4 ? GeoZoneFenceWarning::Firmware::PX4 : GeoZoneFenceWarning::Firmware::ArduPilot);
        QCOMPARE(warning->actionText(), px4 ? QStringLiteral("Warning only, no manoeuvre") : QStringLiteral("RTL or land"));
        QVERIFY(!warning->enabled());

        // The fence uploaded from geozones, and the altitude and circle fences of ArduPilot's FENCE_TYPE 7
        PlanMasterController masterController;
        masterController.setFlyView(false);
        masterController.start();
        GeoZoneFenceImporter importer;
        importer.setGeoFenceController(masterController.geoFenceController());
        importer.select(zones, 25);
        QCOMPARE(importer.selectedCount(), 1);
        QSignalSpy configSpy(warning, &GeoZoneFenceWarning::configChanged);
        importer.uploadToVehicle();
        QTRY_COMPARE_WITH_TIMEOUT(importer.upload()->state(), GeoZoneFenceUpload::Verified, 15000);
        QTRY_VERIFY(configSpy.count() > 0);
        QCOMPARE(warning->config().fence.polygons.count(), 1);
        QVERIFY(!warning->config().fence.polygons.first().inclusion);
        if (px4) {
            QVERIFY(warning->config().fence.inclusionUnion);
            QVERIFY(std::isnan(warning->config().fence.maxAltitude));
        } else {
            QCOMPARE(warning->config().fence.maxAltitude, 100.0);
            QCOMPARE(warning->config().fence.maxDistance, 300.0);
            QCOMPARE(warning->config().fence.horizontalMargin, 2.0);
            QVERIFY(!warning->config().fence.inclusionUnion);
        }

        // Enabling the fence, by its parameter like an RC switch would, enables the warning
        const QString parameter = px4 ? QStringLiteral("GF_ACTION") : QStringLiteral("FENCE_ENABLE");
        vehicle->parameterManager()->getParameter(ParameterManager::defaultComponentId, parameter)->setRawValue(px4 ? 3 : 1);
        QTRY_COMPARE(warning->config().action, px4 ? 3 : 1);
        QTRY_VERIFY(warning->enabled());
        QCOMPARE(warning->actionText(), px4 ? QStringLiteral("Return") : QStringLiteral("RTL or land"));

        // Flying east at 12 m/s, less than 8 s before the exclusion polygon 500 m east, or ArduPilot's circle fence 300 m
        // from home, the breach is predicted
        QSignalSpy warningSpy(&monitor, &GeoZoneMonitor::fenceWarning);
        const QGeoCoordinate start = vehicle->homePosition().atDistanceAndAzimuth(px4 ? 430 : 220, 90);
        warning->evaluate(start, 30, GeoZoneFenceWarning::Velocity{ 0, 12, 0, true });
        QVERIFY2(warning->active(), qPrintable(QStringLiteral("%1 home %2 vehicle %3 max %4").arg(px4).arg(vehicle->homePosition().toString(), vehicle->coordinate().toString())
                                                   .arg(warning->config().fence.maxDistance)));
        QCOMPARE(warning->config().fence.polygons.isEmpty(), false);
        const GeoZoneFencePrediction::Breach breach = *warning->breach();
        QCOMPARE(breach.boundary, px4 ? GeoZoneFencePrediction::Boundary::Polygon : GeoZoneFencePrediction::Boundary::MaxDistance);
        QCOMPARE(warningSpy.count(), 1);
        QVERIFY2(warning->text().startsWith(QStringLiteral("Geofence: %1 in ").arg(warning->actionText())), qPrintable(warning->text()));

        monitor.removeVehicle(vehicle);
        _disconnectMockLink();
    }
}

GEOZONE_TEST(GeoZoneFenceImportTest)
