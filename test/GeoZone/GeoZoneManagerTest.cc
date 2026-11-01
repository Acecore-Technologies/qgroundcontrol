/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneManagerTest.h"
#include "GeoZoneTestHelpers.h"

using namespace GeoZoneTestHelpers;

namespace {

/// A format whose documents name the companion document of each feature by its "detail" property
class DetailFormat : public GeoZoneFormat
{
public:
    GeoZoneParser::Format format() const final { return GeoZoneParser::Format::ED269; }
    QString name() const final { return QStringLiteral("Detail"); }
    int detectionOrder() const final { return 0; }
    bool detect(const QJsonObject &root, const QJsonArray &, GeoZoneParser::Format) const final { return root.contains(QStringLiteral("details")); }
    QList<CompanionRequest> companionRequests(const QUrl &documentUrl, const QJsonObject &, const QJsonArray &features) const final
    {
        QList<CompanionRequest> requests;
        for (const QJsonValue &feature : features) {
            const QString detail = feature.toObject().value(QStringLiteral("detail")).toString();
            requests.append(CompanionRequest{ detail, QStringLiteral("v1"), documentUrl.resolved(QUrl(QStringLiteral("/details/%1").arg(detail))).toString() });
        }
        return requests;
    }
};

/// Captures the messages logged while in scope, with the debug messages of the geozone categories enabled
class ScopedLogCapture
{
public:
    ScopedLogCapture()
    {
        _instance = this;
        QLoggingCategory::setFilterRules(QStringLiteral("qgc.geozone.*.debug=true"));
        _previous = qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
            const QMutexLocker locker(&_instance->_mutex);
            _instance->_messages.append(message);
        });
    }

    ~ScopedLogCapture()
    {
        (void) qInstallMessageHandler(_previous);
        QLoggingCategory::setFilterRules(QString());
        _instance = nullptr;
    }

    Q_DISABLE_COPY_MOVE(ScopedLogCapture)

    QString text() const
    {
        const QMutexLocker locker(&_mutex);
        return _messages.join(QLatin1Char('\n'));
    }

private:
    static inline ScopedLogCapture *_instance = nullptr;
    QtMessageHandler _previous = nullptr;
    mutable QMutex _mutex;
    QStringList _messages;
};

/// @return The content of the files of a directory, to look for secrets
QByteArray _directoryContent(const QString &path)
{
    QByteArray content;
    for (const QString &fileName : QDir(path).entryList(QDir::Files)) {
        QFile file(QDir(path).filePath(fileName));
        if (file.open(QIODevice::ReadOnly)) {
            content += file.readAll();
        }
    }
    return content;
}

} // namespace

void GeoZoneManagerTest::_testManagerLoadPersistClear()
{
    const QTemporaryDir storageDir;
    const QTemporaryDir sourceDir;
    QVERIFY(storageDir.isValid() && sourceDir.isValid());
    const QString ed318File = sourceDir.filePath(QStringLiteral("zones.json"));
    const QString ed269File = sourceDir.filePath(QStringLiteral("other.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), ed318File));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed269_test.json"), ed269File));
    const auto storedFiles = [&storageDir]() {
        return QDir(storageDir.path()).entryList({ QStringLiteral("*.json") }, QDir::Files);
    };
    // Parse caches of the stored copies, see GeoZoneParseCache
    const auto storedCaches = [&storageDir]() {
        return QDir(storageDir.path()).entryList({ QStringLiteral("*.gzc") }, QDir::Files);
    };

    {
        GeoZoneManager manager(storageDir.path());
        manager.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
        QCOMPARE(manager.zoneCount(), 0);
        QCOMPARE(manager.sources()->count(), 0);

        QString errorString;
        QVERIFY2(manager.loadFile(ed318File, errorString), qPrintable(errorString));
        QCOMPARE(manager.zoneCount(), 3);
        QCOMPARE(manager.shapes().count(), 5);
        QCOMPARE(std::count_if(manager.shapes().cbegin(), manager.shapes().cend(), [](const GeoZoneShape *shape) { return shape->isCircle(); }), 1);
        QVERIFY(manager.boundingBox().isValid());

        QCOMPARE(manager.sources()->count(), 1);
        const GeoZoneSource *const ed318Source = manager.sources()->value<GeoZoneSource*>(0);
        QCOMPARE(ed318Source->key(), GeoZoneLoader::fileKey(ed318File));
        QVERIFY(!ed318Source->isFeed());
        QCOMPARE(ed318Source->name(), QStringLiteral("Test GeoZones"));
        QCOMPARE(ed318Source->formatName(), QStringLiteral("ED-318"));
        QCOMPARE(ed318Source->provider(), QStringLiteral("Test Authority"));
        QCOMPARE(ed318Source->skippedCount(), 1);
        QCOMPARE(ed318Source->dataDate(), _utc(QDate(2026, 8, 13), QTime(0, 0)));
        QVERIFY(ed318Source->detailsText().contains(QStringLiteral("issued 2026-08-13")));
        QCOMPARE(manager.zoneList().at(0)->sourceText(), QStringLiteral("Test GeoZones (issued 2026-08-13)"));
        QVERIFY(QFile::exists(QDir(storageDir.path()).filePath(manager.storedFileName(ed318Source->key()))));
        QCOMPARE(storedCaches(), QStringList({ GeoZoneParseCache::path(manager.storedFileName(ed318Source->key())) }));

        // A failed load keeps the current zones
        QVERIFY(!manager.loadFile(sourceDir.filePath(QStringLiteral("missing.json")), errorString));
        QVERIFY(!errorString.isEmpty());
        QCOMPARE(manager.zoneCount(), 3);
        QCOMPARE(manager.sources()->count(), 1);

        QCOMPARE(manager.catalog().zonesInArea(QGeoRectangle(), GeoZonePolicy({ GeoZone::Prohibited })).count(), 1);
        const QGeoRectangle ireland(QGeoCoordinate(52.0, -9.0), QGeoCoordinate(51.0, -8.0));
        const QList<const GeoZone*> zonesInArea = manager.catalog().zonesInArea(ireland, GeoZonePolicy({ GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::USpace }));
        QCOMPARE(zonesInArea.count(), 1);
        QCOMPARE(zonesInArea[0]->name(), QStringLiteral("Control zone"));

        // Another file adds its zones, loading a file with the same name again replaces its zones
        QVERIFY2(manager.loadFile(ed269File, errorString), qPrintable(errorString));
        QCOMPARE(manager.sources()->count(), 2);
        QCOMPARE(manager.zoneCount(), 6);
        const QString previousStoredFile = manager.storedFileName(ed318Source->key());
        QVERIFY2(manager.loadFile(ed318File, errorString), qPrintable(errorString));
        QCOMPARE(manager.sources()->count(), 2);
        QCOMPARE(manager.zoneCount(), 6);
        QVERIFY(manager.storedFileName(ed318Source->key()) != previousStoredFile);
        QCOMPARE(storedFiles().count(), 3);
        QCOMPARE(storedCaches().count(), 2);

        // Zones of sources turned off are left out, loading the file again turns it on
        GeoZoneSource *const ed269Source = manager.sources()->value<GeoZoneSource*>(1);
        ed269Source->setEnabled(false);
        QCOMPARE(manager.zoneCount(), 3);
        QCOMPARE(manager.disabledSourceCount(), 1);
        QVERIFY(std::none_of(manager.zoneList().cbegin(), manager.zoneList().cend(), [&ed269File](const GeoZone *zone) { return zone->sourceKey() == GeoZoneLoader::fileKey(ed269File); }));
        QVERIFY2(manager.loadFile(ed269File, errorString), qPrintable(errorString));
        QVERIFY(ed269Source->enabled());
        QCOMPARE(manager.zoneCount(), 6);
        ed269Source->setEnabled(false);
        QCOMPARE(storedFiles().count(), 3);
    }

    // Unreferenced files are removed on startup
    QFile strayFile(QDir(storageDir.path()).filePath(QStringLiteral("stray.json")));
    QVERIFY(strayFile.open(QIODevice::WriteOnly));
    strayFile.close();

    {
        // Stored copies are reloaded in order on startup
        GeoZoneManager manager(storageDir.path());
        manager.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
        QCOMPARE(manager.sources()->count(), 2);
        // Still turned off
        QVERIFY(!manager.sources()->value<GeoZoneSource*>(1)->enabled());
        QCOMPARE(manager.zoneCount(), 3);
        manager.sources()->value<GeoZoneSource*>(1)->setEnabled(true);
        QCOMPARE(manager.zoneCount(), 6);
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(0)->key(), GeoZoneLoader::fileKey(ed318File));
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(0)->formatName(), QStringLiteral("ED-318"));
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(1)->key(), GeoZoneLoader::fileKey(ed269File));
        QVERIFY(!strayFile.exists());
        QCOMPARE(storedCaches().count(), 2);

        // The parse cache goes with the stored copy
        manager.removeSource(0);
        QCOMPARE(manager.sources()->count(), 1);
        QCOMPARE(manager.zoneCount(), 3);
        QCOMPARE(storedFiles().count(), 2);
        QCOMPARE(storedCaches(), QStringList({ GeoZoneParseCache::path(manager.storedFileName(GeoZoneLoader::fileKey(ed269File))) }));

        manager.clear();
        QCOMPARE(manager.zoneCount(), 0);
        QVERIFY(manager.shapes().isEmpty());
        QVERIFY(storedFiles().isEmpty());
        QVERIFY(storedCaches().isEmpty());
    }

    GeoZoneManager manager(storageDir.path());
    manager.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QCOMPARE(manager.sources()->count(), 0);

    // The single file kept by earlier versions is migrated
    const QTemporaryDir legacyDir;
    QVERIFY(legacyDir.isValid());
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), legacyDir.filePath(QStringLiteral("geozones.json"))));
    QSettings settings;
    settings.setValue(QStringLiteral("GeoZoneManager/sourceName"), QStringLiteral("Legacy zones"));
    {
        GeoZoneManager legacyManager(legacyDir.path());
        legacyManager.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
        QCOMPARE(legacyManager.sources()->count(), 1);
        QCOMPARE(legacyManager.sources()->value<GeoZoneSource*>(0)->key(), QStringLiteral("Legacy zones"));
        QCOMPARE(legacyManager.zoneCount(), 3);
        QVERIFY(!settings.contains(QStringLiteral("GeoZoneManager/sourceName")));
    }
    GeoZoneManager migratedManager(legacyDir.path());
    migratedManager.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QCOMPARE(migratedManager.zoneCount(), 3);

    // Files with the same name in different folders are separate sources
    const QTemporaryDir otherDir;
    QVERIFY(otherDir.isValid());
    const QString sameName = otherDir.filePath(QStringLiteral("zones.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed269_test.json"), sameName));
    QString errorString;
    {
        GeoZoneManager pathManager(storageDir.path());
        QVERIFY2(pathManager.loadFile(ed318File, errorString), qPrintable(errorString));
        QVERIFY2(pathManager.loadFile(sameName, errorString), qPrintable(errorString));
        QCOMPARE(pathManager.sources()->count(), 2);
        QCOMPARE(pathManager.zoneCount(), 6);
        QCOMPARE(pathManager.sources()->value<GeoZoneSource*>(1)->name(), QStringLiteral("Test UASZoneVersion"));
        QVERIFY(QFileInfo(pathManager.sources()->value<GeoZoneSource*>(1)->key()).isAbsolute());
        pathManager.clear();
    }

    // A source stored by name by an earlier version is replaced when its file is loaded again
    {
        GeoZoneManager pathManager(storageDir.path());
        QVERIFY2(pathManager.loadFile(ed318File, errorString), qPrintable(errorString));
    }
    QFile indexFile(QDir(storageDir.path()).filePath(QStringLiteral("sources.json")));
    QVERIFY(indexFile.open(QIODevice::ReadOnly));
    QByteArray index = indexFile.readAll();
    indexFile.close();
    QVERIFY(index.contains(QJsonDocument(QJsonArray{ GeoZoneLoader::fileKey(ed318File) }).toJson(QJsonDocument::Compact).mid(1).chopped(1)));
    index.replace(QJsonDocument(QJsonArray{ GeoZoneLoader::fileKey(ed318File) }).toJson(QJsonDocument::Compact).mid(1).chopped(1), "\"zones.json\"");
    QVERIFY(indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(indexFile.write(index), index.size());
    indexFile.close();
    GeoZoneManager legacyKeyManager(storageDir.path());
    legacyKeyManager.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QCOMPARE(legacyKeyManager.sources()->value<GeoZoneSource*>(0)->key(), QStringLiteral("zones.json"));
    QVERIFY2(legacyKeyManager.loadFile(ed318File, errorString), qPrintable(errorString));
    QCOMPARE(legacyKeyManager.sources()->count(), 1);
    QCOMPARE(legacyKeyManager.sources()->value<GeoZoneSource*>(0)->key(), GeoZoneLoader::fileKey(ed318File));
    QCOMPARE(legacyKeyManager.zoneCount(), 3);
}

