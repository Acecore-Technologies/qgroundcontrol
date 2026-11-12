/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneManagerTest.h"
#include "GeoZoneSearchModel.h"
#include "GeoZoneTestBase.h"
#include "GeoZoneTestHelpers.h"

using namespace GeoZoneTestHelpers;

void GeoZoneManagerTest::_testManagerStartup()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QString url = server.url(QStringLiteral("/ok"));
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        manager.repository()->addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.repository()->busy());
    }
    QCOMPARE(server.requestCount, 1);

    // Downloaded two days ago, so the feed is due for a refresh
    QFile indexFile(QDir(storageDir.path()).filePath(QStringLiteral("sources.json")));
    QVERIFY(indexFile.open(QIODevice::ReadOnly));
    QJsonObject index = QJsonDocument::fromJson(indexFile.readAll()).object();
    indexFile.close();
    QJsonArray sources = index[QStringLiteral("sources")].toArray();
    QCOMPARE(sources.count(), 1);
    const QDateTime downloaded = environment.now().addDays(-2);
    QJsonObject entry = sources[0].toObject();
    entry[QStringLiteral("loadedAt")] = downloaded.toString(Qt::ISODate);
    sources[0] = entry;
    index[QStringLiteral("sources")] = sources;
    QVERIFY(indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(indexFile.write(QJsonDocument(index).toJson()) > 0);
    indexFile.close();

    // Startup loads the stored copy and refreshes due feeds. The test file was issued months ago, but its server
    // just sent it again: nothing is outdated
    GeoZoneManager manager(storageDir.path());
    QSignalSpy outdatedSpy(&manager, &GeoZoneManager::outdatedDataFound);
    manager.init();
    QCOMPARE(manager.sources()->count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount, 2, 10000);
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(manager.zoneCount(), 3);
    QVERIFY(manager.repository()->source(url)->loadedAt() > downloaded.addDays(1));
    QCOMPARE(manager.repository()->validityWarning(), QString());
    QCOMPARE(outdatedSpy.count(), 0);
}

