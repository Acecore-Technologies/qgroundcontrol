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

    {
        GeoZoneManager manager(storageDir.path());
        manager.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
        QCOMPARE(manager.zones()->count(), 0);
        QCOMPARE(manager.sources()->count(), 0);

        QString errorString;
        QVERIFY2(manager.loadFile(ed318File, errorString), qPrintable(errorString));
        QCOMPARE(manager.zones()->count(), 3);
        QCOMPARE(manager.shapes().count(), 5);
        QCOMPARE(std::count_if(manager.shapes().cbegin(), manager.shapes().cend(), [](const GeoZoneShape *shape) { return shape->isCircle(); }), 1);
        QVERIFY(manager.boundingBox().isValid());

        QCOMPARE(manager.sources()->count(), 1);
        const GeoZoneSource *const ed318Source = manager.sources()->value<GeoZoneSource*>(0);
        QCOMPARE(ed318Source->key(), QStringLiteral("zones.json"));
        QVERIFY(!ed318Source->isFeed());
        QCOMPARE(ed318Source->name(), QStringLiteral("Test GeoZones"));
        QCOMPARE(ed318Source->formatName(), QStringLiteral("ED-318"));
        QCOMPARE(ed318Source->provider(), QStringLiteral("Test Authority"));
        QCOMPARE(ed318Source->skippedCount(), 1);
        QCOMPARE(ed318Source->dataDate(), _utc(QDate(2026, 8, 13), QTime(0, 0)));
        QVERIFY(ed318Source->detailsText().contains(QStringLiteral("issued 2026-08-13")));
        QCOMPARE(manager.zones()->value<GeoZone*>(0)->sourceText(), QStringLiteral("Test GeoZones (issued 2026-08-13)"));
        QVERIFY(QFile::exists(QDir(storageDir.path()).filePath(manager.storedFileName(ed318Source->key()))));

        // A failed load keeps the current zones
        QVERIFY(!manager.loadFile(sourceDir.filePath(QStringLiteral("missing.json")), errorString));
        QVERIFY(!errorString.isEmpty());
        QCOMPARE(manager.zones()->count(), 3);
        QCOMPARE(manager.sources()->count(), 1);

        QCOMPARE(manager.zonesInArea({ GeoZone::Prohibited }, QGeoRectangle()).count(), 1);
        const QGeoRectangle ireland(QGeoCoordinate(52.0, -9.0), QGeoCoordinate(51.0, -8.0));
        const QList<GeoZone*> zonesInArea = manager.zonesInArea({ GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::USpace }, ireland);
        QCOMPARE(zonesInArea.count(), 1);
        QCOMPARE(zonesInArea[0]->name(), QStringLiteral("Control zone"));

        // Another file adds its zones, loading a file with the same name again replaces its zones
        QVERIFY2(manager.loadFile(ed269File, errorString), qPrintable(errorString));
        QCOMPARE(manager.sources()->count(), 2);
        QCOMPARE(manager.zones()->count(), 6);
        const QString previousStoredFile = manager.storedFileName(ed318Source->key());
        QVERIFY2(manager.loadFile(ed318File, errorString), qPrintable(errorString));
        QCOMPARE(manager.sources()->count(), 2);
        QCOMPARE(manager.zones()->count(), 6);
        QVERIFY(manager.storedFileName(ed318Source->key()) != previousStoredFile);
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
        QCOMPARE(manager.zones()->count(), 6);
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(0)->key(), QStringLiteral("zones.json"));
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(0)->formatName(), QStringLiteral("ED-318"));
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(1)->key(), QStringLiteral("other.json"));
        QVERIFY(!strayFile.exists());

        manager.removeSource(0);
        QCOMPARE(manager.sources()->count(), 1);
        QCOMPARE(manager.zones()->count(), 3);
        QCOMPARE(storedFiles().count(), 2);

        manager.clear();
        QCOMPARE(manager.zones()->count(), 0);
        QVERIFY(manager.shapes().isEmpty());
        QVERIFY(storedFiles().isEmpty());
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
        QCOMPARE(legacyManager.zones()->count(), 3);
        QVERIFY(!settings.contains(QStringLiteral("GeoZoneManager/sourceName")));
    }
    GeoZoneManager migratedManager(legacyDir.path());
    migratedManager.loadStoredSources(GeoZoneManager::LoadMode::Synchronous);
    QCOMPARE(migratedManager.zones()->count(), 3);
}