void GeoZoneManagerTest::_testManagerStoreFailures()
{
    const QTemporaryDir storageDir;
    const QTemporaryDir sourceDir;
    QVERIFY(storageDir.isValid() && sourceDir.isValid());
    const QString ed318File = sourceDir.filePath(QStringLiteral("zones.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), ed318File));
    const QDir dir(storageDir.path());
    const QString key = GeoZoneLoader::fileKey(ed318File);
    const QString indexPath = dir.filePath(QStringLiteral("sources.json"));
    QString errorString;
    QString storedFile;
    {
        GeoZoneManager manager(storageDir.path());
        QVERIFY2(manager.loadFile(ed318File, errorString), qPrintable(errorString));
        storedFile = manager.storedFileName(key);
        QVERIFY(dir.exists(storedFile));
    }

    // A copy which can't be written keeps the previous one, which is still loaded at the next start
    const QFile::Permissions permissions = QFile::permissions(storageDir.path());
    QVERIFY(QFile::setPermissions(storageDir.path(), QFile::ReadOwner | QFile::ExeOwner));
    if (!QFileInfo(storageDir.path()).isWritable()) {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy storageSpy(&manager, &GeoZoneManager::storageFailed);
        manager.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
        QCOMPARE(manager.zoneCount(), 3);
        QVERIFY2(manager.loadFile(ed318File, errorString), qPrintable(errorString));
        QCOMPARE(storageSpy.count(), 1);
        QCOMPARE(manager.storedFileName(key), storedFile);
    }
    QVERIFY(QFile::setPermissions(storageDir.path(), permissions));
    QVERIFY(dir.exists(storedFile));

    // An index from a newer version is kept as is and its sources aren't touched
    QFile indexFile(indexPath);
    QVERIFY(indexFile.open(QIODevice::ReadOnly));
    QJsonObject index = QJsonDocument::fromJson(indexFile.readAll()).object();
    indexFile.close();
    index.insert(QStringLiteral("version"), 99);
    const QByteArray newerIndex = QJsonDocument(index).toJson();
    QVERIFY(indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(indexFile.write(newerIndex), newerIndex.size());
    indexFile.close();
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy storageSpy(&manager, &GeoZoneManager::storageFailed);
        manager.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
        QCOMPARE(storageSpy.count(), 1);
        QCOMPARE(manager.sources()->count(), 0);
        QVERIFY2(manager.loadFile(ed318File, errorString), qPrintable(errorString));
        QVERIFY(manager.storedFileName(key).isEmpty());
        manager.clear();
    }
    QVERIFY(indexFile.open(QIODevice::ReadOnly));
    QCOMPARE(indexFile.readAll(), newerIndex);
    indexFile.close();
    QVERIFY(dir.exists(storedFile));
    QCOMPARE(dir.entryList({ QStringLiteral("*.json") }, QDir::Files).count(), 2);

    // An unreadable index is moved to a backup folder with the stored copies instead of losing them
    QVERIFY(indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(indexFile.write(R"({ "version": 1, "sources": [)") > 0);
    indexFile.close();
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy storageSpy(&manager, &GeoZoneManager::storageFailed);
        manager.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
        QCOMPARE(storageSpy.count(), 1);
        QCOMPARE(manager.sources()->count(), 0);
    }
    QVERIFY(dir.entryList({ QStringLiteral("*.json") }, QDir::Files).isEmpty());
    const QStringList backups = dir.entryList({ QStringLiteral("backup-*") }, QDir::Dirs);
    QCOMPARE(backups.count(), 1);
    const QDir backupDir(dir.filePath(backups.first()));
    QVERIFY(backupDir.exists(storedFile));
    QVERIFY(backupDir.exists(QStringLiteral("sources.json")));
}

void GeoZoneManagerTest::_testManagerBackgroundLoad()
{
    const ScopedEnvironment environment;
    const QTemporaryDir storageDir;
    const QTemporaryDir sourceDir;
    QVERIFY(storageDir.isValid() && sourceDir.isValid());
    const QString ed318File = sourceDir.filePath(QStringLiteral("zones.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), ed318File));
    QVERIFY(QDir(sourceDir.path()).mkdir(QStringLiteral("newer")));
    const QString ed269File = sourceDir.filePath(QStringLiteral("newer/zones.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed269_test.json"), ed269File));

    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    manager.addFile(ed318File);
    QVERIFY(manager.busy());
    QVERIFY(addedSpy.wait(10000));
    QVERIFY(!manager.busy());
    QCOMPARE(manager.zoneCount(), 3);

    // Only the most recent load of a file is applied, a file with the same name in another folder is another source
    addedSpy.clear();
    manager.addFile(ed318File);
    manager.addFile(ed318File);
    manager.addFile(ed269File);
    QTRY_VERIFY(!manager.busy());
    // The unchanged file isn't added again
    QCOMPARE(addedSpy.count(), 1);
    QCOMPARE(manager.sources()->count(), 2);
    QCOMPARE(manager.sources()->value<GeoZoneSource*>(1)->formatName(), QStringLiteral("ED-269"));

    // Removing all sources drops loads in progress
    manager.addFile(ed318File);
    manager.clear();
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(manager.sources()->count(), 0);

    // Feeds keep their URL as key and can be refreshed
    const QString feedUrl = QUrl::fromLocalFile(ed318File).toString();
    addedSpy.clear();
    manager.addUrl(feedUrl);
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.busy());
    GeoZoneSource *const feed = manager.source(feedUrl);
    QVERIFY(feed);
    QVERIFY(feed->isFeed());
    QCOMPARE(feed->url(), feedUrl);
    QCOMPARE(manager.zoneCount(), 3);
    // The user's file is read in place, never removed
    QVERIFY(QFile::exists(ed318File));
    const QDateTime now = environment.now();
    QVERIFY(!manager.repository()->scheduler()->isDue(*feed, now));
    QVERIFY(manager.repository()->scheduler()->isDue(*feed, now.addSecs(25 * 3600)));

    // A failed refresh keeps the previous data
    QVERIFY(QFile::remove(ed318File));
    manager.refreshFeeds();
    QVERIFY(feed->busy());
    QTRY_VERIFY(!manager.busy());
    QVERIFY(!feed->busy());
    QVERIFY(!feed->errorString().isEmpty());
    QCOMPARE(manager.zoneCount(), 3);

    // Stored sources load in the background in their original order
    GeoZoneManager restarted(storageDir.path());
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Background);
    QCOMPARE(restarted.sources()->count(), 1);
    QVERIFY(restarted.busy());
    QTRY_VERIFY(!restarted.busy());
    QCOMPARE(restarted.zoneCount(), 3);
    QVERIFY(restarted.source(feedUrl));
    QVERIFY(restarted.source(feedUrl)->isFeed());
}

void GeoZoneManagerTest::_testFeedCompanions()
{
    // An FAA TFR list with the detail of one of its NOTAMs
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QFile fixture(QStringLiteral(":/unittest/faa_tfr_test.json"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    server.files.insert("/geoserver/tfr.json", fixture.readAll());
    server.files.insert("/download/detail_6_4240.xml", R"(<XNOTAM-Update><Group><Add><Not>
        <dateEffective>2026-03-06T14:00:00</dateEffective><dateExpire>2026-03-06T22:00:00</dateExpire>
        <TfrNot><TFRAreaGroup><aseTFRArea><codeDistVerUpper>ALT</codeDistVerUpper><valDistVerUpper>3000</valDistVerUpper>
        <uomDistVerUpper>FT</uomDistVerUpper><codeDistVerLower>HEI</codeDistVerLower><valDistVerLower>0</valDistVerLower>
        <uomDistVerLower>FT</uomDistVerLower></aseTFRArea></TFRAreaGroup></TfrNot></Not></Add></Group></XNOTAM-Update>)");
    const QString url = server.url(QStringLiteral("/geoserver/tfr.json"));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto detailRequests = [&server]() { return server.requestedPaths.count("/download/detail_6_4240.xml"); };
    const auto scheduled = [](const GeoZoneManager &manager) {
        const QList<GeoZone*> &zones = manager.zoneList();
        return std::count_if(zones.cbegin(), zones.cend(), [](const GeoZone *zone) { return !zone->data().applicability.isEmpty(); });
    };

    {
        // The details are downloaded with the list and kept in the stored copy
        GeoZoneManager manager(dir.path());
        manager.addUrl(url);
        QTRY_VERIFY_WITH_TIMEOUT(manager.zoneCount() > 0 && !manager.busy(), 10000);
        QCOMPARE(scheduled(manager), 1);
        QCOMPARE(detailRequests(), 1);

        // Unchanged NOTAMs aren't downloaded again
        manager.refreshFeeds();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 10000);
        QCOMPARE(server.requestedPaths.count("/geoserver/tfr.json"), 2);
        QCOMPARE(detailRequests(), 1);
        QCOMPARE(scheduled(manager), 1);
    }

    // After a restart the stored details apply and are reused
    GeoZoneManager restarted(dir.path());
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QCOMPARE(scheduled(restarted), 1);
    restarted.refreshFeeds();
    QTRY_VERIFY_WITH_TIMEOUT(!restarted.busy(), 10000);
    QCOMPARE(detailRequests(), 1);
    QCOMPARE(scheduled(restarted), 1);
}

void GeoZoneManagerTest::_testFeedFetchFormatCompanions()
{
    // The fetch asks the format recognizing the document which companions it needs
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/list", R"({ "details": true, "features": [ { "detail": "a" }, { "detail": "b" } ] })");
    server.files.insert("/details/a", "detail of a");
    server.files.insert("/details/b", "detail of b");
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneDownloader downloader;
    downloader.setDirectory(dir.path());
    const DetailFormat format;

    GeoZoneFeedFetch::Request request;
    request.url = server.url(QStringLiteral("/list"));
    request.formats = { &format };
    // A companion of the stored copy with the current version isn't downloaded again
    request.companions.insert(QStringLiteral("b"), GeoZoneParser::Companion{ QStringLiteral("v1"), "stored detail of b" });
    GeoZoneFeedFetch fetch(&downloader, request);
    std::optional<GeoZoneFeedFetch::Result> result;
    (void) connect(&fetch, &GeoZoneFeedFetch::finished, &fetch, [&result](const GeoZoneFeedFetch::Result &fetchResult) { result = fetchResult; });
    fetch.start();
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 10000);
    QCOMPARE(result->status, GeoZoneFeedFetch::Result::Status::Ready);
    QCOMPARE(fetch.stage(), GeoZoneFeedFetch::Stage::Finished);
    QCOMPARE(server.requestedPaths, (QList<QByteArray>{ "/list", "/details/a" }));

    // The document is kept with its companions
    QFile file(result->filePath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject companions = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("companions")).toObject();
    QCOMPARE(companions.value(QStringLiteral("a")).toObject().value(QStringLiteral("data")).toString(), QStringLiteral("detail of a"));
    QCOMPARE(companions.value(QStringLiteral("b")).toObject().value(QStringLiteral("data")).toString(), QStringLiteral("stored detail of b"));
    file.close();

    // Documents of formats without companions are loaded as downloaded
    request.formats = GeoZoneFormat::formats();
    GeoZoneFeedFetch plainFetch(&downloader, request);
    result.reset();
    (void) connect(&plainFetch, &GeoZoneFeedFetch::finished, &plainFetch, [&result](const GeoZoneFeedFetch::Result &fetchResult) { result = fetchResult; });
    plainFetch.start();
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 10000);
    QCOMPARE(result->status, GeoZoneFeedFetch::Result::Status::Ready);
    QCOMPARE(server.requestedPaths.count(), 3);
    QCOMPARE(GeoZoneDownloader::readFile(result->filePath, 1000), server.files.value("/list"));
}