void GeoZoneManagerTest::_testSettings()
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    // The application converts flight levels with the QNH setting, also on the worker threads of checks
    settings->qnhHPa()->setRawValue(1003.0);
    const double workerQnh = QtConcurrent::run([]() { return GeoZoneEnvironment::qnhHPa(); }).result();
    settings->qnhHPa()->setRawValue(settings->qnhHPa()->rawDefaultValue());
    QCOMPARE(workerQnh, 1003.0);
    QCOMPARE(GeoZoneEnvironment::qnhHPa(), GeoZoneEnvironment::kStandardPressureHPa);

    const ScopedEnvironment environment;
    const QList<Fact*> facts = { settings->showZones(), settings->approachExtraDistance(), settings->approachExtraTime(), settings->staleDays(), settings->outdatedInvalid(),
                                 settings->feedRefreshHours(), settings->liveFeedRefreshMinutes(), settings->qnhHPa() };
    const auto restoreDefaults = qScopeGuard([&facts]() {
        for (Fact *fact : facts) {
            fact->setRawValue(fact->rawDefaultValue());
        }
    });

    // The map visibility of earlier versions is kept
    QSettings qSettings;
    qSettings.setValue(QStringLiteral("GeoZoneManager/visible"), false);
    qSettings.remove(QStringLiteral("GeoZoneManager/showZones"));
    {
        const GeoZoneSettings migrated;
    }
    QVERIFY(!qSettings.contains(QStringLiteral("GeoZoneManager/visible")));
    QCOMPARE(qSettings.value(QStringLiteral("GeoZoneManager/showZones")), QVariant(false));
    qSettings.remove(QStringLiteral("GeoZoneManager/showZones"));

    // The approach distance and time set freely before are what the pilot adds to the manufacturer's thresholds: what
    // was above the former defaults of 100 m and 30 s, never less
    qSettings.setValue(QStringLiteral("GeoZoneManager/approachDistance"), 250);
    qSettings.setValue(QStringLiteral("GeoZoneManager/approachTime"), 20);
    {
        const GeoZoneSettings migrated;
    }
    QVERIFY(!qSettings.contains(QStringLiteral("GeoZoneManager/approachDistance")));
    QVERIFY(!qSettings.contains(QStringLiteral("GeoZoneManager/approachTime")));
    QCOMPARE(qSettings.value(QStringLiteral("GeoZoneManager/approachExtraDistance")).toDouble(), 150.0);
    QVERIFY(!qSettings.contains(QStringLiteral("GeoZoneManager/approachExtraTime")));
    qSettings.remove(QStringLiteral("GeoZoneManager/approachExtraDistance"));

    // Flight alerts and their log are always on: the switches turned off before are dropped
    qSettings.setValue(QStringLiteral("GeoZoneManager/alertsEnabled"), false);
    qSettings.setValue(QStringLiteral("GeoZoneManager/logAlerts"), false);
    {
        const GeoZoneSettings migrated;
    }
    QVERIFY(!qSettings.contains(QStringLiteral("GeoZoneManager/alertsEnabled")));
    QVERIFY(!qSettings.contains(QStringLiteral("GeoZoneManager/logAlerts")));

    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QGeoCoordinate center(47, 8);
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300) });
    QFile file(fileName);
    QVERIFY(file.open(QIODevice::ReadWrite) && file.setFileTime(environment.now().addDays(-5), QFileDevice::FileModificationTime));
    file.close();
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));

    // The manager visibility is the setting
    QSignalSpy visibleSpy(manager.display(), &GeoZoneDisplay::visibleChanged);
    settings->showZones()->setRawValue(false);
    QVERIFY(!manager.display()->visible());
    QCOMPARE(visibleSpy.count(), 1);
    manager.display()->setVisible(true);
    QVERIFY(settings->showZones()->rawValue().toBool());

    // Outdated data is reported at the configured age, 7 days by default, as invalid unless turned off
    QVERIFY(manager.repository()->validityWarning().isEmpty());
    QVERIFY(!manager.repository()->dataInvalid());
    settings->staleDays()->setRawValue(3);
    QCOMPARE(manager.repository()->validityWarning(), QStringLiteral("zones.json: Data is 5 days old"));
    QVERIFY(manager.repository()->dataInvalid());
    settings->outdatedInvalid()->setRawValue(false);
    QVERIFY(!manager.repository()->dataInvalid());
    QVERIFY(!manager.repository()->sources().constFirst()->invalid());
    QCOMPARE(manager.repository()->validityWarning(), QStringLiteral("zones.json: Data is 5 days old"));
    // The zones of invalid data still apply
    settings->outdatedInvalid()->setRawValue(true);
    QVERIFY(manager.repository()->dataInvalid());
    QCOMPARE(manager.zoneCount(), 1);

    // Feeds are refreshed at the configured intervals
    GeoZoneSource feed(QStringLiteral("https://example.com/zones.json"), QStringLiteral("https://example.com/zones.json"));
    GeoZoneParser::Result result;
    result.format = GeoZoneParser::Format::ED318;
    feed.setResult(result, environment.now(), environment.now());
    const GeoZoneRefreshScheduler scheduler;
    settings->feedRefreshHours()->setRawValue(6);
    QCOMPARE(scheduler.refreshIntervalSecs(feed), 6 * 3600);
    result.format = GeoZoneParser::Format::FaaTfr;
    feed.setResult(result, environment.now(), environment.now());
    settings->liveFeedRefreshMinutes()->setRawValue(30);
    QCOMPARE(scheduler.refreshIntervalSecs(feed), 30 * 60);

    // Approach alerts at the manufacturer's distance of 100 m without velocity, with the pilot's extra distance
    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.evaluate({ center.atDistanceAndAzimuth(500, 0) });
    QCOMPARE(alertSpy.count(), 0);
    settings->approachExtraDistance()->setRawValue(150);
    monitor.evaluate({ center.atDistanceAndAzimuth(500, 0) });
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Approaching geozone P")));

    // A new QNH converts flight levels differently, the checks using the zones are updated
    QSignalSpy zonesSpy(&manager, &GeoZoneManager::zonesChanged);
    settings->qnhHPa()->setRawValue(1003.0);
    QCOMPARE(zonesSpy.count(), 1);
    QCOMPARE(zonesSpy.last()[0].value<GeoZoneSource*>(), nullptr);
}