void GeoZoneManagerTest::_testManagerStoreFailures()
{
    const QTemporaryDir storageDir;
    const QTemporaryDir sourceDir;
    QVERIFY(storageDir.isValid() && sourceDir.isValid());
    const QString ed318File = sourceDir.filePath(QStringLiteral("zones.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), ed318File));
    const QDir dir(storageDir.path());
    const QString key = QStringLiteral("zones.json");
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
        QCOMPARE(manager.zones()->count(), 3);
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
    QCOMPARE(manager.zones()->count(), 3);

    // Only the most recent load of a file name is applied
    addedSpy.clear();
    manager.addFile(ed318File);
    manager.addFile(ed269File);
    QTRY_VERIFY(!manager.busy());
    QCOMPARE(addedSpy.count(), 1);
    QCOMPARE(manager.sources()->count(), 1);
    QCOMPARE(manager.sources()->value<GeoZoneSource*>(0)->formatName(), QStringLiteral("ED-269"));

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
    QCOMPARE(manager.zones()->count(), 3);
    // The user's file is read in place, never removed
    QVERIFY(QFile::exists(ed318File));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVERIFY(!feed->refreshDue(now));
    QVERIFY(feed->refreshDue(now.addSecs(25 * 3600)));

    // A failed refresh keeps the previous data
    QVERIFY(QFile::remove(ed318File));
    manager.refreshFeeds();
    QVERIFY(feed->busy());
    QTRY_VERIFY(!manager.busy());
    QVERIFY(!feed->busy());
    QVERIFY(!feed->errorString().isEmpty());
    QCOMPARE(manager.zones()->count(), 3);

    // Stored sources load in the background in their original order
    GeoZoneManager restarted(storageDir.path());
    restarted.loadStoredSources(GeoZoneManager::LoadMode::Background);
    QCOMPARE(restarted.sources()->count(), 1);
    QVERIFY(restarted.busy());
    QTRY_VERIFY(!restarted.busy());
    QCOMPARE(restarted.zones()->count(), 3);
    QVERIFY(restarted.source(feedUrl));
    QVERIFY(restarted.source(feedUrl)->isFeed());
}

void GeoZoneManagerTest::_testFeedNotModified()
{
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

        // An unchanged feed is not downloaded again but counts as current
        QSignalSpy aboutToChangeSpy(&manager, &GeoZoneManager::zonesAboutToChange);
        const QDateTime loadedAt = feed->loadedAt();
        QTest::qWait(10);
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(notModifiedCount, 1);
        QCOMPARE(manager.zones()->count(), 3);
        QVERIFY(feed->loadedAt() > loadedAt);
        QVERIFY(feed->errorString().isEmpty());

        // A server without validators sends the same zones again, which are kept instead of reloaded
        QCOMPARE(aboutToChangeSpy.count(), 0);
        feed->setValidators(QString(), QString());
        manager.refreshFeeds();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(requestCount, 3);
        QCOMPARE(aboutToChangeSpy.count(), 0);
        QCOMPARE(manager.zones()->count(), 3);

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
        QCOMPARE(manager.zones()->value<GeoZone*>(0)->sourceText(), QStringLiteral("Test GeoZones (issued 2026-08-20)"));

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
    restarted.refreshFeeds();
    QTRY_VERIFY(!restarted.busy());
    QCOMPARE(notModifiedCount, 2);
    QCOMPARE(restarted.zones()->count(), 3);
}

void GeoZoneManagerTest::_testSourceValidity()
{
    GeoZoneSource source(QStringLiteral("zones.json"), QString());
    GeoZoneParser::Result result;
    result.format = GeoZoneParser::Format::ED269;
    const QDateTime now = QDateTime::currentDateTimeUtc();

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

    // The manager summarizes outdated sources
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.path());
    QString errorString;
    QVERIFY2(manager.loadFile(QStringLiteral(":/unittest/ed318_test.json"), errorString), qPrintable(errorString));
    QVERIFY(manager.validityWarning().startsWith(QStringLiteral("Test GeoZones: Data is")));
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
    QCOMPARE(result.data.size(), body.size());

    downloader.setMaxBytes(1000);
    for (const QString &path : { QStringLiteral("sized"), QStringLiteral("unsized") }) {
        result = download(path);
        QCOMPARE(result.status, GeoZoneDownloader::Result::Status::Failed);
        QVERIFY2(result.errorString.startsWith(QStringLiteral("The download is larger than")), qPrintable(result.errorString));
        QVERIFY(result.data.isEmpty());
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
    QCOMPARE(manager.zones()->count(), 3);

    // The next successful refresh clears the error
    server.okPathStatus = "200 OK";
    manager.refreshFeeds();
    QTRY_VERIFY(!manager.busy());
    QVERIFY(feed->errorString().isEmpty());
}

void GeoZoneManagerTest::_testManagerStartup()
{
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
    const QDateTime downloaded = QDateTime::currentDateTimeUtc().addDays(-2);
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
    QCOMPARE(manager.zones()->count(), 3);
    QVERIFY(manager.source(url)->loadedAt() > downloaded.addDays(1));
    QTRY_COMPARE(outdatedSpy.count(), 1);
    QVERIFY(outdatedSpy.first()[0].toString().contains(QStringLiteral("days old")));
}

void GeoZoneManagerTest::_testSettings()
{
    GeoZoneSettings *const settings = SettingsManager::instance()->geoZoneSettings();
    const QList<Fact*> facts = { settings->showZones(), settings->approachDistance(), settings->staleDays(),
                                 settings->feedRefreshHours(), settings->liveFeedRefreshMinutes() };
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
    QVERIFY(file.open(QIODevice::ReadWrite) && file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-10), QFileDevice::FileModificationTime));
    file.close();
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.loadFile(fileName, errorString), qPrintable(errorString));

    // The manager visibility is the setting
    QSignalSpy visibleSpy(&manager, &GeoZoneManager::visibleChanged);
    settings->showZones()->setRawValue(false);
    QVERIFY(!manager.visible());
    QCOMPARE(visibleSpy.count(), 1);
    manager.setVisible(true);
    QVERIFY(settings->showZones()->rawValue().toBool());

    // Outdated data is reported at the configured age
    QVERIFY(manager.validityWarning().isEmpty());
    settings->staleDays()->setRawValue(7);
    QCOMPARE(manager.validityWarning(), QStringLiteral("zones.json: Data is 10 days old"));

    // Feeds are refreshed at the configured intervals
    GeoZoneSource feed(QStringLiteral("https://example.com/zones.json"), QStringLiteral("https://example.com/zones.json"));
    GeoZoneParser::Result result;
    result.format = GeoZoneParser::Format::ED318;
    feed.setResult(result, QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc());
    settings->feedRefreshHours()->setRawValue(6);
    QCOMPARE(feed.refreshIntervalSecs(), 6 * 3600);
    result.format = GeoZoneParser::Format::FaaTfr;
    feed.setResult(result, QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc());
    settings->liveFeedRefreshMinutes()->setRawValue(30);
    QCOMPARE(feed.refreshIntervalSecs(), 30 * 60);

    // Approach alerts at the configured distance
    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.evaluate({ center.atDistanceAndAzimuth(500, 0) });
    QCOMPARE(alertSpy.count(), 0);
    settings->approachDistance()->setRawValue(250);
    monitor.evaluate({ center.atDistanceAndAzimuth(500, 0) });
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY(alertSpy.last()[0].toString().startsWith(QStringLiteral("Approaching geozone P")));
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
    QCOMPARE(manager.zones()->count(), zoneCount);

    QVERIFY(manager.labelZones(QGeoRectangle()).isEmpty());

    // Only zones centered in the region are labelled
    const QGeoRectangle smallRegion(QGeoCoordinate(47.005, 7.995), QGeoCoordinate(46.995, 8.035));
    QCOMPARE(manager.labelZones(smallRegion).count(), 4);

    // The limit keeps the most restrictive zones
    const QList<QObject*> labels = manager.labelZones(manager.boundingBox());
    QCOMPARE(labels.count(), GeoZoneManager::maxLabelCount);
    for (const QObject *object : labels) {
        QCOMPARE(qobject_cast<const GeoZone*>(object)->restriction(), GeoZone::Prohibited);
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

    const auto identifiers = [](const QList<QObject*> &zones) {
        QStringList result;
        for (const QObject *zone : zones) {
            result.append(qobject_cast<const GeoZone*>(zone)->identifier());
        }
        return result;
    };

    // Overlapping zones at a point, the most restrictive first
    QCOMPARE(identifiers(manager.zonesAt(center.atDistanceAndAzimuth(500, 90))), QStringList({ QStringLiteral("GZW1"), QStringLiteral("LSZH") }));
    QCOMPARE(identifiers(manager.zonesAt(center)), QStringList({ QStringLiteral("LSZH") }));
    QVERIFY(manager.zonesAt(QGeoCoordinate(48, 9)).isEmpty());

    // Names or identifiers containing the text, those starting with it first, then the most restrictive
    QCOMPARE(identifiers(manager.searchZones(QStringLiteral("zurich"))), QStringList({ QStringLiteral("LSZH"), QStringLiteral("GZW1") }));
    QCOMPARE(identifiers(manager.searchZones(QStringLiteral("I"))), QStringList({ QStringLiteral("GZW1"), QStringLiteral("LSZH") }));
    QCOMPARE(identifiers(manager.searchZones(QStringLiteral("fa"))), QStringList({ QStringLiteral("FAR") }));
    QCOMPARE(manager.searchZones(QStringLiteral("zurich"), 1).count(), 1);
    QVERIFY(manager.searchZones(QStringLiteral("  ")).isEmpty());

    QSignalSpy showSpy(&manager, &GeoZoneManager::showZoneRequested);
    manager.showZone(manager.zones()->get(0));
    manager.showZone(&manager);
    QCOMPARE(showSpy.count(), 1);

    // Highlighted zones are marked on their map items
    GeoZoneMapModel model(&manager);
    model.setView(QGeoRectangle(center.atDistanceAndAzimuth(5000, 315), center.atDistanceAndAzimuth(5000, 135)), 12, false);
    QCOMPARE(model.circles()->count(), 2);
    QObject *const prisonZone = manager.searchZones(QStringLiteral("GZW1")).first();
    model.setHighlightedZones({ prisonZone });
    for (int i = 0; i < model.circles()->count(); i++) {
        const GeoZoneMapItem *const item = model.circles()->value<GeoZoneMapItem*>(i);
        QCOMPARE(item->highlighted(), item->zone() == prisonZone);
    }
}