void GeoZoneManagerTest::_testFeedCancellation()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/slow", server.body);
    server.delays.insert("/slow", 300);
    const QString url = server.url(QStringLiteral("/slow"));
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    const QDir storage(storageDir.path());
    const QDir downloads(storage.filePath(QStringLiteral("downloads")));
    const auto waitPastResponses = []() { QTest::qWait(500); };

    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);
    manager.addUrl(url);
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(manager.zoneCount(), 3);

    // Removing a source while it is refreshed aborts the download, the response arriving later is dropped
    int requests = server.requestCount;
    manager.refreshFeeds();
    QTRY_COMPARE(server.requestCount, requests + 1);
    QVERIFY(manager.busy());
    manager.removeSource(0);
    QVERIFY(!manager.busy());
    waitPastResponses();
    QCOMPARE(manager.sources()->count(), 0);
    QCOMPARE(manager.zoneCount(), 0);
    QCOMPARE(addedSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(storage.entryList({ QStringLiteral("*.json") }, QDir::Files).isEmpty());
    QVERIFY(downloads.entryList(QDir::Files).isEmpty());

    // Adding the feed again while it downloads replaces the download, only the last one is loaded
    requests = server.requestCount;
    manager.addUrl(url);
    QTRY_COMPARE(server.requestCount, requests + 1);
    manager.addUrl(url);
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.busy());
    waitPastResponses();
    QCOMPARE(addedSpy.count(), 2);
    QCOMPARE(manager.sources()->count(), 1);
    QCOMPARE(manager.zoneCount(), 3);
    QCOMPARE(storage.entryList({ QStringLiteral("*.json") }, QDir::Files).count(), 2);
    QVERIFY(downloads.entryList(QDir::Files).isEmpty());

    // Closing while a feed downloads drops it
    const QTemporaryDir closingDir;
    QVERIFY(closingDir.isValid());
    {
        GeoZoneManager closing(closingDir.path());
        requests = server.requestCount;
        closing.addUrl(url);
        QTRY_COMPARE(server.requestCount, requests + 1);
    }
    waitPastResponses();
    QVERIFY(QDir(closingDir.path()).entryList({ QStringLiteral("*.json") }, QDir::Files).isEmpty());

    // A load canceled before it runs neither parses nor stores the download
    const QString downloadPath = storage.filePath(QStringLiteral("download.part"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), downloadPath));
    GeoZoneLoader::Job job = GeoZoneLoader::feedJob(url);
    job.sourcePath = downloadPath;
    job.downloaded = true;
    job.cancelToken.cancel();
    const GeoZoneLoader::Result loadResult = GeoZoneLoader::run(job, GeoZoneStore(storageDir.path()));
    QVERIFY(loadResult.cancelled);
    QVERIFY(!loadResult.stored);
    QCOMPARE(loadResult.result.format, GeoZoneParser::Format::Unknown);
    QVERIFY(!QFile::exists(downloadPath));
    QVERIFY(!storage.exists(job.storedFileName));
}

void GeoZoneManagerTest::_testRefreshScheduler()
{
    ScopedEnvironment environment;

    // How long a response stays current, from its headers
    QCOMPARE(GeoZoneDownloader::maxAgeSecs("public, max-age=600", QByteArray(), QByteArray(), QByteArray()), 600);
    QCOMPARE(GeoZoneDownloader::maxAgeSecs("max-age=600", "Sat, 31 Oct 2026 12:00:00 GMT", QByteArray(), "100"), 500);
    QCOMPARE(GeoZoneDownloader::maxAgeSecs(QByteArray(), "Sat, 31 Oct 2026 12:00:00 GMT", "Sat, 31 Oct 2026 11:00:00 GMT", QByteArray()), 3600);
    QCOMPARE(GeoZoneDownloader::maxAgeSecs("no-cache", QByteArray(), QByteArray(), QByteArray()), -1);
    QCOMPARE(GeoZoneDownloader::maxAgeSecs("max-age=0", "Sat, 31 Oct 2026 12:00:00 GMT", "Sat, 31 Oct 2026 11:00:00 GMT", QByteArray()), -1);
    QCOMPARE(GeoZoneDownloader::maxAgeSecs(QByteArray(), "0", QByteArray(), QByteArray()), -1);

    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/zones", server.body);
    server.etags.insert("/zones", "\"v1\"");
    server.headers.insert("/zones", "Cache-Control: max-age=3600\r\n");
    const QString url = server.url(QStringLiteral("/zones"));
    const QTemporaryDir storageDir;
    const QTemporaryDir sourceDir;
    QVERIFY(storageDir.isValid() && sourceDir.isValid());
    GeoZoneManager manager(storageDir.path());
    const auto settle = [&manager]() { return QTest::qWaitFor([&manager]() { return !manager.busy(); }, 10000); };
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    manager.addUrl(url);
    QVERIFY(addedSpy.wait(10000));
    QVERIFY(settle());
    GeoZoneSource *const feed = manager.source(url);
    QVERIFY(feed);
    GeoZoneRefreshScheduler *const scheduler = manager.repository()->scheduler();
    QSignalSpy dueSpy(scheduler, &GeoZoneRefreshScheduler::refreshDue);

    // The server keeps its data current for an hour, sooner than the refresh interval of a day
    QCOMPARE(scheduler->refreshIntervalSecs(*feed), 3600);
    environment.addSecs(3599);
    scheduler->check();
    QCOMPARE(dueSpy.count(), 0);
    environment.addSecs(1);
    scheduler->check();
    QCOMPARE(dueSpy.count(), 1);
    QVERIFY(feed->busy());
    QVERIFY(settle());
    QCOMPARE(server.requestCount, 2);
    QVERIFY(feed->errorString().isEmpty());
    // Confirmed by the server, it is current for another hour
    scheduler->check();
    QCOMPARE(dueSpy.count(), 1);

    // A server can't make the refreshes more frequent than every 5 minutes, without a lifetime the settings apply
    server.headers.insert("/zones", "Cache-Control: max-age=10\r\n");
    environment.addSecs(3600);
    scheduler->check();
    QVERIFY(settle());
    QCOMPARE(scheduler->refreshIntervalSecs(*feed), GeoZoneRefreshScheduler::minRefreshSecs);
    server.headers.remove("/zones");
    environment.addSecs(GeoZoneRefreshScheduler::minRefreshSecs);
    scheduler->check();
    QVERIFY(settle());
    QCOMPARE(dueSpy.count(), 3);
    QCOMPARE(scheduler->refreshIntervalSecs(*feed), 24 * 3600);

    // A failed refresh keeps the zones and is retried after 5 minutes, then twice as long, within 20 %
    server.etags.remove("/zones");
    server.files.remove("/zones");
    environment.addSecs(24 * 3600);
    scheduler->check();
    QVERIFY(settle());
    QCOMPARE(dueSpy.count(), 4);
    QVERIFY(!feed->errorString().isEmpty());
    QCOMPARE(manager.zoneCount(), 3);
    QDateTime retryAt = scheduler->retryAt(feed->key());
    QVERIFY(environment.now().secsTo(retryAt) >= GeoZoneRefreshScheduler::minRetrySecs * 0.8);
    QVERIFY(environment.now().secsTo(retryAt) <= GeoZoneRefreshScheduler::minRetrySecs * 1.2);
    environment.setNow(retryAt.addSecs(-1));
    scheduler->check();
    QCOMPARE(dueSpy.count(), 4);
    environment.setNow(retryAt);
    scheduler->check();
    QVERIFY(settle());
    QCOMPARE(dueSpy.count(), 5);
    retryAt = scheduler->retryAt(feed->key());
    QVERIFY(environment.now().secsTo(retryAt) >= GeoZoneRefreshScheduler::minRetrySecs * 2 * 0.8);
    QVERIFY(environment.now().secsTo(retryAt) <= GeoZoneRefreshScheduler::minRetrySecs * 2 * 1.2);

    // A successful retry ends the retries
    server.files.insert("/zones", server.body);
    server.headers.insert("/zones", "Cache-Control: max-age=7200\r\n");
    environment.setNow(retryAt);
    scheduler->check();
    QVERIFY(settle());
    QCOMPARE(dueSpy.count(), 6);
    QVERIFY(feed->errorString().isEmpty());
    QVERIFY(!scheduler->retryAt(feed->key()).isValid());
    QCOMPARE(scheduler->nextRefresh(*feed), environment.now().addSecs(7200));

    // Files are only reloaded by the user
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(sourceDir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47, 8), 300) }), errorString), qPrintable(errorString));
    const GeoZoneSource *const file = manager.sources()->value<GeoZoneSource*>(1);
    QVERIFY(!scheduler->nextRefresh(*file).isValid());
    environment.addSecs(365 * 24 * 3600);
    dueSpy.clear();
    scheduler->check();
    QCOMPARE(dueSpy.count(), 1);
    QCOMPARE(dueSpy.first()[0].value<GeoZoneSource*>(), feed);
    QVERIFY(settle());

    // The lifetime given by the server is kept across restarts
    GeoZoneManager restarted(storageDir.path());
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QVERIFY(restarted.source(url));
    QCOMPARE(restarted.repository()->scheduler()->refreshIntervalSecs(*restarted.source(url)), 7200);
}

void GeoZoneManagerTest::_testReloadChanges()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 0), 300),
        _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("C"), QStringLiteral("CONDITIONAL"), center.atDistanceAndAzimuth(5000, 90), 300),
    }), errorString), qPrintable(errorString));
    const GeoZoneSource *const source = manager.sources()->value<GeoZoneSource*>(0);
    QVERIFY(source->changesText().isEmpty());

    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 1);

    // Reloading lists the zones added, removed and changed, and changed zones alert again
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 0), 300),
        _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), center, 400),
        _circleFeature(QStringLiteral("D"), QStringLiteral("CONDITIONAL"), center.atDistanceAndAzimuth(5000, 180), 300),
    }), errorString), qPrintable(errorString));
    QVERIFY2(source->changesText().contains(QStringLiteral("added D; removed C; changed B")), qPrintable(source->changesText()));
    QCOMPARE(source->changedZoneKeys().count(), 1);
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 2);

    // Unchanged zones keep their alert state
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 0), 300),
        _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), center, 400),
    }), errorString), qPrintable(errorString));
    QVERIFY(source->changedZoneKeys().isEmpty());
    QVERIFY(source->changesText().contains(QStringLiteral("removed D")));
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 2);
}