void GeoZoneManagerTest::_testLabelZonesLimit()
{
    // Many zones centered in the same small area, alternating restrictions
    QJsonArray features;
    const int zoneCount = GeoZoneManager::maxLabelCount * 2;
    for (int i = 0; i < zoneCount; i++) {
        const QGeoCoordinate center(47.0 + (i / 40) * 0.01, 8.0 + (i % 40) * 0.01);
        features.append(_circleFeature(QStringLiteral("Z%1").arg(i), (i % 2) ? QStringLiteral("NO_RESTRICTION") : QStringLiteral("PROHIBITED"), center, 100));
    }
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fileName = _writeFeatures(dir, QStringLiteral("many.json"), features);

    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));
    QCOMPARE(manager.zoneCount(), zoneCount);

    QVERIFY(manager.labelZones(QGeoRectangle()).isEmpty());

    // Only zones centered in the region are labelled
    const QGeoRectangle smallRegion(QGeoCoordinate(47.005, 7.995), QGeoCoordinate(46.995, 8.035));
    QCOMPARE(manager.labelZones(smallRegion).count(), 4);

    // The limit keeps the most restrictive zones
    const QVariantList labels = manager.labelZones(manager.boundingBox());
    QCOMPARE(labels.count(), GeoZoneManager::maxLabelCount);
    for (const QVariant &object : labels) {
        QCOMPARE(GeoZoneInfo::fromVariant(object)->restriction(), GeoZone::Prohibited);
    }
}

void GeoZoneManagerTest::_testZoneSearch()
{
    const QGeoCoordinate center(47, 8);
    QJsonObject airport = _circleFeature(QStringLiteral("LSZH"), QStringLiteral("CONDITIONAL"), center, 2000);
    airport[QStringLiteral("name")] = QStringLiteral("Z\u00FCrich airport");
    QJsonObject prison = _circleFeature(QStringLiteral("GZW1"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(500, 90), 300);
    prison[QStringLiteral("name")] = QStringLiteral("Prison Zurich West");
    QJsonObject far = _circleFeature(QStringLiteral("FAR"), QStringLiteral("PROHIBITED"), QGeoCoordinate(46, 7), 300);
    far[QStringLiteral("name")] = QStringLiteral("Bern area");
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ airport, prison, far }), errorString), qPrintable(errorString));

    const auto identifiers = [](const QVariantList &zones) {
        QStringList result;
        for (const QVariant &zone : zones) {
            result.append(GeoZoneInfo::fromVariant(zone)->identifier());
        }
        return result;
    };

    // Overlapping zones at a point, the most restrictive first
    QCOMPARE(identifiers(manager.zonesAt(center.atDistanceAndAzimuth(500, 90))), QStringList({ QStringLiteral("GZW1"), QStringLiteral("LSZH") }));
    QCOMPARE(identifiers(manager.zonesAt(center)), QStringList({ QStringLiteral("LSZH") }));
    QVERIFY(manager.zonesAt(QGeoCoordinate(48, 9)).isEmpty());
    // Zones hidden on maps aren't picked
    const auto restoreRestrictions = qScopeGuard([&manager]() { manager.display()->setRestrictionVisible(GeoZone::Prohibited, true); });
    manager.display()->setRestrictionVisible(GeoZone::Prohibited, false);
    QCOMPARE(identifiers(manager.zonesAt(center.atDistanceAndAzimuth(500, 90))), QStringList({ QStringLiteral("LSZH") }));
    manager.display()->setRestrictionVisible(GeoZone::Prohibited, true);

    // Names or identifiers containing the text, those starting with it first, then the most restrictive
    QCOMPARE(identifiers(manager.searchZones(QStringLiteral("zurich"))), QStringList({ QStringLiteral("LSZH"), QStringLiteral("GZW1") }));
    QCOMPARE(identifiers(manager.searchZones(QStringLiteral("I"))), QStringList({ QStringLiteral("GZW1"), QStringLiteral("LSZH") }));
    QCOMPARE(identifiers(manager.searchZones(QStringLiteral("fa"))), QStringList({ QStringLiteral("FAR") }));
    QCOMPARE(manager.searchZones(QStringLiteral("zurich"), 1).count(), 1);
    QVERIFY(manager.searchZones(QStringLiteral("  ")).isEmpty());
    // ignoring accents however they are typed
    QCOMPARE(identifiers(manager.searchZones(QStringLiteral("Z\u00FCrich a"))), QStringList({ QStringLiteral("LSZH") }));
    QCOMPARE(identifiers(manager.searchZones(QStringLiteral("zu\u0308rich a"))), QStringList({ QStringLiteral("LSZH") }));
    QCOMPARE(GeoZoneCatalog::searchKey(QStringLiteral("\u00C9tang Tr\u00F8ndelag \u0141\u00F3d\u017A Stra\u00DFe")), QStringLiteral("etang trondelag lodz strasse"));

    // The same zone from another source is found once, the results update with the zones
    GeoZoneSearchModel searchModel(&manager);
    searchModel.setDelay(0);
    searchModel.setMaxResults(1);
    searchModel.setText(QStringLiteral("zurich"));
    QVERIFY(searchModel.pending());
    QTRY_VERIFY(!searchModel.pending());
    QCOMPARE(searchModel.count(), 1);
    QVERIFY(searchModel.hasMore());
    QCOMPARE(searchModel.data(searchModel.index(0), GeoZoneSearchModel::IdentifierRole).toString(), QStringLiteral("LSZH"));
    searchModel.setMaxResults(8);
    QCOMPARE(searchModel.count(), 2);
    QVERIFY(!searchModel.hasMore());
    // A copy of a zone in another source is one zone, found once
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("copy.json"), QJsonArray{ airport }), errorString), qPrintable(errorString));
    QCOMPARE(manager.zoneCount(), 3);
    QCOMPARE(identifiers(manager.searchZones(QStringLiteral("zurich"))), QStringList({ QStringLiteral("LSZH"), QStringLiteral("GZW1") }));
    QCOMPARE(searchModel.count(), 2);
    manager.repository()->removeSource(1);
    QCOMPARE(manager.zoneCount(), 3);

    QSignalSpy showSpy(&manager, &GeoZoneManager::showZoneRequested);
    manager.showZone(QVariant::fromValue(GeoZoneInfo(manager.zoneList().first())));
    manager.showZone(QVariant(QStringLiteral("not a zone")));
    QCOMPARE(showSpy.count(), 1);
    QCOMPARE(GeoZoneInfo::fromVariant(showSpy.first().first()), manager.zoneList().first());

    // Highlighted zones are marked for the map layer
    GeoZoneMapModel model(&manager);
    model.setView(QGeoRectangle(center.atDistanceAndAzimuth(5000, 315), center.atDistanceAndAzimuth(5000, 135)), 12, false);
    QCOMPARE(model.shapes().count(), 2);
    const QVariant prisonZone = manager.searchZones(QStringLiteral("GZW1")).first();
    QSignalSpy shapesSpy(&model, &GeoZoneMapModel::shapesChanged);
    model.setHighlightedZones({ prisonZone });
    QCOMPARE(shapesSpy.count(), 1);
    for (const GeoZoneShape *shape : model.shapes()) {
        QCOMPARE(model.highlighted(shape->zone()), shape->zone() == GeoZoneInfo::fromVariant(prisonZone));
    }

    // Values given to QML stay readable after their source is removed
    manager.repository()->clear();
    QCOMPARE(manager.zoneCount(), 0);
    QVERIFY(model.shapes().isEmpty());
    QCOMPARE(searchModel.count(), 0);
    QVERIFY(prisonZone.value<GeoZoneInfo>().valid());
    QCOMPARE(prisonZone.value<GeoZoneInfo>().identifier(), QStringLiteral("GZW1"));
}