void GeoZoneManagerTest::_testFeedNotModified()
{
    ScopedEnvironment environment;
    QFile fixture(QStringLiteral(":/unittest/ed318_test.json"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    QByteArray body = fixture.readAll();

    // Serves the feed with an ETag and answers matching conditional requests with 304 Not Modified
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int requestCount = 0;
    int notModifiedCount = 0;
    (void) connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (QTcpSocket *const socket = server.nextPendingConnection()) {
            (void) connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) {
                    return;
                }
                requestCount++;
                // Header names are case insensitive, Qt sends them in lower case
                if (request.toLower().contains("if-none-match: \"v1\"")) {
                    notModifiedCount++;
                    (void) socket->write("HTTP/1.1 304 Not Modified\r\nETag: \"v1\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else {
                    (void) socket->write(QByteArray("HTTP/1.1 200 OK\r\nETag: \"v1\"\r\nLast-Modified: Sat, 31 Oct 2026 10:00:00 GMT\r\n"
                                                    "Content-Type: application/json\r\nConnection: close\r\nContent-Length: ") +
                                         QByteArray::number(body.size()) + "\r\n\r\n" + body);
                }
                socket->disconnectFromHost();
            });
            (void) connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    const QString url = QStringLiteral("http://127.0.0.1:%1/zones.json").arg(server.serverPort());

    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        manager.addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.busy());
        GeoZoneSource *const feed = manager.source(url);
        QVERIFY(feed);
        QCOMPARE(feed->etag(), QStringLiteral("\"v1\""));
        QCOMPARE(feed->lastModified(), QStringLiteral("Sat, 31 Oct 2026 10:00:00 GMT"));
        QCOMPARE(requestCount, 1);

        // An unchanged feed is not downloaded again but counts as current, keeping the dates of its data
        QSignalSpy aboutToChangeSpy(&manager, &GeoZoneManager::zonesAboutToChange);
        const QDateTime loadedAt = feed->loadedAt();
        const QDateTime dataDate = feed->dataDate();
        QCOMPARE(feed->checkedAt(), loadedAt);
        environment.addSecs(60);
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(notModifiedCount, 1);
        QCOMPARE(manager.zoneCount(), 3);
        QCOMPARE(feed->checkedAt(), environment.now());
        QCOMPARE(feed->loadedAt(), loadedAt);
        QCOMPARE(feed->fileDate(), loadedAt);
        QCOMPARE(feed->dataDate(), dataDate);
        QVERIFY(feed->errorString().isEmpty());

        // A server without validators sends the same zones again, which are kept instead of reloaded
        QCOMPARE(aboutToChangeSpy.count(), 0);
        feed->setValidators(QString(), QString());
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(requestCount, 3);
        QCOMPARE(aboutToChangeSpy.count(), 0);
        QCOMPARE(manager.zoneCount(), 3);

        // Republished metadata is applied to the kept zones
        QJsonObject root = QJsonDocument::fromJson(body).object();
        QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
        QVERIFY(!metadata.isEmpty());
        metadata.insert(QStringLiteral("validTo"), QStringLiteral("2026-09-01T00:00:00Z"));
        metadata.insert(QStringLiteral("issued"), QStringLiteral("2026-08-20T00:00:00Z"));
        root.insert(QStringLiteral("metadata"), metadata);
        body = QJsonDocument(root).toJson();
        feed->setValidators(QString(), QString());
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(aboutToChangeSpy.count(), 0);
        QCOMPARE(feed->validTo(), _utc(QDate(2026, 9, 1), QTime(0, 0)));
        QCOMPARE(manager.zoneList().at(0)->sourceText(), QStringLiteral("Test GeoZones (issued 2026-08-20)"));

        // Changed zones are reloaded, without signaling an added source which would move the maps
        QFile changed(QStringLiteral(":/unittest/ed269_test.json"));
        QVERIFY(changed.open(QIODevice::ReadOnly));
        body = changed.readAll();
        feed->setValidators(QString(), QString());
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(aboutToChangeSpy.count(), 1);
        QCOMPARE(manager.source(url)->formatName(), QStringLiteral("ED-269"));
        QCOMPARE(addedSpy.count(), 1);
    }

    // The validators are kept across restarts
    GeoZoneManager restarted(storageDir.path());
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QVERIFY(restarted.source(url));
    QCOMPARE(restarted.source(url)->etag(), QStringLiteral("\"v1\""));
    const QDateTime downloadedAt = restarted.source(url)->loadedAt();
    environment.addSecs(60);
    restarted.refreshFeeds();
    QTRY_VERIFY(!restarted.busy());
    QCOMPARE(notModifiedCount, 2);
    QCOMPARE(restarted.zoneCount(), 3);

    // When the server confirmed the stored copy is kept apart from when it was downloaded
    GeoZoneManager confirmed(storageDir.path());
    confirmed.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QVERIFY(confirmed.source(url));
    QCOMPARE(confirmed.source(url)->loadedAt(), downloadedAt);
    QCOMPARE(confirmed.source(url)->checkedAt(), environment.now());
    QVERIFY(confirmed.source(url)->detailsText().contains(QStringLiteral("checked")));
    QCOMPARE(confirmed.repository()->scheduler()->nextRefresh(*confirmed.source(url)), environment.now().addSecs(24 * 3600));
}

void GeoZoneManagerTest::_testSourceValidity()
{
    const ScopedEnvironment environment;
    GeoZoneSource source(QStringLiteral("zones.json"), QString());
    GeoZoneParser::Result result;
    result.format = GeoZoneParser::Format::ED269;
    const QDateTime now = environment.now();

    // Without an issue date the file date counts
    source.setResult(result, now, now.addDays(-10));
    QVERIFY(source.validityText().isEmpty());
    source.setResult(result, now, now.addDays(-30));
    QCOMPARE(source.validityText(), QStringLiteral("Data is 30 days old"));

    result.issued = now.addDays(-1);
    source.setResult(result, now, now.addDays(-30));
    QVERIFY(source.validityText().isEmpty());

    result.validTo = now.addDays(-2);
    source.setResult(result, now, now);
    QVERIFY(source.validityText().startsWith(QStringLiteral("Expired on")));

    result.validTo = QDateTime();
    result.validFrom = now.addDays(2);
    source.setResult(result, now, now);
    QVERIFY(source.validityText().startsWith(QStringLiteral("Not valid until")));
    source.updateValidity(now.addDays(3));
    QVERIFY(source.validityText().isEmpty());

    // Without an issue date, downloaded data is as old as the server last confirmed it
    GeoZoneSource download(QStringLiteral("https://example.com/zones.json"), QStringLiteral("https://example.com/zones.json"));
    result = GeoZoneParser::Result();
    result.format = GeoZoneParser::Format::ED269;
    download.setResult(result, now.addDays(-30), now.addDays(-30));
    QCOMPARE(download.validityText(), QStringLiteral("Data is 30 days old"));
    download.setCheckedAt(now.addDays(-1));
    QVERIFY(download.validityText().isEmpty());
    QCOMPARE(download.dataDate(), now.addDays(-30));

    // The manager summarizes outdated sources
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.path());
    QString errorString;
    QVERIFY2(manager.loadFile(QStringLiteral(":/unittest/ed318_test.json"), errorString), qPrintable(errorString));
    QVERIFY(manager.validityWarning().startsWith(QStringLiteral("Test GeoZones: Data is")));

    // Failed refreshes of a feed are retried after 5 minutes, then twice as long after each failure, within 20 %
    GeoZoneRefreshScheduler scheduler;
    GeoZoneSource feed(QStringLiteral("https://example.com/zones.json"), QStringLiteral("https://example.com/zones.json"));
    feed.setStoredDates(now, now);
    QVERIFY(!scheduler.isDue(feed, now.addSecs(60)));
    scheduler.refreshFailed(feed);
    QVERIFY(!scheduler.isDue(feed, now.addSecs(GeoZoneRefreshScheduler::minRetrySecs * 0.79)));
    QVERIFY(scheduler.isDue(feed, now.addSecs(GeoZoneRefreshScheduler::minRetrySecs * 1.21)));
    for (int failure = 2; failure <= 3; failure++) {
        scheduler.refreshFailed(feed);
    }
    QVERIFY(!scheduler.isDue(feed, now.addSecs(GeoZoneRefreshScheduler::minRetrySecs * 4 * 0.79)));
    QVERIFY(scheduler.isDue(feed, now.addSecs(GeoZoneRefreshScheduler::minRetrySecs * 4 * 1.21)));
    // Up to the maximum, or the refresh interval when shorter
    for (int failure = 4; failure <= 20; failure++) {
        scheduler.refreshFailed(feed);
    }
    QVERIFY(scheduler.isDue(feed, now.addSecs(GeoZoneRefreshScheduler::maxRetrySecs * 1.21)));
    // A successful refresh follows the refresh interval again
    scheduler.refreshSucceeded(feed.key(), -1);
    QVERIFY(!scheduler.retryAt(feed.key()).isValid());
    QVERIFY(!scheduler.isDue(feed, now.addSecs(60)));
}

void GeoZoneManagerTest::_testDownloadSizeLimit()
{
    // Serves 4000 bytes, announcing the size or not
    const QByteArray body(4000, ' ');
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    (void) connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (QTcpSocket *const socket = server.nextPendingConnection()) {
            (void) connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) {
                    return;
                }
                const QByteArray length = request.startsWith("GET /sized") ? (QByteArray("Content-Length: ") + QByteArray::number(body.size()) + "\r\n") : QByteArray();
                (void) socket->write(QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n") + length + "\r\n" + body);
                socket->disconnectFromHost();
            });
            (void) connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    GeoZoneDownloader downloader;
    const auto download = [&downloader, &server](const QString &path) {
        GeoZoneDownloader::Result result;
        bool done = false;
        downloader.get(QStringLiteral("http://127.0.0.1:%1/%2").arg(server.serverPort()).arg(path), QString(), QString(),
                       [&result, &done](const GeoZoneDownloader::Result &downloadResult) { result = downloadResult; done = true; });
        if (!QTest::qWaitFor([&done]() { return done; }, 10000)) {
            result.errorString = QStringLiteral("timeout");
        }
        return result;
    };

    GeoZoneDownloader::Result result = download(QStringLiteral("sized"));
    QCOMPARE(result.status, GeoZoneDownloader::Result::Status::Ok);
    QCOMPARE(QFileInfo(result.filePath).size(), body.size());
    QCOMPARE(GeoZoneDownloader::readFile(result.filePath, body.size()), body);

    downloader.setMaxBytes(1000);
    for (const QString &path : { QStringLiteral("sized"), QStringLiteral("unsized") }) {
        result = download(path);
        QCOMPARE(result.status, GeoZoneDownloader::Result::Status::Failed);
        QVERIFY2(result.errorString.startsWith(QStringLiteral("The download is larger than")), qPrintable(result.errorString));
        QVERIFY(result.filePath.isEmpty());
    }
    QCOMPARE(downloader.count(), 0);
}