void GeoZoneManagerTest::_testCatalogSnapshot()
{
    // Checks on worker threads query copies of the catalog, which share its data and keep its zones while the
    // sources change
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("old.json"), QJsonArray{
        _circleFeature(QStringLiteral("OLD"), QStringLiteral("PROHIBITED"), center, 500) }), errorString), qPrintable(errorString));

    QWeakPointer<const GeoZone> oldZone;
    {
        const GeoZoneCatalog snapshot = manager.catalog();
        QCOMPARE(&snapshot.zones(), &manager.catalog().zones());
        oldZone = snapshot.zones().constFirst()->sharedFromThis();

        // Rebuilding the manager's catalog leaves the copy answering with the previous zones after their source
        // is deleted
        const QPointer<GeoZoneSource> oldSource = manager.sources()->value<GeoZoneSource*>(0);
        QVERIFY(oldSource);
        manager.repository()->clear();
        QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("new.json"), QJsonArray{
            _circleFeature(QStringLiteral("NEW"), QStringLiteral("PROHIBITED"), center, 300) }), errorString), qPrintable(errorString));
        QTRY_VERIFY(oldSource.isNull());
        QVERIFY(&snapshot.zones() != &manager.catalog().zones());
        QVERIFY(!oldZone.isNull());

        const QList<const GeoZone*> oldZones = snapshot.zonesAt(center);
        QCOMPARE(oldZones.count(), 1);
        QCOMPARE(oldZones[0]->identifier(), QStringLiteral("OLD"));
        QCOMPARE(snapshot.zonesInArea(QGeoRectangle(), GeoZonePolicy({ GeoZone::Prohibited })).count(), 1);
        const QList<GeoZonePathEvaluator::ZoneResult> hits = GeoZonePathEvaluator(snapshot, GeoZonePolicy({ GeoZone::Prohibited })).evaluate({
            GeoZonePathEvaluator::Path::fromPositions({ GeoZone::Position{ center.atDistanceAndAzimuth(1000, 270) }, GeoZone::Position{ center.atDistanceAndAzimuth(1000, 90) } }) }).zones;
        QCOMPARE(hits.count(), 1);
        QVERIFY(qAbs(hits[0].crossings.constFirst().entry.distance - 500) < 1);

        const QList<const GeoZone*> newZones = manager.catalog().zonesAt(center);
        QCOMPARE(newZones.count(), 1);
        QCOMPARE(newZones[0]->identifier(), QStringLiteral("NEW"));

        // A copy of the copy shares its data too
        const GeoZoneCatalog copy = snapshot;
        QCOMPARE(&copy.zones(), &snapshot.zones());
    }
    // The last copy releases the previous zones
    QVERIFY(oldZone.isNull());
}

void GeoZoneManagerTest::_testMergedSources()
{
    // A regional extract of a national file publishes some of its zones alike, which are kept once
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate other = center.atDistanceAndAzimuth(5000, 90);
    const auto withUpperLimit = [](QJsonObject feature, double upper) {
        QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
        QJsonObject layer = geometry[0].toObject();
        layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
        layer[QStringLiteral("upperLimit")] = upper;
        layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AGL");
        geometry[0] = layer;
        feature[QStringLiteral("geometry")] = geometry;
        return feature;
    };
    const QJsonObject same = _circleFeature(QStringLiteral("SAME"), QStringLiteral("PROHIBITED"), center, 500);
    const QJsonObject limited = withUpperLimit(_circleFeature(QStringLiteral("LIMITS"), QStringLiteral("CONDITIONAL"), other, 300), 120);
    const QJsonObject only = _circleFeature(QStringLiteral("ONLY"), QStringLiteral("CONDITIONAL"), other, 800);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("national.json"), QJsonArray{ same, limited, only }), errorString), qPrintable(errorString));
    QCOMPARE(manager.zoneCount(), 3);
    QCOMPARE(manager.mergedZoneCount(), 0);
    const QString nationalKey = manager.sources()->value<GeoZoneSource*>(0)->key();

    // The zone with the same identifier and other limits is kept from both
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    const QString extractFile = _writeFeatures(dir, QStringLiteral("extract.json"), QJsonArray{ same, withUpperLimit(limited, 150) });
    manager.repository()->addFile(extractFile);
    QVERIFY(addedSpy.wait(10000));
    const QString extractKey = GeoZoneLoader::fileKey(extractFile);
    QCOMPARE(manager.zoneCount(), 4);
    QCOMPARE(manager.mergedZoneCount(), 1);
    QVERIFY(manager.catalog().sharedZoneCounts(extractKey) == (QHash<QString, int>{ { nationalKey, 1 } }));
    // Half of the zones of the extract are loaded already, too few to note
    QVERIFY(manager.sourceNote().isEmpty());

    const QList<const GeoZone*> sameZones = manager.catalog().zonesAt(center);
    QCOMPARE(sameZones.count(), 1);
    const GeoZone *const sameZone = sameZones[0];
    QCOMPARE(sameZone->sourceKey(), nationalKey);
    QCOMPARE(sameZone->sourceCount(), 2);
    QCOMPARE(sameZone->otherSources().count(), 1);
    QCOMPARE(sameZone->otherSources()[0].key, extractKey);
    QVERIFY(sameZone->fromSource(extractKey));
    QVERIFY(GeoZoneInfo(sameZone).details().sourceText().endsWith(QStringLiteral(" · 2 source(s)")));
    QCOMPARE(GeoZoneInfo(sameZone).details().sourcesText().split(QLatin1Char('\n')).count(), 2);
    QCOMPARE(manager.catalog().zonesAt(other).count(), 3);
    QCOMPARE(manager.catalog().search(QStringLiteral("SAME"), 10).count(), 1);
    QCOMPARE(manager.catalog().search(QStringLiteral("LIMITS"), 10).count(), 2);
    // Showing the extract only on the maps shows the zone
    GeoZoneMapFilter filter;
    filter.source = extractKey;
    QVERIFY(filter.accepts(*sameZone, QDateTime::currentDateTimeUtc(), GeoZoneAltitudeRange()));
    QVERIFY(GeoZoneCatalog::sameContent(*sameZone, *sameZone));

    // A file with mostly zones loaded already is noted, until it is removed
    addedSpy.clear();
    manager.repository()->addFile(_writeFeatures(dir, QStringLiteral("copy.json"), QJsonArray{ same, only }));
    QVERIFY(addedSpy.wait(10000));
    QCOMPARE(manager.zoneCount(), 4);
    QCOMPARE(manager.mergedZoneCount(), 3);
    QCOMPARE(manager.catalog().zonesAt(center)[0]->sourceCount(), 3);
    QVERIFY(manager.sourceNote().contains(manager.sources()->value<GeoZoneSource*>(0)->name()));

    // Without the national file the extract keeps the zone
    manager.sources()->value<GeoZoneSource*>(0)->setEnabled(false);
    QCOMPARE(manager.zoneCount(), 3);
    QCOMPARE(manager.catalog().zonesAt(center).count(), 1);
    QCOMPARE(manager.catalog().zonesAt(center)[0]->sourceKey(), extractKey);
    QCOMPARE(manager.catalog().zonesAt(center)[0]->sourceCount(), 2);
    manager.sources()->value<GeoZoneSource*>(0)->setEnabled(true);
    QCOMPARE(manager.catalog().zonesAt(center)[0]->sourceKey(), nationalKey);

    QVERIFY(!manager.sourceNote().isEmpty());
    manager.repository()->removeSource(2);
    QVERIFY(manager.sourceNote().isEmpty());
    QCOMPARE(manager.catalog().zonesAt(center)[0]->sourceCount(), 2);
}