void GeoZoneManagerTest::_testDownloadFailures()
{
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    GeoZoneDownloader downloader;
    downloader.setTimeout(std::chrono::milliseconds(500));
    const auto download = [&downloader, &server](const QString &path) {
        GeoZoneDownloader::Result result;
        bool done = false;
        downloader.get(server.url(path), QString(), QString(), [&result, &done](const GeoZoneDownloader::Result &downloadResult) { result = downloadResult; done = true; });
        if (!QTest::qWaitFor([&done]() { return done; }, 10000)) {
            result.errorString = QStringLiteral("no callback");
        }
        return result;
    };

    GeoZoneDownloader::Result result = download(QStringLiteral("/missing"));
    QCOMPARE(result.status, GeoZoneDownloader::Result::Status::Failed);
    QVERIFY2(result.errorString.contains(QStringLiteral("Not Found")), qPrintable(result.errorString));
    result = download(QStringLiteral("/broken"));
    QCOMPARE(result.status, GeoZoneDownloader::Result::Status::Failed);
    QVERIFY2(result.errorString.contains(QStringLiteral("Internal Server Error")), qPrintable(result.errorString));
    result = download(QStringLiteral("/empty"));
    QCOMPARE(result.errorString, QStringLiteral("No data was received."));
    result = download(QStringLiteral("/hang"));
    QCOMPARE(result.errorString, QStringLiteral("The server didn't respond within 1 second(s)."));
    QCOMPARE(downloader.count(), 0);

    // Adding a feed which fails reports it and adds nothing
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    GeoZoneManager manager(storageDir.path());
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    manager.addUrl(server.url(QStringLiteral("/missing")));
    QVERIFY(failedSpy.wait(10000));
    QCOMPARE(failedSpy.last()[0].toString(), server.url(QStringLiteral("/missing")));
    QVERIFY(failedSpy.last()[1].toString().contains(QStringLiteral("Not Found")));
    QCOMPARE(manager.sources()->count(), 0);
    manager.addUrl(QStringLiteral("ftp://example.com/zones.json"));
    QCOMPARE(failedSpy.count(), 2);

    // A refresh which fails keeps the zones and shows the error on the source
    manager.addUrl(server.url(QStringLiteral("/ok")));
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.busy());
    GeoZoneSource *const feed = manager.source(server.url(QStringLiteral("/ok")));
    QVERIFY(feed);
    server.okPathStatus = "503 Service Unavailable";
    manager.refreshFeeds();
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(failedSpy.count(), 3);
    QVERIFY(feed->errorString().contains(QStringLiteral("Service Unavailable")));
    QCOMPARE(manager.zoneCount(), 3);

    // The next successful refresh clears the error
    server.okPathStatus = "200 OK";
    manager.refreshFeeds();
    QTRY_VERIFY(!manager.busy());
    QVERIFY(feed->errorString().isEmpty());

    // Downloads become the stored copies, without being left in the download directory
    const QString downloadDir = QDir(manager.storageDir()).filePath(QStringLiteral("downloads"));
    QCOMPARE(QDir(downloadDir).entryList(QDir::Files).count(), 0);
    QFile stored(QDir(manager.storageDir()).filePath(manager.storedFileName(feed->key())));
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QCOMPARE(stored.readAll(), server.body);

    // Removing a source cancels its download
    const int requests = server.requestCount;
    server.okPathStatus = "200 OK";
    manager.addUrl(server.url(QStringLiteral("/hang")));
    QTRY_COMPARE(server.requestCount, requests + 1);
    QVERIFY(manager.busy());
    manager.clear();
    QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 1000);
    QCOMPARE(manager.sources()->count(), 0);
    QCOMPARE(failedSpy.count(), 3);
    QCOMPARE(QDir(downloadDir).entryList(QDir::Files).count(), 0);
}

void GeoZoneManagerTest::_testDownloadRedirects()
{
    TestFeedServer server;
    TestFeedServer otherServer;
    QVERIFY(server.listen(QHostAddress::LocalHost) && otherServer.listen(QHostAddress::LocalHost));
    server.redirects.insert("/old", "/moved");
    server.redirects.insert("/moved", server.url(QStringLiteral("/new")).toUtf8());
    server.files.insert("/new", server.body);
    server.redirects.insert("/away", otherServer.url(QStringLiteral("/data")).toUtf8());
    otherServer.files.insert("/data", server.body);
    server.redirects.insert("/loop", "/loop");
    server.redirects.insert("/form", "/new");

    GeoZoneDownloader downloader;
    GeoZoneDownloader::Options options;
    options.headers.append(GeoZoneDownloader::Header("Accept", "application/json"));
    options.authHeaders.append(GeoZoneDownloader::Header("X-Api-Key", "s3cret key"));
    options.secrets.append(QStringLiteral("s3cret key"));
    const auto download = [&downloader](const QString &url, const GeoZoneDownloader::Options &requestOptions) {
        GeoZoneDownloader::Result result;
        bool done = false;
        downloader.get(url, QString(), QString(), [&result, &done](const GeoZoneDownloader::Result &downloadResult) { result = downloadResult; done = true; }, QString(), requestOptions);
        if (!QTest::qWaitFor([&done]() { return done; }, 10000)) {
            result.errorString = QStringLiteral("no callback");
        }
        if (!result.filePath.isEmpty()) {
            (void) QFile::remove(result.filePath);
        }
        return result;
    };

    // The redirects are followed and recorded, the credentials go to the same server
    GeoZoneDownloader::Result result = download(server.url(QStringLiteral("/old?key=s3cret%20key")), options);
    QVERIFY2(result.status == GeoZoneDownloader::Result::Status::Ok, qPrintable(result.errorString));
    QCOMPARE(result.finalUrl, server.url(QStringLiteral("/new")));
    QCOMPARE(result.redirects, QStringList({ server.url(QStringLiteral("/old?key=***")), server.url(QStringLiteral("/moved")) }));
    QCOMPARE(result.sha256, QCryptographicHash::hash(server.body, QCryptographicHash::Sha256));
    QCOMPARE(result.statusCode, 200);
    QCOMPARE(server.requests.count(), 3);
    for (const QByteArray &request : std::as_const(server.requests)) {
        QCOMPARE(TestFeedServer::header(request, "X-Api-Key"), QByteArray("s3cret key"));
        QCOMPARE(TestFeedServer::header(request, "Accept"), QByteArray("application/json"));
    }

    // Another server doesn't get them
    result = download(server.url(QStringLiteral("/away")), options);
    QVERIFY2(result.status == GeoZoneDownloader::Result::Status::Ok, qPrintable(result.errorString));
    QCOMPARE(result.finalUrl, otherServer.url(QStringLiteral("/data")));
    QCOMPARE(otherServer.requests.count(), 1);
    QVERIFY(TestFeedServer::header(otherServer.requests.first(), "X-Api-Key").isEmpty());
    QCOMPARE(TestFeedServer::header(otherServer.requests.first(), "Accept"), QByteArray("application/json"));

    result = download(server.url(QStringLiteral("/loop")), options);
    QCOMPARE(result.status, GeoZoneDownloader::Result::Status::Failed);
    QCOMPARE(result.errorString, QStringLiteral("The server redirected more than %1 times.").arg(GeoZoneDownloader::maxRedirects));
    QCOMPARE(result.redirects.count(), GeoZoneDownloader::maxRedirects);

    // A POST redirected by 302 continues with a GET, errors don't show the secrets
    GeoZoneDownloader::Options post = options;
    post.post = true;
    post.body = R"({ "query": true })";
    post.contentType = "application/json";
    server.requests.clear();
    result = download(server.url(QStringLiteral("/form")), post);
    QVERIFY2(result.status == GeoZoneDownloader::Result::Status::Ok, qPrintable(result.errorString));
    QCOMPARE(server.requests.count(), 2);
    QVERIFY(server.requests[0].startsWith("POST /form"));
    QCOMPARE(TestFeedServer::requestBody(server.requests[0]), post.body);
    QCOMPARE(TestFeedServer::header(server.requests[0], "Content-Type"), QByteArray("application/json"));
    QVERIFY(server.requests[1].startsWith("GET /new"));
    result = download(server.url(QStringLiteral("/missing?key=s3cret%20key")), options);
    QCOMPARE(result.status, GeoZoneDownloader::Result::Status::Failed);
    QVERIFY2(!result.errorString.contains(QStringLiteral("s3cret")) && result.errorString.contains(QStringLiteral("key=***")), qPrintable(result.errorString));
    QCOMPARE(GeoZoneDownloader::redacted(QStringLiteral("a s3cret key b s3cret%20key"), options.secrets), QStringLiteral("a *** b ***"));
}

void GeoZoneManagerTest::_testServiceApiKeys()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/header", server.body);
    server.files.insert("/query", server.body);
    server.files.insert("/bearer", server.body);

    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    const QString headerUrl = server.url(QStringLiteral("/header"));
    const QString queryUrl = server.url(QStringLiteral("/query"));
    {
        ScopedLogCapture log;
        GeoZoneManager manager(storageDir.path());
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);

        // An API key in a header
        GeoZoneService service;
        service.auth = GeoZoneService::Auth::ApiKeyHeader;
        service.authParameter = QStringLiteral("X-Api-Key");
        manager.addService(headerUrl, service.toVariantMap(), QStringLiteral("header-s3cret"));
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(TestFeedServer::header(server.requests.last(), "X-Api-Key"), QByteArray("header-s3cret"));
        GeoZoneSource *const headerFeed = manager.source(headerUrl);
        QVERIFY(headerFeed);
        QCOMPARE(manager.zoneCount(), 3);
        QVERIFY(headerFeed->needsCredentials());
        QVERIFY(!headerFeed->credentialsMissing());

        // An API key in the query, which the source's URL doesn't show
        service.auth = GeoZoneService::Auth::ApiKeyQuery;
        service.authParameter = QStringLiteral("api_key");
        manager.addService(queryUrl, service.toVariantMap(), QStringLiteral("query-s3cret"));
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(server.requestedPaths.last(), QByteArray("/query?api_key=query-s3cret"));
        QVERIFY(manager.source(queryUrl));

        // A static bearer token
        service.auth = GeoZoneService::Auth::Bearer;
        service.authParameter.clear();
        manager.addService(server.url(QStringLiteral("/bearer")), service.toVariantMap(), QStringLiteral("bearer-s3cret"));
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(TestFeedServer::header(server.requests.last(), "Authorization"), QByteArray("Bearer bearer-s3cret"));

        // Failures don't show the secret, and a feed which couldn't be added leaves no secret behind
        service.auth = GeoZoneService::Auth::ApiKeyQuery;
        service.authParameter = QStringLiteral("api_key");
        const QString missingUrl = server.url(QStringLiteral("/missing"));
        manager.addService(missingUrl, service.toVariantMap(), QStringLiteral("missing-s3cret"));
        QVERIFY(failedSpy.wait(10000));
        QVERIFY2(!failedSpy.last()[1].toString().contains(QStringLiteral("missing-s3cret")), qPrintable(failedSpy.last()[1].toString()));
        QVERIFY(manager.repository()->credentials().secret(missingUrl).isEmpty());

        // The service is kept with the source, its secret in the settings rather than the store, and neither is logged
        QVERIFY(manager.repository()->credentials().secret(headerUrl) == QStringLiteral("header-s3cret"));
        const QByteArray stored = _directoryContent(storageDir.path());
        QVERIFY(stored.contains("X-Api-Key"));
        for (const char *secret : { "header-s3cret", "query-s3cret", "bearer-s3cret", "missing-s3cret" }) {
            QVERIFY2(!stored.contains(secret), secret);
            QVERIFY2(!log.text().contains(QLatin1String(secret)), secret);
        }
        QVERIFY(log.text().contains(QStringLiteral("Downloading")));
    }

    // The services are restored with the sources, their secrets from the settings
    GeoZoneManager restarted(storageDir.path());
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    GeoZoneSource *const queryFeed = restarted.source(queryUrl);
    QVERIFY(queryFeed);
    QCOMPARE(queryFeed->service().auth, GeoZoneService::Auth::ApiKeyQuery);
    server.requestedPaths.clear();
    restarted.refreshFeeds();
    QTRY_VERIFY(!restarted.busy());
    QVERIFY(server.requestedPaths.contains("/query?api_key=query-s3cret"));

    // Without its secret, a service isn't requested
    restarted.setCredentials(queryUrl, QString());
    QVERIFY(queryFeed->credentialsMissing());
    restarted.refreshFeeds();
    QTRY_VERIFY(!restarted.busy());
    QCOMPARE(queryFeed->errorString(), QStringLiteral("The credentials of the service are missing."));
    QCOMPARE(restarted.zoneCount(), 9);
    restarted.setCredentials(queryUrl, QStringLiteral("new-s3cret"));
    QVERIFY(!queryFeed->credentialsMissing());
    QTRY_VERIFY(!restarted.busy());
    QVERIFY(server.requestedPaths.contains("/query?api_key=new-s3cret"));
    QVERIFY(queryFeed->errorString().isEmpty());

    // Removing the sources removes their secrets
    restarted.clear();
    QVERIFY(restarted.repository()->credentials().secret(queryUrl).isEmpty());
    QVERIFY(restarted.repository()->credentials().secret(headerUrl).isEmpty());
}