void GeoZoneManagerTest::_testServiceOriginsConfirmed()
{
    // The other servers of a service are confirmed by name: a confirmation given for other servers than those the
    // service has when it is added doesn't count, like after its addresses were edited
    const ScopedEnvironment environment;
    TestFeedServer server;
    TestFeedServer tokenServer;
    TestFeedServer hostile;
    QVERIFY(server.listen(QHostAddress::LocalHost) && tokenServer.listen(QHostAddress::LocalHost) && hostile.listen(QHostAddress::LocalHost));
    tokenServer.files.insert("/token", R"({ "access_token": "token-1", "token_type": "Bearer", "expires_in": 3600 })");
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    GeoZoneManager manager(storageDir.path());
    const auto forgetSecrets = qScopeGuard([&manager]() { manager.repository()->clear(); });
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);
    const QString url = server.url(QStringLiteral("/ok"));
    const QString secret = QStringLiteral("client-s3cret");
    const QString tokenOrigin = tokenServer.url(QString());
    const QString hostileOrigin = hostile.url(QString());
    GeoZoneService service;
    service.auth = GeoZoneService::Auth::OAuth2ClientCredentials;
    service.tokenUrl = tokenServer.url(QStringLiteral("/token"));
    service.user = QStringLiteral("client");
    QCOMPARE(manager.serviceOtherOrigins(url, service.toVariantMap()), QStringList{ tokenOrigin });
    const auto refused = [&](const GeoZoneService &added, const QStringList &confirmedOrigins, const QString &unconfirmedOrigin) {
        const int failures = static_cast<int>(failedSpy.count());
        manager.addService(url, added.toVariantMap(), secret, confirmedOrigins);
        return (failedSpy.count() == (failures + 1)) && failedSpy.last()[1].toString().contains(unconfirmedOrigin) &&
               manager.repository()->credentials().secret(url).isEmpty() && !manager.repository()->hasSource(url) && !manager.repository()->busy();
    };

    // No server confirmed
    QVERIFY(refused(service, QStringList(), tokenOrigin));
    // The token address changed after its server was confirmed
    GeoZoneService moved = service;
    moved.tokenUrl = hostile.url(QStringLiteral("/token"));
    QVERIFY(refused(moved, { tokenOrigin }, hostileOrigin));
    QVERIFY2(!failedSpy.last()[1].toString().contains(tokenOrigin), qPrintable(failedSpy.last()[1].toString()));
    // A second server was added after the first one was confirmed
    GeoZoneService second = service;
    second.sha256Url = hostile.url(QStringLiteral("/ok.sha256"));
    QVERIFY(refused(second, { tokenOrigin }, hostileOrigin));
    QCOMPARE(tokenServer.requestCount, 0);

    // The servers confirmed are those of the service: the secret goes to the token server
    manager.addService(url, service.toVariantMap(), secret, QStringList{ tokenOrigin, hostileOrigin });
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(failedSpy.count(), 3);
    QCOMPARE(tokenServer.requestCount, 1);
    QCOMPARE(hostile.requestCount, 0);
    QCOMPARE(manager.repository()->credentials().secret(url), secret);

    // Callers which don't show the servers confirm all of them or none
    manager.repository()->clear();
    manager.addService(url, moved.toVariantMap(), secret);
    QCOMPARE(failedSpy.count(), 4);
    QCOMPARE(hostile.requestCount, 0);
    manager.addService(url, service.toVariantMap(), secret, true);
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(tokenServer.requestCount, 2);
}

GEOZONE_TEST(GeoZoneManagerTest)