void GeoZoneManagerTest::_testServiceOAuth2()
{
    ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int tokenRequests = 0;
    QByteArray currentToken = "token-1";
    server.handler = [&](const QByteArray &request, QByteArray &status, QByteArray &content, QByteArray &) {
        const QByteArray path = request.split(' ').value(1);
        if (path == "/token") {
            tokenRequests++;
            if (!request.startsWith("POST") || (TestFeedServer::header(request, "Authorization") != "Basic " + QByteArray("client:client-s3cret").toBase64()) ||
                (TestFeedServer::requestBody(request) != "grant_type=client_credentials&scope=zones")) {
                status = "401 Unauthorized";
                return true;
            }
            currentToken = "token-" + QByteArray::number(tokenRequests);
            content = R"({ "access_token": ")" + currentToken + R"(", "token_type": "Bearer", "expires_in": 3600 })";
            return true;
        }
        if (path == "/zones") {
            if (TestFeedServer::header(request, "Authorization") != "Bearer " + currentToken) {
                status = "401 Unauthorized";
            } else {
                content = server.body;
            }
            return true;
        }
        return false;
    };

    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    ScopedLogCapture log;
    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);
    GeoZoneService service;
    service.auth = GeoZoneService::Auth::OAuth2ClientCredentials;
    service.tokenUrl = server.url(QStringLiteral("/token"));
    service.user = QStringLiteral("client");
    service.scope = QStringLiteral("zones");
    const QString url = server.url(QStringLiteral("/zones"));

    // A wrong client secret is reported
    manager.addService(url, service.toVariantMap(), QStringLiteral("wrong"));
    QVERIFY(failedSpy.wait(10000));
    QVERIFY2(failedSpy.last()[1].toString().contains(QStringLiteral("The client id or secret were refused.")), qPrintable(failedSpy.last()[1].toString()));
    QCOMPARE(manager.sources()->count(), 0);

    // A token is requested with the client credentials, then used until it expires
    manager.addService(url, service.toVariantMap(), QStringLiteral("client-s3cret"));
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(tokenRequests, 2);
    QCOMPARE(manager.zoneCount(), 3);
    manager.refreshFeeds();
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(tokenRequests, 2);
    QVERIFY(manager.source(url)->errorString().isEmpty());

    // A token the server no longer accepts is replaced
    currentToken = "revoked";
    manager.refreshFeeds();
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(tokenRequests, 3);
    QVERIFY(manager.source(url)->errorString().isEmpty());

    // An expired token is replaced before the download
    environment.addSecs(3600);
    const qsizetype requests = server.requests.count();
    manager.refreshFeeds();
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(tokenRequests, 4);
    QCOMPARE(server.requests.count(), requests + 2);
    QVERIFY(manager.source(url)->errorString().isEmpty());

    QVERIFY(!_directoryContent(storageDir.path()).contains("client-s3cret"));
    QVERIFY(!_directoryContent(storageDir.path()).contains("token-"));
    QVERIFY(!log.text().contains(QStringLiteral("client-s3cret")));
    QVERIFY(!log.text().contains(QStringLiteral("token-")));
}

void GeoZoneManagerTest::_testEd318Service()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QJsonObject query;
    server.handler = [&](const QByteArray &request, QByteArray &status, QByteArray &content, QByteArray &) {
        if (!request.startsWith("POST /uas-zones ")) {
            return false;
        }
        if (TestFeedServer::header(request, "Authorization") != "Basic " + QByteArray("user:dfs-s3cret").toBase64()) {
            status = "401 Unauthorized";
            return true;
        }
        query = QJsonDocument::fromJson(TestFeedServer::requestBody(request)).object();
        const QJsonObject reply{
            { "genericReply", QJsonObject{ { "requestProcessedTimeStamp", "2026-11-01T10:00:00Z" } } },
            { "uasZones", QJsonDocument::fromJson(server.body).object() },
        };
        content = QJsonDocument(reply).toJson();
        return true;
    };
    // A service wrapping the GeoJSON features in its response
    const QJsonArray features = QJsonDocument::fromJson(server.body).object().value("features").toArray();
    const QByteArray wrapped = QJsonDocument(QJsonObject{ { "status", "Success" }, { "data", QJsonObject{ { "geojson", features } } } }).toJson();
    server.files.insert("/wrapped", wrapped);

    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);

    // ED-318 retrieval interface: a POST of the time of interest, the zones in the reply
    GeoZoneService service;
    service.name = QStringLiteral("Test ED-318 service");
    service.method = GeoZoneService::Method::Ed318Post;
    service.auth = GeoZoneService::Auth::Basic;
    service.user = QStringLiteral("user");
    service.timeWindowHours = 6;
    const QString url = server.url(QStringLiteral("/uas-zones"));
    manager.addService(url, service.toVariantMap(), QStringLiteral("dfs-s3cret"));
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(manager.zoneCount(), 3);
    GeoZoneSource *const feed = manager.source(url);
    QVERIFY(feed);
    QCOMPARE(feed->name(), service.name);
    QCOMPARE(feed->formatName(), QStringLiteral("ED-318"));
    QCOMPARE(query.value("startDateTime").toString(), environment.now().toString(Qt::ISODate));
    QCOMPARE(query.value("endDateTime").toString(), environment.now().addSecs(6 * 3600).toString(Qt::ISODate));
    QVERIFY(!query.contains("geometry"));

    // The stored copy is the zones' document
    QFile stored(QDir(storageDir.path()).filePath(manager.storedFileName(url)));
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(stored.readAll()).object(), QJsonDocument::fromJson(server.body).object());

    // Zones in a member of the response, like the GeoJSON of the FAA NOTAM Management Service
    service = GeoZoneService();
    service.responseMember = QStringLiteral("data.geojson");
    manager.addService(server.url(QStringLiteral("/wrapped")), service.toVariantMap(), QString());
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(manager.zoneCount(), 6);
    service.responseMember = QStringLiteral("data.aixm");
    manager.addService(server.url(QStringLiteral("/wrapped")), service.toVariantMap(), QString());
    QVERIFY(failedSpy.wait(10000));
    QVERIFY2(failedSpy.last()[1].toString().contains(QStringLiteral("has no data.aixm")), qPrintable(failedSpy.last()[1].toString()));
    QCOMPARE(manager.zoneCount(), 6);

    // The FAA NOTAM Management Service is configured, but its NOTAMs can't be read yet
    const QList<GeoZoneProviders::ServiceTemplate> templates = GeoZoneProviders::serviceTemplates();
    const auto nms = std::find_if(templates.cbegin(), templates.cend(), [](const GeoZoneProviders::ServiceTemplate &serviceTemplate) {
        return serviceTemplate.service.id == QStringLiteral("faa-nms");
    });
    QVERIFY(nms != templates.cend());
    QVERIFY(!nms->supported);
    QCOMPARE(nms->service.auth, GeoZoneService::Auth::OAuth2ClientCredentials);
    QVERIFY(GeoZoneService::hasAreaPlaceholders(nms->url));
    QCOMPARE(GeoZoneService::fromVariantMap(nms->service.toVariantMap()), nms->service);
}

void GeoZoneManagerTest::_testServiceAreaOfInterest()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/zones", server.body);
    QJsonObject query;
    server.handler = [&](const QByteArray &request, QByteArray &, QByteArray &content, QByteArray &) {
        if (!request.startsWith("POST /uas-zones ")) {
            return false;
        }
        query = QJsonDocument::fromJson(TestFeedServer::requestBody(request)).object();
        content = QJsonDocument(QJsonObject{ { "uasZones", QJsonDocument::fromJson(server.body).object() } }).toJson();
        return true;
    };

    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    const QString url = server.url(QStringLiteral("/zones?bbox={bbox}&lat={lat}&radius={radiusKm}"));
    const auto requestedUrl = [&server]() { return server.url(QString::fromUtf8(server.requestedPaths.last())); };
    {
        GeoZoneManager manager(storageDir.path());
        GeoZoneSourceRepository *const repository = manager.repository();
        repository->setAreaMargin(1000);
        repository->setFollowArea(true);
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);

        // Without a mission or a vehicle, there is no area to download
        manager.addService(url, GeoZoneService().toVariantMap(), QString());
        QTRY_COMPARE(failedSpy.count(), 1);
        QVERIFY2(failedSpy.last()[1].toString().contains(QStringLiteral("Plan a mission or connect a vehicle")), qPrintable(failedSpy.last()[1].toString()));
        QCOMPARE(manager.sources()->count(), 0);

        // The zones around the vehicle, the URL template filled with the area
        repository->setVehiclePosition(QGeoCoordinate(47.0, 8.0));
        manager.addService(url, GeoZoneService().toVariantMap(), QString());
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.busy());
        GeoZoneSource *const feed = manager.source(url);
        QVERIFY(feed);
        QVERIFY(feed->followsArea());
        const QGeoRectangle area = GeoZoneSourceRepository::expanded(QGeoRectangle(QGeoCoordinate(47.0, 8.0), QGeoCoordinate(47.0, 8.0)), 1000);
        QCOMPARE(feed->area(), area);
        QVERIFY(std::abs(area.height() - (2000.0 / 111320)) < 1e-6);
        QCOMPARE(requestedUrl(), GeoZoneService::resolve(url, area, QDateTime(), QDateTime()));
        QVERIFY2(requestedUrl().contains(QStringLiteral("&lat=47.00000&radius=1.5")), qPrintable(requestedUrl()));

        // Moving within the area downloads nothing, leaving it downloads the new area
        const int requests = server.requestCount;
        repository->setVehiclePosition(QGeoCoordinate(47.005, 8.005));
        QVERIFY(!manager.busy());
        repository->setVehiclePosition(QGeoCoordinate(47.1, 8.0));
        QVERIFY(manager.busy());
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(server.requestCount, requests + 1);
        QVERIFY(feed->area().contains(QGeoCoordinate(47.1, 8.0)));
        QVERIFY(!feed->area().contains(QGeoCoordinate(47.0, 8.0)));

        // The area of interest covers the missions and the vehicle
        const QObject mission;
        const QGeoRectangle missionArea(QGeoCoordinate(47.3, 7.9), QGeoCoordinate(47.2, 8.1));
        manager.setMissionArea(&mission, missionArea);
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(repository->areaOfInterest(), QGeoRectangle(QGeoCoordinate(47.3, 7.9), QGeoCoordinate(47.1, 8.1)));
        QVERIFY(feed->area().contains(repository->areaOfInterest()));
        QCOMPARE(server.requestCount, requests + 2);

        // Feeds keep their area while they don't follow it, also when refreshed
        repository->setFollowArea(false);
        const QGeoRectangle kept = feed->area();
        repository->setVehiclePosition(QGeoCoordinate(48.0, 9.0));
        QVERIFY(!manager.busy());
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(feed->area(), kept);
        repository->setFollowArea(true);
        QTRY_VERIFY(!manager.busy());
        QVERIFY(feed->area().contains(QGeoCoordinate(48.0, 9.0)));
        manager.setMissionArea(&mission, QGeoRectangle());

        // ED-318 services get the area as a polygon
        GeoZoneService ed318;
        ed318.method = GeoZoneService::Method::Ed318Post;
        ed318.areaOfInterest = true;
        manager.addService(server.url(QStringLiteral("/uas-zones")), ed318.toVariantMap(), QString());
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.busy());
        const QGeoRectangle ed318Area = manager.source(server.url(QStringLiteral("/uas-zones")))->area();
        QVERIFY(ed318Area.contains(QGeoCoordinate(48.0, 9.0)));
        const QJsonObject geometry = query.value("geometry").toObject();
        QCOMPARE(geometry.value("type").toString(), QStringLiteral("Polygon"));
        const QJsonArray ring = geometry.value("coordinates").toArray().first().toArray();
        QCOMPARE(ring.count(), 5);
        QCOMPARE(ring[0].toArray(), (QJsonArray{ ed318Area.topLeft().longitude(), ed318Area.bottomRight().latitude() }));
        QCOMPARE(ring[2].toArray(), (QJsonArray{ ed318Area.bottomRight().longitude(), ed318Area.topLeft().latitude() }));
    }

    // The area is kept with the source and refreshed without a mission or vehicle
    GeoZoneManager restarted(storageDir.path());
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    GeoZoneSource *const feed = restarted.source(url);
    QVERIFY(feed);
    const QGeoRectangle stored = feed->area();
    QVERIFY(stored.contains(QGeoCoordinate(48.0, 9.0)));
    restarted.refreshFeeds();
    QTRY_VERIFY(!restarted.busy());
    QVERIFY(feed->errorString().isEmpty());
    const QString storedUrl = GeoZoneService::resolve(url, stored, QDateTime(), QDateTime());
    QVERIFY(std::any_of(server.requestedPaths.cbegin(), server.requestedPaths.cend(), [&](const QByteArray &path) { return server.url(QString::fromUtf8(path)) == storedUrl; }));
    QCOMPARE(feed->area(), stored);

    // Across the antimeridian
    const QGeoRectangle crossing = GeoZoneSourceRepository::expanded(QGeoRectangle(QGeoCoordinate(-17.0, 179.99), QGeoCoordinate(-17.0, 179.99)), 5000);
    QVERIFY(crossing.topLeft().longitude() > 179.9);
    QVERIFY(crossing.bottomRight().longitude() < -179.9);
    QVERIFY(crossing.contains(QGeoCoordinate(-17.0, -179.99)));
}

void GeoZoneManagerTest::_testProvenance()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.redirects.insert("/latest", "/v2/zones.json");
    server.files.insert("/v2/zones.json", server.body);
    server.etags.insert("/v2/zones.json", "\"p1\"");
    server.headers.insert("/v2/zones.json", "Last-Modified: Sat, 31 Oct 2026 10:00:00 GMT\r\n");
    const QString url = server.url(QStringLiteral("/latest"));

    const QTemporaryDir storageDir;
    const QTemporaryDir exportDir;
    QVERIFY(storageDir.isValid() && exportDir.isValid());
    QJsonObject report;
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        manager.addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.busy());
        const GeoZoneSource *const feed = manager.source(url);
        QVERIFY(feed);

        // The redirects, validators, hashes and time of the download are recorded
        const GeoZoneProvenance &provenance = feed->provenance();
        QCOMPARE(provenance.requestUrl, url);
        QCOMPARE(provenance.finalUrl, server.url(QStringLiteral("/v2/zones.json")));
        QCOMPARE(provenance.redirects, QStringList({ url }));
        QCOMPARE(provenance.etag, QStringLiteral("\"p1\""));
        QCOMPARE(provenance.lastModified, QStringLiteral("Sat, 31 Oct 2026 10:00:00 GMT"));
        QCOMPARE(provenance.fetchedAt, environment.now());
        QCOMPARE(provenance.sha256, QCryptographicHash::hash(server.body, QCryptographicHash::Sha256));
        QCOMPARE(provenance.contentHash, GeoZoneParseCache::hash(server.body));
        QCOMPARE(provenance.size, server.body.size());
        QCOMPARE(provenance.integrity, GeoZoneProvenance::Integrity::Unchecked);
        QVERIFY(!feed->verified());
        QVERIFY2(feed->provenanceText().contains(QStringLiteral("redirected from 127.0.0.1")), qPrintable(feed->provenanceText()));
        QVERIFY(feed->provenanceText().contains(QStringLiteral("SHA-256 %1").arg(QString::fromLatin1(provenance.sha256.toHex().left(12)))));

        // Files have their provenance too
        const QTemporaryDir sourceDir;
        const QString fileName = _writeFeatures(sourceDir, QStringLiteral("zones.json"), QJsonArray{
            _circleFeature(QStringLiteral("F1"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.0, 8.0), 500) });
        QString errorString;
        QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));
        const GeoZoneSource *const file = manager.source(GeoZoneLoader::fileKey(fileName));
        QCOMPARE(file->provenance().requestUrl, QUrl::fromLocalFile(GeoZoneLoader::fileKey(fileName)).toString());
        QCOMPARE(file->provenance().contentHash.size(), 32);
        QVERIFY(file->provenance().sha256.isEmpty());

        // The report for briefings
        QVERIFY(manager.exportProvenance(exportDir.filePath(QStringLiteral("provenance.json"))));
        QFile exported(exportDir.filePath(QStringLiteral("provenance.json")));
        QVERIFY(exported.open(QIODevice::ReadOnly));
        report = QJsonDocument::fromJson(exported.readAll()).object();
        QCOMPARE(report, manager.provenanceReport());
        QCOMPARE(report.value("sources").toArray().count(), 2);
        const QJsonObject feedReport = report.value("sources").toArray().first().toObject();
        QCOMPARE(feedReport.value("url").toString(), url);
        QCOMPARE(feedReport.value("zoneCount").toInt(), 3);
        QCOMPARE(feedReport.value("format").toString(), QStringLiteral("ED-318"));
        const QJsonObject provenanceReport = feedReport.value("provenance").toObject();
        QCOMPARE(provenanceReport.value("finalUrl").toString(), server.url(QStringLiteral("/v2/zones.json")));
        QCOMPARE(provenanceReport.value("sha256").toString(), QString::fromLatin1(provenance.sha256.toHex()));
        QCOMPARE(provenanceReport.value("blake2b256").toString(), QString::fromLatin1(provenance.contentHash.toHex()));
        QCOMPARE(provenanceReport.value("fetchedAt").toString(), environment.now().toString(Qt::ISODate));
        QCOMPARE(provenanceReport.value("integrity").toString(), QStringLiteral("unchecked"));
    }

    // The provenance is kept with the stored copies
    GeoZoneManager restarted(storageDir.path());
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QVERIFY(restarted.source(url));
    QCOMPARE(restarted.source(url)->provenance().toJson(), report.value("sources").toArray().first().toObject().value("provenance").toObject());
    QCOMPARE(GeoZoneProvenance::fromJson(restarted.source(url)->provenance().toJson()).toJson(), restarted.source(url)->provenance().toJson());
}

void GeoZoneManagerTest::_testIntegrity()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QByteArray sha256 = QCryptographicHash::hash(server.body, QCryptographicHash::Sha256);
    server.files.insert("/digest", server.body);
    server.headers.insert("/digest", "Repr-Digest: sha-256=:" + sha256.toBase64() + ":\r\n");
    server.files.insert("/tampered", server.body);
    server.headers.insert("/tampered", "Digest: SHA-256=" + QCryptographicHash::hash("other", QCryptographicHash::Sha256).toBase64() + "\r\n");
    server.files.insert("/signed.json", server.body);
    server.files.insert("/signed.json.sha256", sha256.toHex() + "  signed.json\n");

    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);

    // A digest header matching the data verifies it
    manager.addUrl(server.url(QStringLiteral("/digest")));
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.busy());
    const GeoZoneSource *const digest = manager.source(server.url(QStringLiteral("/digest")));
    QVERIFY(digest->verified());
    QCOMPARE(digest->provenance().integrityText, QStringLiteral("repr-digest header"));
    QVERIFY(digest->provenanceText().contains(QStringLiteral("verified")));

    // Data which doesn't match isn't loaded
    manager.addUrl(server.url(QStringLiteral("/tampered")));
    QVERIFY(failedSpy.wait(10000));
    QVERIFY2(failedSpy.last()[1].toString().contains(QStringLiteral("The integrity check failed: the SHA-256 of the data doesn't match its digest header.")),
             qPrintable(failedSpy.last()[1].toString()));
    QVERIFY(!manager.source(server.url(QStringLiteral("/tampered"))));

    // The SHA-256 a service publishes next to its data
    GeoZoneService service;
    service.id = QStringLiteral("signed-test");
    service.sha256Url = QStringLiteral("{url}.sha256");
    const QString signedUrl = server.url(QStringLiteral("/signed.json"));
    manager.addService(signedUrl, service.toVariantMap(), QString());
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.busy());
    GeoZoneSource *const signedFeed = manager.source(signedUrl);
    QVERIFY(signedFeed->verified());
    QCOMPARE(signedFeed->provenance().integrityText, server.url(QStringLiteral("/signed.json.sha256")));
    QCOMPARE(signedFeed->provenance().providerId, QStringLiteral("signed-test"));
    QVERIFY(!signedFeed->integrityFailed());

    // A refresh which doesn't match keeps the verified zones and marks the source
    server.files.insert("/signed.json.sha256", QCryptographicHash::hash("other", QCryptographicHash::Sha256).toHex());
    manager.refreshFeeds();
    QTRY_VERIFY(!manager.busy());
    QVERIFY(signedFeed->integrityFailed());
    QVERIFY2(signedFeed->errorString().contains(QStringLiteral("doesn't match %1").arg(server.url(QStringLiteral("/signed.json.sha256")))), qPrintable(signedFeed->errorString()));
    QVERIFY(signedFeed->verified());
    QCOMPARE(signedFeed->zoneCount(), 3);
    QVERIFY(GeoZoneSourceRepository::sourceReport(*signedFeed).value("integrityFailed").toBool());

    // As does a SHA-256 which can't be read
    server.files.remove("/signed.json.sha256");
    manager.refreshFeeds();
    QTRY_VERIFY(!manager.busy());
    QVERIFY(signedFeed->integrityFailed());
    QVERIFY(signedFeed->errorString().contains(QStringLiteral("could not be downloaded")));
    server.files.insert("/signed.json.sha256", "not a hash");
    manager.refreshFeeds();
    QTRY_VERIFY(!manager.busy());
    QVERIFY(signedFeed->errorString().contains(QStringLiteral("has no SHA-256")));

    server.files.insert("/signed.json.sha256", sha256.toHex().toUpper());
    manager.refreshFeeds();
    QTRY_VERIFY(!manager.busy());
    QVERIFY(!signedFeed->integrityFailed());
    QVERIFY(signedFeed->errorString().isEmpty());
}

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
        manager.addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.busy());
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

    // Startup loads the stored copy, refreshes due feeds, then reports outdated data once: the test file was
    // issued months ago
    GeoZoneManager manager(storageDir.path());
    QSignalSpy outdatedSpy(&manager, &GeoZoneManager::outdatedDataFound);
    manager.init();
    QCOMPARE(manager.sources()->count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount, 2, 10000);
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(manager.zoneCount(), 3);
    QVERIFY(manager.source(url)->loadedAt() > downloaded.addDays(1));
    QTRY_COMPARE(outdatedSpy.count(), 1);
    QVERIFY(outdatedSpy.first()[0].toString().contains(QStringLiteral("days old")));
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
    const QList<Fact*> facts = { settings->showZones(), settings->approachDistance(), settings->staleDays(),
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

    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QGeoCoordinate center(47, 8);
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300) });
    QFile file(fileName);
    QVERIFY(file.open(QIODevice::ReadWrite) && file.setFileTime(environment.now().addDays(-10), QFileDevice::FileModificationTime));
    file.close();
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));

    // The manager visibility is the setting
    QSignalSpy visibleSpy(manager.display(), &GeoZoneDisplay::visibleChanged);
    settings->showZones()->setRawValue(false);
    QVERIFY(!manager.display()->visible());
    QCOMPARE(visibleSpy.count(), 1);
    manager.display()->setVisible(true);
    QVERIFY(settings->showZones()->rawValue().toBool());

    // Outdated data is reported at the configured age
    QVERIFY(manager.validityWarning().isEmpty());
    settings->staleDays()->setRawValue(7);
    QCOMPARE(manager.validityWarning(), QStringLiteral("zones.json: Data is 10 days old"));

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

    // Approach alerts at the configured distance
    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.evaluate({ center.atDistanceAndAzimuth(500, 0) });
    QCOMPARE(alertSpy.count(), 0);
    settings->approachDistance()->setRawValue(250);
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
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));
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
    airport[QStringLiteral("name")] = QStringLiteral("Zurich airport");
    QJsonObject prison = _circleFeature(QStringLiteral("GZW1"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(500, 90), 300);
    prison[QStringLiteral("name")] = QStringLiteral("Prison Zurich West");
    QJsonObject far = _circleFeature(QStringLiteral("FAR"), QStringLiteral("PROHIBITED"), QGeoCoordinate(46, 7), 300);
    far[QStringLiteral("name")] = QStringLiteral("Bern area");
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ airport, prison, far }), errorString), qPrintable(errorString));

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

    QSignalSpy showSpy(&manager, &GeoZoneManager::showZoneRequested);
    manager.showZone(QVariant::fromValue(GeoZoneInfo(manager.zoneList().first())));
    manager.showZone(QVariant(QStringLiteral("not a zone")));
    QCOMPARE(showSpy.count(), 1);
    QCOMPARE(GeoZoneInfo::fromVariant(showSpy.first().first()), manager.zoneList().first());

    // Highlighted zones are marked on their map items
    GeoZoneMapModel model(&manager);
    model.setView(QGeoRectangle(center.atDistanceAndAzimuth(5000, 315), center.atDistanceAndAzimuth(5000, 135)), 12, false);
    QCOMPARE(model.circles()->count(), 2);
    const QVariant prisonZone = manager.searchZones(QStringLiteral("GZW1")).first();
    model.setHighlightedZones({ prisonZone });
    for (int i = 0; i < model.circles()->count(); i++) {
        const GeoZoneMapItem *const item = model.circles()->value<GeoZoneMapItem*>(i);
        QCOMPARE(item->highlighted(), item->zone() == GeoZoneInfo::fromVariant(prisonZone));
    }

    // Values given to QML stay readable after their source is removed
    manager.clear();
    QCOMPARE(manager.zoneCount(), 0);
    QCOMPARE(model.circles()->count(), 0);
    QVERIFY(prisonZone.value<GeoZoneInfo>().valid());
    QCOMPARE(prisonZone.value<GeoZoneInfo>().identifier(), QStringLiteral("GZW1"));
}

void GeoZoneManagerTest::_testFeedDatasetLink()
{
    QFile fixture(QStringLiteral(":/unittest/uk_aixm_test.xml"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const QByteArray dataset = fixture.readAll();

    // A page linking the dataset of each cycle, which answers conditional requests with 304 Not Modified like a
    // page which didn't change, and datasets with their own ETag
    QByteArray page = "<html><body><a href=\"/d/UAS_AREA_1/EG_FULL_20200101_XML.zip\">XML</a></body></html>";
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QList<QByteArray> requests;
    (void) connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (QTcpSocket *const socket = server.nextPendingConnection()) {
            (void) connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) {
                    return;
                }
                const QByteArray lower = request.toLower();
                requests.append(request);
                const QByteArray path = request.split(' ').value(1);
                const bool conditional = lower.contains("if-none-match") || lower.contains("if-modified-since");
                QByteArray response;
                if (path == "/page") {
                    response = conditional ? QByteArray("HTTP/1.1 304 Not Modified\r\nContent-Length: 0\r\n") :
                                             ("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + QByteArray::number(page.size()) + "\r\n");
                    response += "Connection: close\r\n\r\n" + (conditional ? QByteArray() : page);
                } else {
                    const QByteArray etag = "\"" + path.right(16) + "\"";
                    if (lower.contains("if-none-match: " + etag.toLower())) {
                        response = "HTTP/1.1 304 Not Modified\r\nETag: " + etag + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    } else {
                        response = "HTTP/1.1 200 OK\r\nETag: " + etag + "\r\nLast-Modified: Sat, 31 Oct 2026 10:00:00 GMT\r\nContent-Type: application/xml\r\n"
                                   "Connection: close\r\nContent-Length: " + QByteArray::number(dataset.size()) + "\r\n\r\n" + dataset;
                    }
                }
                (void) socket->write(response);
                socket->disconnectFromHost();
            });
            (void) connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    const QString url = QStringLiteral("http://127.0.0.1:%1/page").arg(server.serverPort());
    const auto lastRequest = [&requests](const QByteArray &path) {
        for (qsizetype i = requests.count() - 1; i >= 0; i--) {
            if (requests[i].split(' ').value(1) == path) {
                return requests[i].toLower();
            }
        }
        return QByteArray();
    };

    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        manager.addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.busy());
        GeoZoneSource *const feed = manager.source(url);
        QVERIFY(feed);
        QVERIFY(feed->zoneCount() > 0);
        QVERIFY(feed->datasetUrl().endsWith(QStringLiteral("EG_FULL_20200101_XML.zip")));

        // The page is read again without validators, the unchanged dataset is confirmed by its own
        const int zoneCount = manager.zoneCount();
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QVERIFY(!lastRequest("/page").contains("if-none-match"));
        QVERIFY(!lastRequest("/page").contains("if-modified-since"));
        QVERIFY(lastRequest("/d/UAS_AREA_1/EG_FULL_20200101_XML.zip").contains("if-none-match"));
        QCOMPARE(manager.zoneCount(), zoneCount);
        QVERIFY(feed->errorString().isEmpty());

        // A new cycle on the page is downloaded
        page = "<html><body><a href=\"/d/UAS_AREA_1/EG_FULL_20200101_XML.zip\">XML</a>"
               "<a href=\"/d/UAS_AREA_1/EG_FULL_20210101_XML.zip\">XML</a></body></html>";
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QVERIFY(feed->datasetUrl().endsWith(QStringLiteral("EG_FULL_20210101_XML.zip")));
        QVERIFY(!lastRequest("/d/UAS_AREA_1/EG_FULL_20210101_XML.zip").isEmpty());
        QVERIFY(!lastRequest("/d/UAS_AREA_1/EG_FULL_20210101_XML.zip").contains("if-none-match"));
    }

    // The dataset is remembered with its validators
    GeoZoneManager restarted(storageDir.path());
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QVERIFY(restarted.source(url));
    QVERIFY(restarted.source(url)->datasetUrl().endsWith(QStringLiteral("EG_FULL_20210101_XML.zip")));
    QCOMPARE(restarted.source(url)->etag(), QStringLiteral("\"20210101_XML.zip\""));
}

void GeoZoneManagerTest::_testWfsPaging()
{
    // A WFS limiting its responses to 2 of the 5 features of the area, like the IGN and DiPUL servers
    QFile fixture(QStringLiteral(":/unittest/france_uas_test.json"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const QJsonObject template_ = QJsonDocument::fromJson(fixture.readAll()).object();
    QJsonArray allFeatures;
    for (int i = 0; i < 5; i++) {
        QJsonObject feature = template_.value(QStringLiteral("features")).toArray().first().toObject();
        feature.insert(QStringLiteral("id"), QStringLiteral("carte_restriction_drones_lf.%1").arg(i));
        QJsonObject geometry = feature.value(QStringLiteral("geometry")).toObject();
        geometry.insert(QStringLiteral("coordinates"), QJsonArray{ QJsonArray{ QJsonArray{
            QJsonArray{ 2.0 + (i * 0.1), 48.0 }, QJsonArray{ 2.05 + (i * 0.1), 48.0 }, QJsonArray{ 2.05 + (i * 0.1), 48.05 }, QJsonArray{ 2.0 + (i * 0.1), 48.0 } } } });
        feature.insert(QStringLiteral("geometry"), geometry);
        allFeatures.append(feature);
    }
    constexpr int pageSize = 2;
    bool failThirdPage = false;
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QList<QUrlQuery> queries;
    (void) connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (QTcpSocket *const socket = server.nextPendingConnection()) {
            (void) connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n")) {
                    return;
                }
                const QUrlQuery query(QUrl(QString::fromLatin1(request.split(' ').value(1))));
                queries.append(query);
                const int start = query.queryItemValue(QStringLiteral("STARTINDEX")).toInt();
                QByteArray response;
                if (failThirdPage && (start >= 2 * pageSize)) {
                    response = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                } else {
                    QJsonObject page = template_;
                    QJsonArray features;
                    for (int i = start; (i < start + pageSize) && (i < allFeatures.count()); i++) {
                        features.append(allFeatures[i]);
                    }
                    page.insert(QStringLiteral("features"), features);
                    page.insert(QStringLiteral("numberMatched"), allFeatures.count());
                    page.insert(QStringLiteral("totalFeatures"), allFeatures.count());
                    page.insert(QStringLiteral("numberReturned"), features.count());
                    const QByteArray body = QJsonDocument(page).toJson(QJsonDocument::Compact);
                    response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
                }
                (void) socket->write(response);
                socket->disconnectFromHost();
            });
            (void) connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    const QString url = QStringLiteral("http://127.0.0.1:%1/wfs?SERVICE=WFS&VERSION=2.0.0&REQUEST=GetFeature&TYPENAMES=restrictions&OUTPUTFORMAT=application/json")
                            .arg(server.serverPort());

    // All pages are downloaded and stored together
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        manager.addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.zoneCount(), 5);
        QCOMPARE(queries.count(), 3);
        QCOMPARE(queries[1].queryItemValue(QStringLiteral("STARTINDEX")), QStringLiteral("2"));
        QCOMPARE(queries[2].queryItemValue(QStringLiteral("STARTINDEX")), QStringLiteral("4"));
        QVERIFY(manager.source(url)->warningText().isEmpty());
    }
    GeoZoneManager restarted(storageDir.path());
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QCOMPARE(restarted.zoneCount(), 5);

    // A page which fails leaves the zones of the others, and the source tells how many are missing
    failThirdPage = true;
    restarted.refreshFeeds();
    QTRY_VERIFY(!restarted.busy());
    QCOMPARE(restarted.zoneCount(), 4);
    QVERIFY(restarted.source(url)->warningText().contains(QStringLiteral("left out 1 zones")));
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
    QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("old.json"), QJsonArray{
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
        manager.clear();
        QVERIFY2(manager.loadFile(_writeFeatures(dir, QStringLiteral("new.json"), QJsonArray{
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
