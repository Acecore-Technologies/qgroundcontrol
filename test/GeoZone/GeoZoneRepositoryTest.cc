/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRepositoryTest.h"
#include "GeoZoneAirac.h"
#include "GeoZoneAwareness.h"
#include "GeoZoneFeedCatalogue.h"
#include "GeoZoneSourceVersion.h"
#include "GeoZoneTestBase.h"
#include "GeoZoneTestHelpers.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QLocale>
#include <QtCore/QUuid>
#include <QtCore/QUrlQuery>

using namespace GeoZoneTestHelpers;

namespace {

/// Longest a download from the local test server, the load of its zones or another step of a test may take
constexpr int stepTimeoutMSecs = 10000;

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

/// @return The feature of the IGN test file as many times, each with its own identifier and area
QJsonArray wfsFeatures(const QJsonObject &collection, int count)
{
    QJsonArray features;
    for (int i = 0; i < count; i++) {
        QJsonObject feature = collection.value(QStringLiteral("features")).toArray().first().toObject();
        feature.insert(QStringLiteral("id"), QStringLiteral("carte_restriction_drones_lf.%1").arg(i));
        QJsonObject geometry = feature.value(QStringLiteral("geometry")).toObject();
        const double west = 2.0 + (i * 0.1);
        geometry.insert(QStringLiteral("coordinates"), QJsonArray{ QJsonArray{ QJsonArray{
            QJsonArray{ west, 48.0 }, QJsonArray{ west + 0.05, 48.0 }, QJsonArray{ west + 0.05, 48.05 }, QJsonArray{ west, 48.0 } } } });
        feature.insert(QStringLiteral("geometry"), geometry);
        features.append(feature);
    }
    return features;
}

/// @return Response of a WFS limiting its responses to pageSize features, to a GetFeature request from its STARTINDEX
QByteArray wfsPage(const QJsonObject &collection, const QJsonArray &allFeatures, const QByteArray &request, int pageSize)
{
    const QUrlQuery query(QUrl(QString::fromLatin1(request.split(' ').value(1))));
    const int start = query.queryItemValue(QStringLiteral("STARTINDEX")).toInt();
    QJsonObject page = collection;
    QJsonArray features;
    for (int i = start; (i < (start + pageSize)) && (i < allFeatures.count()); i++) {
        features.append(allFeatures[i]);
    }
    page.insert(QStringLiteral("features"), features);
    page.insert(QStringLiteral("numberMatched"), allFeatures.count());
    page.insert(QStringLiteral("totalFeatures"), allFeatures.count());
    page.insert(QStringLiteral("numberReturned"), features.count());
    return QJsonDocument(page).toJson(QJsonDocument::Compact);
}

} // namespace

void GeoZoneRepositoryTest::_testManagerLoadPersistClear()
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
        manager.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
        QCOMPARE(manager.zoneCount(), 0);
        QCOMPARE(manager.sources()->count(), 0);

        QString errorString;
        QVERIFY2(manager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
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
        QVERIFY(QFile::exists(QDir(storageDir.path()).filePath(manager.repository()->storedFileName(ed318Source->key()))));
        QCOMPARE(storedCaches(), QStringList({ GeoZoneParseCache::path(manager.repository()->storedFileName(ed318Source->key())) }));

        // A failed load keeps the current zones
        QVERIFY(!manager.repository()->loadFile(sourceDir.filePath(QStringLiteral("missing.json")), errorString));
        QVERIFY(!errorString.isEmpty());
        QCOMPARE(manager.zoneCount(), 3);
        QCOMPARE(manager.sources()->count(), 1);

        QCOMPARE(manager.catalog().zonesInArea(QGeoRectangle(), GeoZonePolicy({ GeoZone::Prohibited })).count(), 1);
        const QGeoRectangle ireland(QGeoCoordinate(52.0, -9.0), QGeoCoordinate(51.0, -8.0));
        const QList<const GeoZone*> zonesInArea = manager.catalog().zonesInArea(ireland, GeoZonePolicy({ GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::USpace }));
        QCOMPARE(zonesInArea.count(), 1);
        QCOMPARE(zonesInArea[0]->name(), QStringLiteral("Control zone"));

        // Another file adds its zones, loading a file with the same name again replaces its zones
        QVERIFY2(manager.repository()->loadFile(ed269File, errorString), qPrintable(errorString));
        QCOMPARE(manager.sources()->count(), 2);
        QCOMPARE(manager.zoneCount(), 6);
        const QString previousStoredFile = manager.repository()->storedFileName(ed318Source->key());
        QVERIFY2(manager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
        QCOMPARE(manager.sources()->count(), 2);
        QCOMPARE(manager.zoneCount(), 6);
        QVERIFY(manager.repository()->storedFileName(ed318Source->key()) != previousStoredFile);
        QCOMPARE(storedFiles().count(), 3);
        QCOMPARE(storedCaches().count(), 2);

        // Zones of sources turned off are left out, loading the file again turns it on
        GeoZoneSource *const ed269Source = manager.sources()->value<GeoZoneSource*>(1);
        ed269Source->setEnabled(false);
        QCOMPARE(manager.zoneCount(), 3);
        QCOMPARE(manager.disabledSourceCount(), 1);
        QVERIFY(std::none_of(manager.zoneList().cbegin(), manager.zoneList().cend(), [&ed269File](const GeoZone *zone) { return zone->sourceKey() == GeoZoneLoader::fileKey(ed269File); }));
        QVERIFY2(manager.repository()->loadFile(ed269File, errorString), qPrintable(errorString));
        QVERIFY(ed269Source->enabled());
        QCOMPARE(manager.zoneCount(), 6);
        ed269Source->setEnabled(false);
        QCOMPARE(storedFiles().count(), 3);
    }

    // Unreferenced copies are removed on startup with their caches. The files of others in the folder, which the
    // store didn't name, are left alone.
    const QDir storage(storageDir.path());
    const QString strayCopy = QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json");
    const QStringList strayFiles = { strayCopy, GeoZoneParseCache::path(strayCopy) };
    const QStringList otherFiles = { QStringLiteral("stray.json"), QStringLiteral("stray.json.gzc"), QStringLiteral("notes.xml"), QStringLiteral("export.zip"),
                                     QStringLiteral("authorisations.json.20260615-120000.bak"), QStringLiteral("copy-of-") + strayCopy };
    for (const QString &name : strayFiles + otherFiles) {
        QFile file(storage.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    {
        // Stored copies are reloaded in order on startup
        GeoZoneManager manager(storageDir.path());
        manager.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
        QCOMPARE(manager.sources()->count(), 2);
        // Still turned off
        QVERIFY(!manager.sources()->value<GeoZoneSource*>(1)->enabled());
        QCOMPARE(manager.zoneCount(), 3);
        manager.sources()->value<GeoZoneSource*>(1)->setEnabled(true);
        QCOMPARE(manager.zoneCount(), 6);
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(0)->key(), GeoZoneLoader::fileKey(ed318File));
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(0)->formatName(), QStringLiteral("ED-318"));
        QCOMPARE(manager.sources()->value<GeoZoneSource*>(1)->key(), GeoZoneLoader::fileKey(ed269File));
        for (const QString &name : strayFiles) {
            QVERIFY2(!storage.exists(name), qPrintable(name));
        }
        for (const QString &name : otherFiles) {
            QVERIFY2(storage.exists(name), qPrintable(name));
            QVERIFY(QFile::remove(storage.filePath(name)));
        }
        QCOMPARE(storedCaches().count(), 2);

        // The parse cache goes with the stored copy
        manager.repository()->removeSource(0);
        QCOMPARE(manager.sources()->count(), 1);
        QCOMPARE(manager.zoneCount(), 3);
        QCOMPARE(storedFiles().count(), 2);
        QCOMPARE(storedCaches(), QStringList({ GeoZoneParseCache::path(manager.repository()->storedFileName(GeoZoneLoader::fileKey(ed269File))) }));

        manager.repository()->clear();
        QCOMPARE(manager.zoneCount(), 0);
        QVERIFY(manager.shapes().isEmpty());
        QVERIFY(storedFiles().isEmpty());
        QVERIFY(storedCaches().isEmpty());
    }

    GeoZoneManager manager(storageDir.path());
    manager.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QCOMPARE(manager.sources()->count(), 0);

    // The single file kept by earlier versions is migrated
    const QTemporaryDir legacyDir;
    QVERIFY(legacyDir.isValid());
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), legacyDir.filePath(QStringLiteral("geozones.json"))));
    QSettings settings;
    settings.setValue(QStringLiteral("GeoZoneManager/sourceName"), QStringLiteral("Legacy zones"));
    {
        GeoZoneManager legacyManager(legacyDir.path());
        legacyManager.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
        QCOMPARE(legacyManager.sources()->count(), 1);
        QCOMPARE(legacyManager.sources()->value<GeoZoneSource*>(0)->key(), QStringLiteral("Legacy zones"));
        QCOMPARE(legacyManager.zoneCount(), 3);
        QVERIFY(!settings.contains(QStringLiteral("GeoZoneManager/sourceName")));
    }
    GeoZoneManager migratedManager(legacyDir.path());
    migratedManager.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QCOMPARE(migratedManager.zoneCount(), 3);

    // Files with the same name in different folders are separate sources
    const QTemporaryDir otherDir;
    QVERIFY(otherDir.isValid());
    const QString sameName = otherDir.filePath(QStringLiteral("zones.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed269_test.json"), sameName));
    QString errorString;
    {
        GeoZoneManager pathManager(storageDir.path());
        QVERIFY2(pathManager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
        QVERIFY2(pathManager.repository()->loadFile(sameName, errorString), qPrintable(errorString));
        QCOMPARE(pathManager.sources()->count(), 2);
        QCOMPARE(pathManager.zoneCount(), 6);
        QCOMPARE(pathManager.sources()->value<GeoZoneSource*>(1)->name(), QStringLiteral("Test UASZoneVersion"));
        QVERIFY(QFileInfo(pathManager.sources()->value<GeoZoneSource*>(1)->key()).isAbsolute());
        pathManager.repository()->clear();
    }

    // A source stored by name by an earlier version is replaced when its file is loaded again
    {
        GeoZoneManager pathManager(storageDir.path());
        QVERIFY2(pathManager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
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
    legacyKeyManager.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QCOMPARE(legacyKeyManager.sources()->value<GeoZoneSource*>(0)->key(), QStringLiteral("zones.json"));
    QVERIFY2(legacyKeyManager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
    QCOMPARE(legacyKeyManager.sources()->count(), 1);
    QCOMPARE(legacyKeyManager.sources()->value<GeoZoneSource*>(0)->key(), GeoZoneLoader::fileKey(ed318File));
    QCOMPARE(legacyKeyManager.zoneCount(), 3);
}

void GeoZoneRepositoryTest::_testManagerStoreFailures()
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
        QVERIFY2(manager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
        storedFile = manager.repository()->storedFileName(key);
        QVERIFY(dir.exists(storedFile));
    }

    // A copy which can't be written keeps the previous one, which is still loaded at the next start
    const QFile::Permissions permissions = QFile::permissions(storageDir.path());
    {
        // Writable again whatever fails, or the folder and what follows it in the test process couldn't be removed
        const auto restorePermissions = qScopeGuard([&storageDir, permissions]() { (void) QFile::setPermissions(storageDir.path(), permissions); });
        QVERIFY(QFile::setPermissions(storageDir.path(), QFile::ReadOwner | QFile::ExeOwner));
        if (!QFileInfo(storageDir.path()).isWritable()) {
            GeoZoneManager manager(storageDir.path());
            QSignalSpy storageSpy(&manager, &GeoZoneManager::storageFailed);
            manager.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
            QCOMPARE(manager.zoneCount(), 3);
            QVERIFY2(manager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
            QCOMPARE(storageSpy.count(), 1);
            QCOMPARE(manager.repository()->storedFileName(key), storedFile);
        }
    }
    QCOMPARE(QFile::permissions(storageDir.path()), permissions);
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
        manager.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
        QCOMPARE(storageSpy.count(), 1);
        QCOMPARE(manager.sources()->count(), 0);
        QVERIFY2(manager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
        QVERIFY(manager.repository()->storedFileName(key).isEmpty());
        manager.repository()->clear();
    }
    QVERIFY(indexFile.open(QIODevice::ReadOnly));
    QCOMPARE(indexFile.readAll(), newerIndex);
    indexFile.close();
    QVERIFY(dir.exists(storedFile));
    QCOMPARE(dir.entryList({ QStringLiteral("*.json") }, QDir::Files).count(), 2);

    // An unreadable index is moved to a backup folder with the stored copies instead of losing them. Other files stay.
    QVERIFY(indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(indexFile.write(R"({ "version": 1, "sources": [)") > 0);
    indexFile.close();
    QFile otherFile(dir.filePath(QStringLiteral("notes.xml")));
    QVERIFY(otherFile.open(QIODevice::WriteOnly));
    otherFile.close();
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy storageSpy(&manager, &GeoZoneManager::storageFailed);
        manager.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
        QCOMPARE(storageSpy.count(), 1);
        QCOMPARE(manager.sources()->count(), 0);
    }
    QVERIFY(dir.entryList({ QStringLiteral("*.json") }, QDir::Files).isEmpty());
    const QStringList backups = dir.entryList({ QStringLiteral("backup-*") }, QDir::Dirs);
    QCOMPARE(backups.count(), 1);
    const QDir backupDir(dir.filePath(backups.first()));
    QVERIFY(backupDir.exists(storedFile));
    QVERIFY(backupDir.exists(QStringLiteral("sources.json")));
    QVERIFY(otherFile.exists());
    QVERIFY(!backupDir.exists(QStringLiteral("notes.xml")));
}

void GeoZoneRepositoryTest::_testStoredCopyKeptUntilIndexSaved()
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
    QString replacingFile;
    QByteArray savedIndex;
    {
        GeoZoneManager manager(storageDir.path());
        QVERIFY2(manager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
        storedFile = manager.repository()->storedFileName(key);
        QVERIFY(dir.exists(storedFile));
        QFile indexFile(indexPath);
        QVERIFY(indexFile.open(QIODevice::ReadOnly));
        savedIndex = indexFile.readAll();
        indexFile.close();

        // The index can't be written while a folder is in its place, files of the store can still be removed. The
        // copy which the index on disk names must then stay: it is the one the next start loads.
        QVERIFY(indexFile.remove());
        QVERIFY(dir.mkpath(QStringLiteral("sources.json/blocked")));
        QSignalSpy storageSpy(&manager, &GeoZoneManager::storageFailed);
        QVERIFY2(manager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
        QCOMPARE(storageSpy.count(), 1);
        replacingFile = manager.repository()->storedFileName(key);
        QVERIFY(replacingFile != storedFile);
        QVERIFY(dir.exists(replacingFile));
        QVERIFY(dir.exists(storedFile));
    }

    // As after a crash or a failed save: the index still names the previous copy, which is loaded
    QVERIFY(QDir(indexPath).removeRecursively());
    QFile indexFile(indexPath);
    QVERIFY(indexFile.open(QIODevice::WriteOnly));
    QCOMPARE(indexFile.write(savedIndex), savedIndex.size());
    indexFile.close();
    GeoZoneManager manager(storageDir.path());
    manager.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QCOMPARE(manager.zoneCount(), 3);
    QCOMPARE(manager.repository()->storedFileName(key), storedFile);
    QVERIFY(!dir.exists(replacingFile));

    // Once the index naming the new copy is saved, the one it replaces is removed, like the copy of a removed source
    QVERIFY2(manager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
    const QString newFile = manager.repository()->storedFileName(key);
    QVERIFY(newFile != storedFile);
    QVERIFY(dir.exists(newFile));
    QVERIFY(!dir.exists(storedFile));
    manager.repository()->clear();
    QVERIFY(!dir.exists(newFile));
    QVERIFY(!QFile::exists(indexPath));
}

void GeoZoneRepositoryTest::_testInterruptedCopiesRemoved()
{
    const QTemporaryDir storageDir;
    const QTemporaryDir sourceDir;
    QVERIFY(storageDir.isValid() && sourceDir.isValid());
    const QString ed318File = sourceDir.filePath(QStringLiteral("zones.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), ed318File));
    const QDir dir(storageDir.path());
    QString errorString;
    QString storedFile;
    {
        GeoZoneManager manager(storageDir.path());
        QVERIFY2(manager.repository()->loadFile(ed318File, errorString), qPrintable(errorString));
        storedFile = manager.repository()->storedFileName(GeoZoneLoader::fileKey(ed318File));
    }

    // What a copy into the store, a download or the import of a pack left when the application died: the partial copy
    // of the source's own copy, of another, and files of the downloads folder
    const QString otherCopy = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QStringList partials = { storedFile + QStringLiteral(".part"), otherCopy + QStringLiteral(".zip.part"),
                                   QStringLiteral("downloads/%1.part").arg(otherCopy), QStringLiteral("downloads/%1.json").arg(otherCopy) };
    // Not the store's
    const QStringList otherFiles = { QStringLiteral("notes.part"), QStringLiteral("notes.json.part") };
    QVERIFY(dir.mkpath(QStringLiteral("downloads")));
    for (const QString &name : partials + otherFiles) {
        QFile file(dir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("{ \"features\": [") > 0);
    }

    GeoZoneManager manager(storageDir.path());
    manager.repository()->init();
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(manager.zoneCount(), 3);
    for (const QString &name : partials) {
        QVERIFY2(!dir.exists(name), qPrintable(name));
    }
    for (const QString &name : otherFiles) {
        QVERIFY2(dir.exists(name), qPrintable(name));
    }
    QVERIFY(dir.exists(storedFile));
    manager.repository()->clear();
}

void GeoZoneRepositoryTest::_testIndexEntriesValidated_data()
{
    QTest::addColumn<QString>("member");
    QTest::addColumn<QString>("value");     // {outside} is the folder next to the store, {stored} the stored copy
    QTest::addColumn<bool>("loaded");

    QTest::newRow("as written") << QStringLiteral("file") << QStringLiteral("{stored}") << true;
    QTest::newRow("absolute path") << QStringLiteral("file") << QStringLiteral("{outside}/victim.json") << false;
    QTest::newRow("parent folder") << QStringLiteral("file") << QStringLiteral("../outside/victim.json") << false;
    QTest::newRow("subfolder") << QStringLiteral("file") << QStringLiteral("operator/victim.json") << false;
    QTest::newRow("windows path") << QStringLiteral("file") << QStringLiteral("..\\outside\\victim.json") << false;
    QTest::newRow("drive") << QStringLiteral("file") << QStringLiteral("C:victim.json") << false;
    QTest::newRow("parent") << QStringLiteral("file") << QStringLiteral("..") << false;
    // Read, like the copies of profiles made by hand, and left when its source is removed
    QTest::newRow("another file of the folder") << QStringLiteral("file") << QStringLiteral("notes.json") << true;
    QTest::newRow("no file") << QStringLiteral("file") << QString() << false;
    QTest::newRow("no key") << QStringLiteral("key") << QString() << false;
    QTest::newRow("url of another source") << QStringLiteral("url") << QStringLiteral("https://elsewhere.example.com/zones") << false;
}

void GeoZoneRepositoryTest::_testIndexEntriesValidated()
{
    QFETCH(QString, member);
    QFETCH(QString, value);
    QFETCH(bool, loaded);

    // A source stored by the application, and files which aren't its copies: next to the store, in a subfolder of it
    // and in the store itself
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir root(dir.path());
    QVERIFY(root.mkpath(QStringLiteral("store/operator")) && root.mkpath(QStringLiteral("outside")));
    const QString storePath = root.filePath(QStringLiteral("store"));
    const QString sourceFile = root.filePath(QStringLiteral("zones.json"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), sourceFile));
    const QStringList victims = { root.filePath(QStringLiteral("outside/victim.json")), root.filePath(QStringLiteral("store/operator/victim.json")),
                                  root.filePath(QStringLiteral("store/notes.json")) };
    for (const QString &victim : victims) {
        QVERIFY(QFile::copy(sourceFile, victim));
    }
    QString errorString;
    QString storedFile;
    {
        GeoZoneManager manager(storePath);
        QVERIFY2(manager.repository()->loadFile(sourceFile, errorString), qPrintable(errorString));
        storedFile = manager.repository()->storedFileName(GeoZoneLoader::fileKey(sourceFile));
    }

    // The index as someone else may have written it
    QFile indexFile(QDir(storePath).filePath(QStringLiteral("sources.json")));
    QVERIFY(indexFile.open(QIODevice::ReadOnly));
    QJsonObject index = QJsonDocument::fromJson(indexFile.readAll()).object();
    indexFile.close();
    QJsonArray sources = index.value(QStringLiteral("sources")).toArray();
    QCOMPARE(sources.count(), 1);
    QJsonObject entry = sources[0].toObject();
    entry.insert(member, QString(value).replace(QStringLiteral("{outside}"), root.filePath(QStringLiteral("outside"))).replace(QStringLiteral("{stored}"), storedFile));
    index.insert(QStringLiteral("sources"), QJsonArray{ entry });
    QVERIFY(indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(indexFile.write(QJsonDocument(index).toJson()) > 0);
    indexFile.close();

    // Its entry is only read when it names a file of the store, which is only removed with the source when the store
    // named it
    GeoZoneManager manager(storePath);
    manager.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QCOMPARE(manager.sources()->count(), loaded ? 1 : 0);
    QCOMPARE(manager.zoneCount(), loaded ? 3 : 0);
    manager.repository()->clear();
    for (const QString &victim : victims) {
        QVERIFY2(QFile::exists(victim), qPrintable(victim));
    }
}

void GeoZoneRepositoryTest::_testManagerBackgroundLoad()
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
    manager.repository()->addFile(ed318File);
    QVERIFY(manager.repository()->busy());
    QVERIFY(addedSpy.wait(10000));
    QVERIFY(!manager.repository()->busy());
    QCOMPARE(manager.zoneCount(), 3);

    // Only the most recent load of a file is applied, a file with the same name in another folder is another source
    addedSpy.clear();
    manager.repository()->addFile(ed318File);
    manager.repository()->addFile(ed318File);
    manager.repository()->addFile(ed269File);
    QTRY_VERIFY(!manager.repository()->busy());
    // The unchanged file isn't added again
    QCOMPARE(addedSpy.count(), 1);
    QCOMPARE(manager.sources()->count(), 2);
    QCOMPARE(manager.sources()->value<GeoZoneSource*>(1)->formatName(), QStringLiteral("ED-269"));

    // XML files are recognized by their content, like the files the dialog offers
    const QString aixmFile = sourceDir.filePath(QStringLiteral("zones.xml"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/uk_aixm_test.xml"), aixmFile));
    addedSpy.clear();
    manager.repository()->addFile(aixmFile);
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(manager.sources()->count(), 3);
    QCOMPARE(manager.sources()->value<GeoZoneSource*>(2)->formatName(), QStringLiteral("AIXM 5.1"));
    QCOMPARE(manager.sources()->value<GeoZoneSource*>(2)->zoneCount(), 3);

    // Removing all sources drops loads in progress
    manager.repository()->addFile(ed318File);
    manager.repository()->clear();
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(manager.sources()->count(), 0);

    // Files added together are reported once, with the reasons of those which failed
    const QString brokenFile = sourceDir.filePath(QStringLiteral("broken.json"));
    QFile broken(brokenFile);
    QVERIFY(broken.open(QIODevice::WriteOnly));
    QVERIFY(broken.write("{ \"broken\": ") > 0);
    broken.close();
    QSignalSpy finishedSpy(&manager, &GeoZoneManager::fileOperationFinished);
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);
    manager.repository()->addFiles({ ed318File, ed269File, brokenFile, aixmFile });
    QVERIFY(finishedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(finishedSpy.first().at(0).toBool(), false);
    const QString summary = finishedSpy.first().at(1).toString();
    QVERIFY2(summary.startsWith(QStringLiteral("3 geozone file(s) added, 1 failed:\nbroken.json: ")), qPrintable(summary));
    QCOMPARE(manager.sources()->count(), 3);
    finishedSpy.clear();
    manager.repository()->addFiles({ brokenFile });
    QVERIFY(failedSpy.wait(10000));
    QCOMPARE(finishedSpy.count(), 0);
    manager.repository()->clear();
    QTRY_VERIFY(!manager.repository()->busy());

    // Feeds keep their URL as key and can be refreshed
    const QString feedUrl = QUrl::fromLocalFile(ed318File).toString();
    addedSpy.clear();
    manager.repository()->addUrl(feedUrl);
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    GeoZoneSource *const feed = manager.repository()->source(feedUrl);
    QVERIFY(feed);
    QVERIFY(feed->isFeed());
    QCOMPARE(feed->url(), feedUrl);
    QCOMPARE(manager.zoneCount(), 3);
    // The user's file is read in place, never removed
    QVERIFY(QFile::exists(ed318File));
    const QDateTime now = environment.now();
    QVERIFY(!manager.repository()->scheduler()->isDue(*feed, now));
    QVERIFY(manager.repository()->scheduler()->isDue(*feed, now.addSecs(25 * 3600)));

    // A feed can be refreshed on its own
    manager.repository()->refreshSource(feedUrl);
    QVERIFY(feed->busy());
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(feed->errorString().isEmpty());
    QCOMPARE(manager.zoneCount(), 3);

    // A failed refresh keeps the previous data
    QVERIFY(QFile::remove(ed318File));
    manager.repository()->refreshFeeds();
    QVERIFY(feed->busy());
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(!feed->busy());
    QVERIFY(!feed->errorString().isEmpty());
    QCOMPARE(manager.zoneCount(), 3);

    // Stored sources load in the background in their original order
    GeoZoneManager restarted(storageDir.path());
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Background);
    QCOMPARE(restarted.sources()->count(), 1);
    QVERIFY(restarted.repository()->busy());
    QTRY_VERIFY(!restarted.repository()->busy());
    QCOMPARE(restarted.zoneCount(), 3);
    QVERIFY(restarted.repository()->source(feedUrl));
    QVERIFY(restarted.repository()->source(feedUrl)->isFeed());
}

void GeoZoneRepositoryTest::_testFeedCompanions()
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
        manager.repository()->addUrl(url);
        QTRY_VERIFY_WITH_TIMEOUT(manager.zoneCount() > 0 && !manager.repository()->busy(), 10000);
        QCOMPARE(scheduled(manager), 1);
        QCOMPARE(detailRequests(), 1);

        // Unchanged NOTAMs aren't downloaded again
        manager.repository()->refreshFeeds();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.repository()->busy(), 10000);
        QCOMPARE(server.requestedPaths.count("/geoserver/tfr.json"), 2);
        QCOMPARE(detailRequests(), 1);
        QCOMPARE(scheduled(manager), 1);
    }

    // After a restart the stored details apply and are reused
    GeoZoneManager restarted(dir.path());
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QCOMPARE(scheduled(restarted), 1);
    restarted.repository()->refreshFeeds();
    QTRY_VERIFY_WITH_TIMEOUT(!restarted.repository()->busy(), 10000);
    QCOMPARE(detailRequests(), 1);
    QCOMPARE(scheduled(restarted), 1);
}

void GeoZoneRepositoryTest::_testFeedFetchFormatCompanions()
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

void GeoZoneRepositoryTest::_testCompanionsWriteFailure()
{
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/list", R"({ "details": true, "features": [ { "detail": "a" } ] })");
    server.files.insert("/details/a", "detail of a");
    server.delays.insert("/details/a", 500);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneDownloader downloader;
    downloader.setDirectory(dir.path());
    const DetailFormat format;

    GeoZoneFeedFetch::Request request;
    request.url = server.url(QStringLiteral("/list"));
    request.formats = { &format };
    GeoZoneFeedFetch fetch(&downloader, request);
    std::optional<GeoZoneFeedFetch::Result> result;
    (void) connect(&fetch, &GeoZoneFeedFetch::finished, &fetch, [&result](const GeoZoneFeedFetch::Result &fetchResult) { result = fetchResult; });
    fetch.start();

    // While the companion is downloaded, the folder of the downloads stops taking new files: the document can't be
    // written with its companion
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount, 2, stepTimeoutMSecs);
    const QFile::Permissions permissions = QFile::permissions(dir.path());
    const auto restore = qScopeGuard([&dir, permissions]() { (void) QFile::setPermissions(dir.path(), permissions); });
    QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::ExeOwner));
    if (QFileInfo(dir.path()).isWritable()) {
        QSKIP("The folder can't be made read-only for this user");
    }
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), stepTimeoutMSecs);

    // The fetch fails rather than passing a document which wasn't written whole
    QCOMPARE(result->status, GeoZoneFeedFetch::Result::Status::Failed);
    QVERIFY2(result->errorString.startsWith(QStringLiteral("The download could not be saved")), qPrintable(result->errorString));
    QVERIFY(result->filePath.isEmpty());
}

void GeoZoneRepositoryTest::_testFeedCancellation()
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
    // Until each request was answered or abandoned, so that no delayed response is still to come
    const auto waitPastResponses = [&server]() { return QTest::qWaitFor([&server]() { return server.settled(); }, 10000); };

    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);
    manager.repository()->addUrl(url);
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(manager.zoneCount(), 3);

    // Removing a source while it is refreshed aborts the download, the response arriving later is dropped
    int requests = server.requestCount;
    manager.repository()->refreshFeeds();
    QTRY_COMPARE(server.requestCount, requests + 1);
    QVERIFY(manager.repository()->busy());
    manager.repository()->removeSource(0);
    QVERIFY(!manager.repository()->busy());
    QVERIFY(waitPastResponses());
    QCOMPARE(server.abandonedCount, 1);
    QCOMPARE(manager.sources()->count(), 0);
    QCOMPARE(manager.zoneCount(), 0);
    QCOMPARE(addedSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(storage.entryList({ QStringLiteral("*.json") }, QDir::Files).isEmpty());
    QVERIFY(downloads.entryList(QDir::Files).isEmpty());

    // Adding the feed again while it downloads replaces the download, only the last one is loaded
    requests = server.requestCount;
    manager.repository()->addUrl(url);
    QTRY_COMPARE(server.requestCount, requests + 1);
    manager.repository()->addUrl(url);
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(waitPastResponses());
    QCOMPARE(server.abandonedCount, 2);
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
        closing.repository()->addUrl(url);
        QTRY_COMPARE(server.requestCount, requests + 1);
    }
    QVERIFY(waitPastResponses());
    QCOMPARE(server.abandonedCount, 3);
    QVERIFY(QDir(closingDir.path()).entryList({ QStringLiteral("*.json") }, QDir::Files).isEmpty());

    // A load canceled before it runs neither parses nor stores the download
    const QString downloadPath = storage.filePath(QStringLiteral("download.part"));
    QVERIFY(QFile::copy(QStringLiteral(":/unittest/ed318_test.json"), downloadPath));
    GeoZoneLoader::Job job = GeoZoneLoader::feedJob(url);
    job.sourcePath = downloadPath;
    job.storedFileName = GeoZoneStore::newFileName(downloadPath);
    job.downloaded = true;
    job.cancelToken.cancel();
    const GeoZoneLoader::Result loadResult = GeoZoneLoader::run(job, GeoZoneStore(storageDir.path()));
    QVERIFY(loadResult.cancelled);
    QVERIFY(!loadResult.stored);
    QCOMPARE(loadResult.result.format, GeoZoneParser::Format::Unknown);
    QVERIFY(!QFile::exists(downloadPath));
    QVERIFY(!storage.exists(job.storedFileName));
}

void GeoZoneRepositoryTest::_testRefreshScheduler()
{
    ScopedEnvironment environment;

    // How long a response stays current, from its headers
    QCOMPARE(GeoZoneRefreshScheduler::maxAgeSecsOf({ { "cache-control", "public, max-age=600" } }), 600);
    QCOMPARE(GeoZoneRefreshScheduler::maxAgeSecsOf({ { "cache-control", "max-age=600" }, { "expires", "Sat, 31 Oct 2026 12:00:00 GMT" }, { "age", "100" } }), 500);
    QCOMPARE(GeoZoneRefreshScheduler::maxAgeSecsOf({ { "expires", "Sat, 31 Oct 2026 12:00:00 GMT" }, { "date", "Sat, 31 Oct 2026 11:00:00 GMT" } }), 3600);
    QCOMPARE(GeoZoneRefreshScheduler::maxAgeSecsOf({ { "cache-control", "no-cache" } }), -1);
    QCOMPARE(GeoZoneRefreshScheduler::maxAgeSecsOf({ { "cache-control", "max-age=0" }, { "expires", "Sat, 31 Oct 2026 12:00:00 GMT" }, { "date", "Sat, 31 Oct 2026 11:00:00 GMT" } }), -1);
    QCOMPARE(GeoZoneRefreshScheduler::maxAgeSecsOf({ { "expires", "0" } }), -1);

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
    const auto settle = [&manager]() { return QTest::qWaitFor([&manager]() { return !manager.repository()->busy(); }, 10000); };
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    manager.repository()->addUrl(url);
    QVERIFY(addedSpy.wait(10000));
    QVERIFY(settle());
    GeoZoneSource *const feed = manager.repository()->source(url);
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
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(sourceDir, QStringLiteral("zones.json"), QJsonArray{
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
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QVERIFY(restarted.repository()->source(url));
    QCOMPARE(restarted.repository()->scheduler()->refreshIntervalSecs(*restarted.repository()->source(url)), 7200);
}

void GeoZoneRepositoryTest::_testFeedCatalogueOverlays()
{
    ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/catalogue", R"({ "version": 1, "feeds": [ { "id": "maker", "name": "Maker zones", "url": "https://maker.example.com/zones.json" },
                                                                    { "id": "lgs", "enabled": false } ] })");
    server.etags.insert("/catalogue", "\"c1\"");
    const QString catalogueUrl = server.url(QStringLiteral("/catalogue"));
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    const QString filePath = QDir(storageDir.path()).filePath(QStringLiteral("feeds.json"));
    const auto writeCatalogue = [&filePath](const QByteArray &content) {
        QFile file(filePath);
        return file.open(QIODevice::WriteOnly) && (file.write(content) == content.size());
    };
    const auto presetNames = []() {
        QStringList names;
        for (const QVariant &preset : GeoZoneProviders::feedPresets()) {
            names.append(preset.toMap().value(QStringLiteral("name")).toString());
        }
        return names;
    };
    // The catalogue file of the geozone data folder replaces a built-in feed
    QVERIFY(writeCatalogue(R"json({ "feeds": [ { "id": "foca", "name": "Switzerland (operator mirror)", "url": "https://mirror.example.com/ch.json" } ] })json"));

    {
        GeoZoneManager manager(storageDir.path());
        GeoZoneFeedCatalogueLoader *const catalogue = manager.repository()->feedCatalogue();
        QSignalSpy changedSpy(&manager, &GeoZoneManager::feedCatalogueChanged);
        catalogue->setUrl(catalogueUrl);
        manager.repository()->init();
        QVERIFY2(presetNames().contains(QStringLiteral("Switzerland (operator mirror)")), qPrintable(catalogue->warning() + catalogue->statusText() + presetNames().join(QLatin1Char('|'))));
        QVERIFY(!presetNames().contains(QStringLiteral("Switzerland (FOCA)")));
        QVERIFY(changedSpy.count() > 0);

        // The catalogue of the URL is downloaded and applied below the file
        QTRY_VERIFY(presetNames().contains(QStringLiteral("Maker zones")));
        QVERIFY(!presetNames().contains(QStringLiteral("Latvia (LGS)")));
        // which is told, since a downloaded catalogue takes a built-in feed away
        QCOMPARE(catalogue->warning(), QStringLiteral("The feed catalogue of %1 turns off 1 built-in feed(s): Latvia (LGS)").arg(catalogueUrl));
        QVERIFY(catalogue->statusText().contains(QStringLiteral("feeds.json")));
        QCOMPARE(server.requestCount, 1);
        // Downloaded again after the refresh interval, confirmed with its validators
        catalogue->check();
        QCOMPARE(server.requestCount, 1);
        environment.addSecs(24 * 3600);
        catalogue->check();
        QTRY_COMPARE(server.requestCount, 2);
        QVERIFY(TestFeedServer::header(server.requests.constLast(), "if-none-match").contains("c1"));
        QTRY_COMPARE(catalogue->checkedAt(), environment.now());

        // An invalid file is ignored with a warning: the built-in feed is back, the downloaded catalogue stays
        QVERIFY(writeCatalogue("{ \"feeds\": [ "));
        catalogue->check();
        QVERIFY2(catalogue->warning().contains(QStringLiteral("feeds.json is ignored: Invalid JSON")), qPrintable(catalogue->warning()));
        QVERIFY(presetNames().contains(QStringLiteral("Switzerland (FOCA)")));
        QVERIFY(presetNames().contains(QStringLiteral("Maker zones")));

        // An invalid download is ignored, its last valid copy stays in use
        server.files.insert("/catalogue", R"({ "version": 9, "feeds": [] })");
        server.etags.insert("/catalogue", "\"c2\"");
        catalogue->check(true);
        QTRY_VERIFY(catalogue->warning().contains(QStringLiteral("is ignored, its previous copy is used")));
        QVERIFY(presetNames().contains(QStringLiteral("Maker zones")));
    }
    // The built-in catalogue is used again once the repository is gone
    QVERIFY(presetNames().contains(QStringLiteral("Latvia (LGS)")));
    QVERIFY(!presetNames().contains(QStringLiteral("Maker zones")));

    // Offline at the next start, the copy of the downloaded catalogue is used
    QFile::remove(filePath);
    server.close();
    {
        GeoZoneManager manager(storageDir.path());
        GeoZoneFeedCatalogueLoader *const catalogue = manager.repository()->feedCatalogue();
        catalogue->setUrl(catalogueUrl);
        environment.addSecs(24 * 3600);
        manager.repository()->init();
        QVERIFY(presetNames().contains(QStringLiteral("Maker zones")));
        QTRY_VERIFY(catalogue->warning().contains(QStringLiteral("couldn't be downloaded")));
        QVERIFY(presetNames().contains(QStringLiteral("Maker zones")));
        // Without a URL, its copy is dropped
        catalogue->setUrl(QString());
        QVERIFY(!presetNames().contains(QStringLiteral("Maker zones")));
        QVERIFY(catalogue->warning().isEmpty());
    }
}

void GeoZoneRepositoryTest::_testFeedCatalogueUrlRules()
{
    const auto presetNames = []() {
        QStringList names;
        for (const QVariant &preset : GeoZoneProviders::feedPresets()) {
            names.append(preset.toMap().value(QStringLiteral("name")).toString());
        }
        return names;
    };
    const QByteArray makerCatalogue = R"({ "feeds": [ { "id": "maker", "name": "Maker zones", "url": "https://maker.example.com/zones.json" } ] })";
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    const QString cachePath = QDir(storageDir.path()).filePath(QString::fromLatin1(GeoZoneFeedCatalogueLoader::cacheFileName));
    GeoZoneDownloader downloader;
    downloader.setDirectory(QDir(storageDir.path()).filePath(QStringLiteral("downloads")));

    // A catalogue changes the feeds offered: it isn't downloaded over http from another computer, where anyone on the
    // way could change it, and a copy downloaded that way before isn't used
    const QString plainUrl = QStringLiteral("http://catalogue.invalid/feeds.json");
    {
        QFile cache(cachePath);
        QVERIFY(cache.open(QIODevice::WriteOnly));
        QVERIFY(cache.write(QJsonDocument(QJsonObject{ { QStringLiteral("url"), plainUrl }, { QStringLiteral("checkedAt"), QStringLiteral("2026-11-01T00:00:00Z") },
                                                       { QStringLiteral("catalogue"), QString::fromUtf8(makerCatalogue) } }).toJson()) > 0);
    }
    {
        GeoZoneFeedCatalogueLoader loader(storageDir.path(), &downloader);
        loader.setUrl(plainUrl);
        loader.init();
        QCOMPARE(downloader.count(), 0);
        QVERIFY2(loader.warning().startsWith(QStringLiteral("The feed catalogue %1 is not downloaded: Its address must start with https://").arg(plainUrl)), qPrintable(loader.warning()));
        QVERIFY(!presetNames().contains(QStringLiteral("Maker zones")));
        QVERIFY(!QFile::exists(cachePath));
        // nor from addresses which aren't web addresses
        loader.setUrl(QStringLiteral("ftp://catalogue.invalid/feeds.json"));
        QCOMPARE(downloader.count(), 0);
        QVERIFY2(loader.warning().contains(QStringLiteral("It is not an https address.")), qPrintable(loader.warning()));
        // An https address is downloaded, and so is an http address of this computer, reached without a network
        for (const QString &url : { QStringLiteral("https://127.0.0.1:1/feeds.json"), QStringLiteral("http://localhost:1/feeds.json"), QStringLiteral("http://[::1]:1/feeds.json") }) {
            loader.setUrl(url);
            QCOMPARE(downloader.count(), 1);
            QVERIFY2(!loader.warning().contains(QStringLiteral("is not downloaded")), qPrintable(loader.warning()));
            loader.setUrl(QString());
            QCOMPARE(downloader.count(), 0);
            QCOMPARE(loader.warning(), QString());
        }
    }

    // A server on this computer is reached without a network, over http too. The built-in feeds and services a
    // downloaded catalogue turns off are named, those it adds or replaces aren't
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/catalogue", R"json({ "feeds": [ { "id": "maker", "name": "Maker zones", "url": "https://maker.example.com/zones.json" },
                                                          { "id": "foca", "name": "Switzerland (maker mirror)", "url": "https://maker.example.com/ch.json" },
                                                          { "id": "lgs", "enabled": false }, { "id": "faa-tfr", "enabled": false }, { "id": "unknown", "enabled": false } ],
                                              "services": [ { "id": "openaip", "enabled": false } ] })json");
    {
        const QString url = server.url(QStringLiteral("/catalogue"));
        GeoZoneFeedCatalogueLoader loader(storageDir.path(), &downloader);
        loader.setUrl(url);
        loader.init();
        QTRY_VERIFY_WITH_TIMEOUT(presetNames().contains(QStringLiteral("Maker zones")), stepTimeoutMSecs);
        QVERIFY(presetNames().contains(QStringLiteral("Switzerland (maker mirror)")));
        QVERIFY(!presetNames().contains(QStringLiteral("Latvia (LGS)")));
        QCOMPARE(loader.warning(), QStringLiteral("The feed catalogue of %1 turns off 3 built-in feed(s): Latvia (LGS), United States TFRs (FAA), Airspaces (OpenAIP, unofficial)").arg(url));
    }
    QVERIFY(presetNames().contains(QStringLiteral("Latvia (LGS)")));
}

void GeoZoneRepositoryTest::_testRefreshAfterOffline()
{
    using Network = GeoZoneRefreshScheduler::Network;
    ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const quint16 port = server.serverPort();
    server.files.insert("/zones", server.body);
    const QString url = server.url(QStringLiteral("/zones"));
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());

    {
        GeoZoneManager manager(storageDir.path());
        GeoZoneSourceRepository *const repository = manager.repository();
        const auto settle = [repository]() { return QTest::qWaitFor([repository]() { return !repository->busy(); }, 10000); };
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        repository->addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QVERIFY(settle());
        GeoZoneSource *const feed = repository->source(url);
        GeoZoneRefreshScheduler *const scheduler = repository->scheduler();
        QSignalSpy dueSpy(scheduler, &GeoZoneRefreshScheduler::refreshDue);
        QCOMPARE(feed->nextCheck(), environment.now().addSecs(24 * 3600));
        QVERIFY2(feed->nextCheckText().startsWith(QStringLiteral("next check ")), qPrintable(feed->nextCheckText()));

        // The refresh falls due while no server can be reached: it fails, keeping the zones, and is retried later
        server.close();
        environment.addSecs(24 * 3600);
        scheduler->check();
        QCOMPARE(dueSpy.count(), 1);
        QVERIFY(settle());
        QVERIFY(!feed->errorString().isEmpty());
        QVERIFY(scheduler->unreachable(url));
        QCOMPARE(manager.zoneCount(), 3);
        for (int failure = 2; failure <= 4; failure++) {
            environment.setNow(scheduler->retryAt(url));
            scheduler->check();
            QVERIFY(settle());
            QCOMPARE(dueSpy.count(), failure);
        }
        // Retried later and later, but at least every 15 minutes since the servers weren't reached
        QVERIFY(environment.now().secsTo(scheduler->retryAt(url)) >= GeoZoneRefreshScheduler::maxUnreachableRetrySecs * 0.8);
        QVERIFY(environment.now().secsTo(scheduler->retryAt(url)) <= GeoZoneRefreshScheduler::maxUnreachableRetrySecs * 1.2);

        // Known to be offline, the refresh waits for the network instead of failing again
        repository->setNetwork(Network::Offline);
        QVERIFY(feed->nextCheckText().startsWith(QStringLiteral("next check ")));
        environment.setNow(scheduler->retryAt(url).addSecs(60));
        scheduler->check();
        QCOMPARE(dueSpy.count(), 4);
        QVERIFY(feed->waitingForNetwork());
        QCOMPARE(feed->nextCheckText(), QStringLiteral("next check when online"));

        // Back online, it is refreshed at once rather than at its next retry
        QVERIFY(server.listen(QHostAddress::LocalHost, port));
        environment.setNow(scheduler->retryAt(url).addSecs(-60));
        repository->setNetwork(Network::Online);
        QCOMPARE(dueSpy.count(), 5);
        QVERIFY(settle());
        QVERIFY2(feed->errorString().isEmpty(), qPrintable(feed->errorString()));
        QVERIFY(!scheduler->unreachable(url));
        QVERIFY(!scheduler->retryAt(url).isValid());
        QCOMPARE(feed->checkedAt(), environment.now());
        QVERIFY(!feed->waitingForNetwork());

        // Server errors keep their retries when the network comes back
        server.files.remove("/zones");
        server.handler = [](const QByteArray &, QByteArray &status, QByteArray &, QByteArray &) { status = "503 Service Unavailable"; return true; };
        environment.addSecs(24 * 3600);
        scheduler->check();
        QVERIFY(settle());
        QCOMPARE(dueSpy.count(), 6);
        QVERIFY(!scheduler->unreachable(url));
        QVERIFY(scheduler->retryAt(url) > environment.now());
        repository->setNetwork(Network::Offline);
        repository->setNetwork(Network::Online);
        QCOMPARE(dueSpy.count(), 6);
        server.handler = nullptr;
        server.files.insert("/zones", server.body);
    }

    // The next start finds the refresh overdue: offline, it waits for the network, then is refreshed once online
    environment.addSecs(2 * 24 * 3600);
    GeoZoneManager manager(storageDir.path());
    GeoZoneSourceRepository *const repository = manager.repository();
    const int requests = server.requestCount;
    repository->setNetwork(Network::Offline);
    repository->init();
    QTRY_VERIFY(!repository->busy() && (manager.sources()->count() == 1));
    GeoZoneSource *const feed = repository->source(url);
    QTRY_VERIFY(feed->waitingForNetwork());
    QCOMPARE(server.requestCount, requests);
    QCOMPARE(feed->nextCheckText(), QStringLiteral("next check when online"));
    repository->setNetwork(Network::Online);
    QTRY_COMPARE(server.requestCount, requests + 1);
    QTRY_VERIFY(!repository->busy());
    QCOMPARE(feed->checkedAt(), environment.now());
    QVERIFY(feed->errorString().isEmpty());
}

void GeoZoneRepositoryTest::_testRetryWhenOnline()
{
    using Network = GeoZoneRefreshScheduler::Network;
    ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const quint16 port = server.serverPort();
    server.files.insert("/zones", server.body);
    const QString url = server.url(QStringLiteral("/zones"));
    const QString areaUrl = server.url(QStringLiteral("/zones?bbox={bbox}"));
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    GeoZoneManager manager(storageDir.path());
    GeoZoneSourceRepository *const repository = manager.repository();
    GeoZoneRefreshScheduler *const scheduler = repository->scheduler();
    const auto settle = [repository]() { return QTest::qWaitFor([repository]() { return !repository->busy(); }, stepTimeoutMSecs); };
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    repository->addUrl(url);
    QVERIFY(addedSpy.wait(stepTimeoutMSecs));
    repository->setVehiclePosition(QGeoCoordinate(47.0, 8.0));
    manager.addService(areaUrl, GeoZoneService().toVariantMap(), QString());
    QVERIFY(addedSpy.wait(stepTimeoutMSecs));
    QVERIFY(settle());
    const GeoZoneSource *const feed = repository->source(url);
    const GeoZoneSource *const areaFeed = repository->source(areaUrl);
    QVERIFY(feed);
    QVERIFY(areaFeed);
    const QDateTime addedAt = environment.now();

    // Without a connection, a refresh of the user fails an hour later, and so does the download of the area the
    // vehicle flew to: neither is due by the schedule
    server.close();
    environment.addSecs(3600);
    repository->refreshSource(url);
    const QGeoCoordinate away(47.5, 8.5);
    repository->setVehiclePosition(away);
    QVERIFY(settle());
    QVERIFY(scheduler->unreachable(url));
    QVERIFY(scheduler->unreachable(areaUrl));
    QVERIFY(!feed->errorString().isEmpty());
    QVERIFY(!areaFeed->area().contains(away));
    QCOMPARE(feed->checkedAt(), addedAt);

    // Both are tried again as soon as the network is back, rather than at the next refresh a day later
    repository->setNetwork(Network::Offline);
    QVERIFY(server.listen(QHostAddress::LocalHost, port));
    environment.addSecs(60);
    const int requests = server.requestCount;
    repository->setNetwork(Network::Online);
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount, requests + 2, stepTimeoutMSecs);
    QVERIFY(settle());
    QCOMPARE(feed->errorString(), QString());
    QCOMPARE(feed->checkedAt(), environment.now());
    QCOMPARE(areaFeed->errorString(), QString());
    QVERIFY(areaFeed->area().contains(away));
    QVERIFY(!scheduler->unreachable(url));
    QVERIFY(!scheduler->retryAt(url).isValid());
    QVERIFY(!scheduler->retryAt(areaUrl).isValid());
}

void GeoZoneRepositoryTest::_testReloadChanges()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
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
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 0), 300),
        _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), center, 400),
        _circleFeature(QStringLiteral("D"), QStringLiteral("CONDITIONAL"), center.atDistanceAndAzimuth(5000, 180), 300),
    }), errorString), qPrintable(errorString));
    QVERIFY2(source->changesText().startsWith(QStringLiteral("Updated ")) && source->changesText().endsWith(QStringLiteral(": 1 new, 1 changed, 1 removed")),
             qPrintable(source->changesText()));
    QCOMPARE(source->changesDetails(), QStringLiteral("New: D\nChanged: B\nRemoved: C"));
    QCOMPARE(source->changedZoneKeys().count(), 1);
    QJsonObject lastChanges = GeoZoneSourceRepository::sourceReport(*source).value(QStringLiteral("lastChanges")).toObject();
    QCOMPARE(lastChanges.value(QStringLiteral("addedCount")).toInt(), 1);
    QCOMPARE(lastChanges.value(QStringLiteral("changedCount")).toInt(), 1);
    QCOMPARE(lastChanges.value(QStringLiteral("removedCount")).toInt(), 1);
    QCOMPARE(lastChanges.value(QStringLiteral("changed")).toArray(), QJsonArray{ QStringLiteral("B") });
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 2);

    // Unchanged zones keep their alert state
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 0), 300),
        _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), center, 400),
    }), errorString), qPrintable(errorString));
    QVERIFY(source->changedZoneKeys().isEmpty());
    QVERIFY2(source->changesText().endsWith(QStringLiteral(": 1 removed")), qPrintable(source->changesText()));
    QCOMPARE(source->changesDetails(), QStringLiteral("Removed: D"));
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 2);

    // Reloading the same zones keeps the changes of the last reload which changed them
    const GeoZoneSourceChanges changes = source->changes();
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 0), 300),
        _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), center, 400),
    }), errorString), qPrintable(errorString));
    QCOMPARE(source->changes(), changes);

    // and so does the next start
    GeoZoneManager restarted(dir.filePath(QStringLiteral("storage")));
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    const GeoZoneSource *const restored = restarted.sources()->value<GeoZoneSource*>(0);
    QCOMPARE(restored->changes().removed, QStringList{ QStringLiteral("D") });
    QCOMPARE(restored->changes().at.toSecsSinceEpoch(), changes.at.toSecsSinceEpoch());
    lastChanges = GeoZoneSourceRepository::sourceReport(*restored).value(QStringLiteral("lastChanges")).toObject();
    QCOMPARE(lastChanges.value(QStringLiteral("removedCount")).toInt(), 1);
}

void GeoZoneRepositoryTest::_testAiracCycles_data()
{
    QTest::addColumn<QDate>("date");
    QTest::addColumn<QDate>("start");
    QTest::addColumn<QDate>("next");
    QTest::addColumn<QString>("name");

    QTest::newRow("epoch") << QDate(2020, 1, 2) << QDate(2020, 1, 2) << QDate(2020, 1, 30) << QStringLiteral("2001");
    QTest::newRow("before the epoch") << QDate(2019, 12, 31) << QDate(2019, 12, 5) << QDate(2020, 1, 2) << QStringLiteral("1913");
    QTest::newRow("long before") << QDate(2015, 1, 8) << QDate(2015, 1, 8) << QDate(2015, 2, 5) << QStringLiteral("1501");
    QTest::newRow("14th cycle of 2020") << QDate(2020, 12, 31) << QDate(2020, 12, 31) << QDate(2021, 1, 28) << QStringLiteral("2014");
    QTest::newRow("into the next year") << QDate(2021, 1, 27) << QDate(2020, 12, 31) << QDate(2021, 1, 28) << QStringLiteral("2014");
    QTest::newRow("first of 2021") << QDate(2021, 1, 28) << QDate(2021, 1, 28) << QDate(2021, 2, 25) << QStringLiteral("2101");
    QTest::newRow("first of 2024") << QDate(2024, 1, 25) << QDate(2024, 1, 25) << QDate(2024, 2, 22) << QStringLiteral("2401");
    QTest::newRow("last day of 2513") << QDate(2026, 1, 21) << QDate(2025, 12, 25) << QDate(2026, 1, 22) << QStringLiteral("2513");
    QTest::newRow("first of 2026") << QDate(2026, 1, 22) << QDate(2026, 1, 22) << QDate(2026, 2, 19) << QStringLiteral("2601");
    QTest::newRow("within 2611") << QDate(2026, 11, 1) << QDate(2026, 10, 29) << QDate(2026, 11, 26) << QStringLiteral("2611");
}

void GeoZoneRepositoryTest::_testAiracCycles()
{
    QFETCH(QDate, date);
    QFETCH(QDate, start);
    QFETCH(QDate, next);
    QFETCH(QString, name);

    QCOMPARE(GeoZoneAirac::cycleStart(date), start);
    QCOMPARE(GeoZoneAirac::nextCycleStart(date), next);
    QCOMPARE(GeoZoneAirac::cycleName(date), name);
    QCOMPARE(GeoZoneSourceVersion::airacCycleOf(date), name);
}

void GeoZoneRepositoryTest::_testAiracExpiry()
{
    ScopedEnvironment environment;
    environment.setNow(QDateTime(QDate(2026, 1, 21), QTime(23, 0), QTimeZone::UTC));
    const QString datasetUrl = QStringLiteral("https://example.org/d/UAS_AREA_1/EG_FULL_20251225_XML.zip");
    QCOMPARE(GeoZoneProviders::airacDatasetDate(datasetUrl), QDate(2025, 12, 25));
    QVERIFY(!GeoZoneProviders::airacDatasetDate(QStringLiteral("https://example.org/zones_20251225.zip")).isValid());

    // The data of a dataset published by AIRAC cycle is current during its cycle
    const QString url = QStringLiteral("https://example.org/page");
    GeoZoneSource source(url, url);
    GeoZoneParser::Result result;
    result.format = GeoZoneParser::Format::Aixm;
    source.setResult(result, environment.now(), environment.now());
    source.setDatasetUrl(datasetUrl);
    QVERIFY2(source.validityText().isEmpty(), qPrintable(source.validityText()));
    QVERIFY2(source.detailsText().contains(QStringLiteral("AIRAC 2513")), qPrintable(source.detailsText()));
    const QJsonObject airac = GeoZoneSourceRepository::sourceReport(source).value(QStringLiteral("airac")).toObject();
    QCOMPARE(airac.value(QStringLiteral("cycle")).toString(), QStringLiteral("2513"));
    QCOMPARE(airac.value(QStringLiteral("effective")).toString(), QStringLiteral("2025-12-25"));
    QCOMPARE(airac.value(QStringLiteral("ends")).toString(), QStringLiteral("2026-01-22"));

    // and outdated once the next cycle takes effect, a day after the download
    environment.setNow(QDateTime(QDate(2026, 1, 22), QTime(0, 0), QTimeZone::UTC));
    source.updateValidity(environment.now());
    QCOMPARE(source.validityText(), QStringLiteral("AIRAC cycle 2513 ended on 2026-01-22, a newer cycle is in effect"));

    // Data of the next cycle, downloaded before it takes effect, isn't
    source.setDatasetUrl(QStringLiteral("https://example.org/d/UAS_AREA_1/EG_FULL_20260219_XML.zip"));
    QVERIFY2(source.validityText().isEmpty(), qPrintable(source.validityText()));
}

void GeoZoneRepositoryTest::_testFeedNotModified()
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
        manager.repository()->addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.repository()->busy());
        GeoZoneSource *const feed = manager.repository()->source(url);
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
        manager.repository()->refreshFeeds();
        QTRY_VERIFY(!manager.repository()->busy());
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
        manager.repository()->refreshFeeds();
        QTRY_VERIFY(!manager.repository()->busy());
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
        manager.repository()->refreshFeeds();
        QTRY_VERIFY(!manager.repository()->busy());
        QCOMPARE(aboutToChangeSpy.count(), 0);
        QCOMPARE(feed->validTo(), _utc(QDate(2026, 9, 1), QTime(0, 0)));
        QCOMPARE(manager.zoneList().at(0)->sourceText(), QStringLiteral("Test GeoZones (issued 2026-08-20)"));

        // Changed zones are reloaded, without signaling an added source which would move the maps
        QFile changed(QStringLiteral(":/unittest/ed269_test.json"));
        QVERIFY(changed.open(QIODevice::ReadOnly));
        body = changed.readAll();
        feed->setValidators(QString(), QString());
        manager.repository()->refreshFeeds();
        QTRY_VERIFY(!manager.repository()->busy());
        QCOMPARE(aboutToChangeSpy.count(), 1);
        QCOMPARE(manager.repository()->source(url)->formatName(), QStringLiteral("ED-269"));
        QCOMPARE(addedSpy.count(), 1);
    }

    // The validators are kept across restarts
    GeoZoneManager restarted(storageDir.path());
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QVERIFY(restarted.repository()->source(url));
    QCOMPARE(restarted.repository()->source(url)->etag(), QStringLiteral("\"v1\""));
    const QDateTime downloadedAt = restarted.repository()->source(url)->loadedAt();
    environment.addSecs(60);
    restarted.repository()->refreshFeeds();
    QTRY_VERIFY(!restarted.repository()->busy());
    QCOMPARE(notModifiedCount, 2);
    QCOMPARE(restarted.zoneCount(), 3);

    // When the server confirmed the stored copy is kept apart from when it was downloaded
    GeoZoneManager confirmed(storageDir.path());
    confirmed.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QVERIFY(confirmed.repository()->source(url));
    QCOMPARE(confirmed.repository()->source(url)->loadedAt(), downloadedAt);
    QCOMPARE(confirmed.repository()->source(url)->checkedAt(), environment.now());
    QVERIFY(confirmed.repository()->source(url)->detailsText().contains(QStringLiteral("checked")));
    QCOMPARE(confirmed.repository()->scheduler()->nextRefresh(*confirmed.repository()->source(url)), environment.now().addSecs(24 * 3600));
}

void GeoZoneRepositoryTest::_testSourceValidity()
{
    const ScopedEnvironment environment;
    GeoZoneSource source(QStringLiteral("zones.json"), QString());
    GeoZoneParser::Result result;
    result.format = GeoZoneParser::Format::ED269;
    const QDateTime now = environment.now();

    // Without an issue date the file date counts, outdated after 7 days by default and then invalid
    source.setResult(result, now, now.addDays(-5));
    QVERIFY(source.validityText().isEmpty());
    QVERIFY(!source.invalid());
    source.setResult(result, now, now.addDays(-30));
    QCOMPARE(source.validityText(), QStringLiteral("Data is 30 days old"));
    QVERIFY(source.invalid());
    // The file's own date is a date, when it was loaded a time
    QVERIFY2(source.detailsText().contains(QStringLiteral("file from %1").arg(now.addDays(-30).toString(QStringLiteral("yyyy-MM-dd")))), qPrintable(source.detailsText()));
    QVERIFY2(source.detailsText().contains(QStringLiteral("loaded %1").arg(QLocale().toString(now.toLocalTime(), QLocale::ShortFormat))), qPrintable(source.detailsText()));

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
    // Downloads tell the time
    QVERIFY2(download.detailsText().contains(QStringLiteral("downloaded %1").arg(QLocale().toString(now.addDays(-30).toLocalTime(), QLocale::ShortFormat))),
             qPrintable(download.detailsText()));
    QVERIFY(!GeoZoneSource::timeText(now.addDays(-30), now).contains(QStringLiteral("today")));
    download.setCheckedAt(now.addDays(-1));
    QVERIFY(download.validityText().isEmpty());
    QCOMPARE(download.dataDate(), now.addDays(-30));

    // The manager summarizes outdated sources
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.path());
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(QStringLiteral(":/unittest/ed318_test.json"), errorString), qPrintable(errorString));
    QVERIFY(manager.repository()->validityWarning().startsWith(QStringLiteral("Test GeoZones: Data is")));
    QVERIFY(manager.repository()->dataInvalid());
    // with the time the data was last pulled
    QCOMPARE(manager.repository()->lastPulledAt(), now);
    QCOMPARE(manager.repository()->lastPulledText(), QStringLiteral("Data pulled %1 today").arg(QLocale().toString(now.toLocalTime().time(), QLocale::ShortFormat)));

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

void GeoZoneRepositoryTest::_testFeedAgeFromLastCheck()
{
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2027, 1, 10), QTime(12, 0)));
    const QDateTime now = environment.now();
    const QString issuedText = QStringLiteral("issued %1").arg(now.addDays(-90).toString(QStringLiteral("yyyy-MM-dd")));

    // A feed whose data was issued months ago, like the ED-318 file of Sweden, is current when its server just sent it
    const QString feedUrl = QStringLiteral("https://example.com/zones.json");
    GeoZoneSource feed(feedUrl, feedUrl);
    GeoZoneParser::Result result;
    result.format = GeoZoneParser::Format::ED318;
    result.issued = now.addDays(-90);
    feed.setResult(result, now, now);
    QCOMPARE(feed.validityText(), QString());
    QVERIFY(!feed.invalid());
    // The issue date is shown as information
    QCOMPARE(feed.dataDate(), result.issued);
    QVERIFY2(feed.detailsText().contains(issuedText), qPrintable(feed.detailsText()));
    // It is outdated once its server didn't confirm it for longer than the outdated age, 7 days by default
    feed.updateValidity(now.addDays(7));
    QCOMPARE(feed.validityText(), QString());
    feed.updateValidity(now.addDays(8));
    QCOMPARE(feed.validityText(), QStringLiteral("Data is 8 days old"));
    QVERIFY(feed.invalid());
    environment.addSecs(8 * 24 * 3600);
    feed.setCheckedAt(environment.now());
    QCOMPARE(feed.validityText(), QString());
    environment.setNow(now);

    // The validity period of the data still counts right after a download
    result.validTo = now.addDays(-2);
    feed.setResult(result, now, now);
    QVERIFY2(feed.validityText().startsWith(QStringLiteral("Expired on")), qPrintable(feed.validityText()));
    result.validTo = QDateTime();
    result.validFrom = now.addDays(2);
    feed.setResult(result, now, now);
    QVERIFY2(feed.validityText().startsWith(QStringLiteral("Not valid until")), qPrintable(feed.validityText()));

    // Live restrictions are outdated two hours after their server last confirmed them, whenever they were issued
    GeoZoneSource live(feedUrl, feedUrl);
    result = GeoZoneParser::Result();
    result.format = GeoZoneParser::Format::FaaTfr;
    result.issued = now.addSecs(-10 * 3600);
    live.setResult(result, now, now);
    QVERIFY(live.isLive());
    QCOMPARE(live.validityText(), QString());
    live.updateValidity(now.addSecs(3 * 3600));
    QCOMPARE(live.validityText(), QStringLiteral("Data is 3 hour(s) old"));

    // A file is as old as its issue date, however recently it was loaded
    GeoZoneSource file(QStringLiteral("zones.json"), QString());
    result = GeoZoneParser::Result();
    result.format = GeoZoneParser::Format::ED318;
    result.issued = now.addDays(-90);
    file.setResult(result, now, now);
    QCOMPARE(file.validityText(), QStringLiteral("Data is 90 days old"));

    // The test feed was issued in August 2026: just downloaded, it is current
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QString url = server.url(QStringLiteral("/ok"));
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        manager.repository()->addUrl(url);
        QVERIFY(addedSpy.wait(stepTimeoutMSecs));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.repository()->busy(), stepTimeoutMSecs);
        const GeoZoneSource *const added = manager.repository()->source(url);
        QVERIFY(added);
        QCOMPARE(added->dataDate(), _utc(QDate(2026, 8, 13), QTime(0, 0)));
        QCOMPARE(added->validityText(), QString());
        QCOMPARE(manager.repository()->validityWarning(), QString());
        QVERIFY(!manager.repository()->dataInvalid());
    }

    // A month later the server sends the same data at startup: nothing is reported as outdated
    environment.addSecs(30 * 24 * 3600);
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy outdatedSpy(&manager, &GeoZoneManager::outdatedDataFound);
        manager.init();
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount, 2, stepTimeoutMSecs);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.repository()->busy(), stepTimeoutMSecs);
        QCOMPARE(manager.repository()->validityWarning(), QString());
        QCOMPARE(outdatedSpy.count(), 0);
    }

    // Another month later the server fails: the copy it last confirmed a month ago is reported, once
    environment.addSecs(30 * 24 * 3600);
    server.okPathStatus = "500 Internal Server Error";
    GeoZoneManager manager(storageDir.path());
    QSignalSpy outdatedSpy(&manager, &GeoZoneManager::outdatedDataFound);
    manager.init();
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount, 3, stepTimeoutMSecs);
    QTRY_COMPARE_WITH_TIMEOUT(outdatedSpy.count(), 1, stepTimeoutMSecs);
    QVERIFY2(outdatedSpy.first()[0].toString().contains(QStringLiteral("Data is 30 days old")), qPrintable(outdatedSpy.first()[0].toString()));
    QVERIFY(manager.repository()->dataInvalid());
    QCOMPARE(manager.zoneCount(), 3);
}

void GeoZoneRepositoryTest::_testDownloadSizeLimit()
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

void GeoZoneRepositoryTest::_testPagedSizeLimit()
{
    // A WFS answering 6 zones in 3 pages
    QFile restrictions(QStringLiteral(":/unittest/france_uas_test.json"));
    QVERIFY(restrictions.open(QIODevice::ReadOnly));
    const QJsonObject collection = QJsonDocument::fromJson(restrictions.readAll()).object();
    const QJsonArray zones = wfsFeatures(collection, 6);
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.handler = [&](const QByteArray &request, QByteArray &, QByteArray &content, QByteArray &) {
        content = wfsPage(collection, zones, request, 2);
        return true;
    };
    const qint64 pageBytes = wfsPage(collection, zones, "GET /wfs HTTP/1.1", 2).size();
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneDownloader downloader;
    downloader.setDirectory(dir.path());
    GeoZoneFeedFetch::Request request;
    request.url = server.url(QStringLiteral("/wfs?SERVICE=WFS&VERSION=2.0.0&REQUEST=GetFeature&TYPENAMES=restrictions&OUTPUTFORMAT=application/json"));
    const auto fetchResult = [&downloader, &request]() {
        GeoZoneFeedFetch fetch(&downloader, request);
        std::optional<GeoZoneFeedFetch::Result> result;
        (void) connect(&fetch, &GeoZoneFeedFetch::finished, &fetch, [&result](const GeoZoneFeedFetch::Result &finished) { result = finished; });
        fetch.start();
        (void) QTest::qWaitFor([&result]() { return result.has_value(); }, stepTimeoutMSecs);
        return result;
    };

    // Each page fits the size a download may have, but not all together, which are loaded as one document
    downloader.setMaxBytes((5 * pageBytes) / 2);
    std::optional<GeoZoneFeedFetch::Result> result = fetchResult();
    QVERIFY(result.has_value());
    QCOMPARE(result->status, GeoZoneFeedFetch::Result::Status::Failed);
    QVERIFY2(result->errorString.startsWith(QStringLiteral("The pages of the response are larger than")), qPrintable(result->errorString));
    QCOMPARE(server.requestCount, 3);
    QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());

    // Within the size, all pages are kept
    downloader.setMaxBytes(4 * pageBytes);
    result = fetchResult();
    QVERIFY(result.has_value());
    QCOMPARE(result->status, GeoZoneFeedFetch::Result::Status::Ready);
    QCOMPARE(result->pagePaths.count(), 2);
}

void GeoZoneRepositoryTest::_testDownloadWithoutFile()
{
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    // A folder for the downloads which can't be made: it is below a file
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile blocker(dir.filePath(QStringLiteral("file")));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    GeoZoneDownloader downloader;
    downloader.setDirectory(dir.filePath(QStringLiteral("file/downloads")));

    std::optional<GeoZoneDownloader::Result> result;
    downloader.get(server.url(QStringLiteral("/ok")), QString(), QString(), [&result](const GeoZoneDownloader::Result &downloadResult) { result = downloadResult; });
    // The failure is reported like others, once the call returned
    QVERIFY(!result.has_value());
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), stepTimeoutMSecs);
    QCOMPARE(result->status, GeoZoneDownloader::Result::Status::Failed);
    QVERIFY2(result->errorString.startsWith(QStringLiteral("The download could not be saved")), qPrintable(result->errorString));
    QVERIFY(!result->unreachable);
    QVERIFY(result->filePath.isEmpty());
    // Nothing was requested, whose response would have piled up in memory
    QCOMPARE(server.requestCount, 0);
    QCOMPARE(downloader.count(), 0);
}

void GeoZoneRepositoryTest::_testDownloadFilesRemoved()
{
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto files = [&dir]() { return QDir(dir.path()).entryList(QDir::Files); };
    {
        GeoZoneDownloader downloader;
        downloader.setDirectory(dir.path());
        // A download which is canceled leaves no file
        std::optional<GeoZoneDownloader::Result> canceled;
        downloader.get(server.url(QStringLiteral("/hang")), QString(), QString(), [&canceled](const GeoZoneDownloader::Result &result) { canceled = result; },
                       QStringLiteral("canceled"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount, 1, stepTimeoutMSecs);
        QCOMPARE(files().count(), 1);
        downloader.abort(QStringLiteral("canceled"));
        QTRY_VERIFY_WITH_TIMEOUT(canceled.has_value(), stepTimeoutMSecs);
        QCOMPARE(canceled->status, GeoZoneDownloader::Result::Status::Failed);
        QCOMPARE(files(), QStringList());

        // Nor does one still in progress when its downloader goes, like when the application quits
        downloader.get(server.url(QStringLiteral("/hang")), QString(), QString(), [](const GeoZoneDownloader::Result &) {});
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount, 2, stepTimeoutMSecs);
        QCOMPARE(files().count(), 1);
        QVERIFY(files().constFirst().endsWith(QStringLiteral(".part")));
    }
    QCOMPARE(files(), QStringList());
}

void GeoZoneRepositoryTest::_testDownloadFailures()
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
    manager.repository()->addUrl(server.url(QStringLiteral("/missing")));
    QVERIFY(failedSpy.wait(10000));
    QCOMPARE(failedSpy.last()[0].toString(), server.url(QStringLiteral("/missing")));
    QVERIFY(failedSpy.last()[1].toString().contains(QStringLiteral("Not Found")));
    QCOMPARE(manager.sources()->count(), 0);
    manager.repository()->addUrl(QStringLiteral("ftp://example.com/zones.json"));
    QCOMPARE(failedSpy.count(), 2);

    // A refresh which fails keeps the zones and shows the error on the source
    manager.repository()->addUrl(server.url(QStringLiteral("/ok")));
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    GeoZoneSource *const feed = manager.repository()->source(server.url(QStringLiteral("/ok")));
    QVERIFY(feed);
    server.okPathStatus = "503 Service Unavailable";
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(failedSpy.count(), 3);
    QVERIFY(feed->errorString().contains(QStringLiteral("Service Unavailable")));
    QCOMPARE(manager.zoneCount(), 3);

    // The next successful refresh clears the error
    server.okPathStatus = "200 OK";
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(feed->errorString().isEmpty());

    // Downloads become the stored copies, without being left in the download directory
    const QString downloadDir = QDir(manager.repository()->storageDir()).filePath(QStringLiteral("downloads"));
    QCOMPARE(QDir(downloadDir).entryList(QDir::Files).count(), 0);
    QFile stored(QDir(manager.repository()->storageDir()).filePath(manager.repository()->storedFileName(feed->key())));
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QCOMPARE(stored.readAll(), server.body);

    // Removing a source cancels its download
    const int requests = server.requestCount;
    server.okPathStatus = "200 OK";
    manager.repository()->addUrl(server.url(QStringLiteral("/hang")));
    QTRY_COMPARE(server.requestCount, requests + 1);
    QVERIFY(manager.repository()->busy());
    manager.repository()->clear();
    QTRY_VERIFY_WITH_TIMEOUT(!manager.repository()->busy(), 1000);
    QCOMPARE(manager.sources()->count(), 0);
    QCOMPARE(failedSpy.count(), 3);
    QCOMPARE(QDir(downloadDir).entryList(QDir::Files).count(), 0);
}

void GeoZoneRepositoryTest::_testDownloadRedirects()
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

    // nor the custom headers of a service, which may hold keys
    GeoZoneService service;
    service.headers.append({ QStringLiteral("X-Client-Key"), QStringLiteral("header-s3cret") });
    server.requests.clear();
    otherServer.requests.clear();
    result = download(server.url(QStringLiteral("/away")), service.downloadOptions(QGeoRectangle(), QDateTime(), QString()));
    QVERIFY2(result.status == GeoZoneDownloader::Result::Status::Ok, qPrintable(result.errorString));
    QCOMPARE(TestFeedServer::header(server.requests.first(), "X-Client-Key"), QByteArray("header-s3cret"));
    QCOMPARE(otherServer.requests.count(), 1);
    QVERIFY(TestFeedServer::header(otherServer.requests.first(), "X-Client-Key").isEmpty());

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

void GeoZoneRepositoryTest::_testProvenance()
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
    QVERIFY(storageDir.isValid());
    QJsonObject provenanceJson;
    {
        GeoZoneManager manager(storageDir.path());
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        manager.repository()->addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.repository()->busy());
        const GeoZoneSource *const feed = manager.repository()->source(url);
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
        QCOMPARE(provenance.contentHash, GeoZoneHash::contentHash(server.body));
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
        QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));
        const GeoZoneSource *const file = manager.repository()->source(GeoZoneLoader::fileKey(fileName));
        QCOMPARE(file->provenance().requestUrl, QUrl::fromLocalFile(GeoZoneLoader::fileKey(fileName)).toString());
        QCOMPARE(file->provenance().contentHash.size(), 32);
        QVERIFY(file->provenance().sha256.isEmpty());

        // The report of packs, and the sources of briefings
        const QJsonObject feedReport = GeoZoneSourceRepository::sourceReport(*feed);
        QCOMPARE(feedReport.value("url").toString(), url);
        QCOMPARE(feedReport.value("zoneCount").toInt(), 3);
        QCOMPARE(feedReport.value("format").toString(), QStringLiteral("ED-318"));
        provenanceJson = feedReport.value("provenance").toObject();
        QCOMPARE(provenanceJson.value("finalUrl").toString(), server.url(QStringLiteral("/v2/zones.json")));
        QCOMPARE(provenanceJson.value("sha256").toString(), QString::fromLatin1(provenance.sha256.toHex()));
        QCOMPARE(provenanceJson.value("blake2b256").toString(), QString::fromLatin1(provenance.contentHash.toHex()));
        QCOMPARE(provenanceJson.value("fetchedAt").toString(), environment.now().toString(Qt::ISODate));
        QCOMPARE(provenanceJson.value("integrity").toString(), QStringLiteral("unchecked"));
        QCOMPARE(GeoZoneSourceVersion::fromSource(feed).toJson().value("provenance").toObject(), provenanceJson);
    }

    // The provenance is kept with the stored copies
    GeoZoneManager restarted(storageDir.path());
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QVERIFY(restarted.repository()->source(url));
    QCOMPARE(restarted.repository()->source(url)->provenance().toJson(), provenanceJson);
    QCOMPARE(GeoZoneProvenance::fromJson(restarted.repository()->source(url)->provenance().toJson()).toJson(), restarted.repository()->source(url)->provenance().toJson());
}

void GeoZoneRepositoryTest::_testIntegrity()
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
    // A compressed response, whose digest is of the compressed data
    const QByteArray compressed = qCompress(server.body).mid(4);
    server.files.insert("/compressed", compressed);
    server.headers.insert("/compressed", "Content-Encoding: deflate\r\nRepr-Digest: sha-256=:" +
                          QCryptographicHash::hash(compressed, QCryptographicHash::Sha256).toBase64() + ":\r\n");

    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);

    // A digest header matching the data verifies it
    manager.repository()->addUrl(server.url(QStringLiteral("/digest")));
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    const GeoZoneSource *const digest = manager.repository()->source(server.url(QStringLiteral("/digest")));
    QVERIFY(digest->verified());
    QCOMPARE(digest->provenance().integrityText, QStringLiteral("repr-digest header"));
    QVERIFY(digest->provenanceText().contains(QStringLiteral("verified")));

    // The digest of compressed data can't be checked against the data Qt decompressed
    manager.repository()->addUrl(server.url(QStringLiteral("/compressed")));
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    const GeoZoneSource *const compressedFeed = manager.repository()->source(server.url(QStringLiteral("/compressed")));
    QCOMPARE(compressedFeed->zoneCount(), 3);
    QCOMPARE(compressedFeed->provenance().integrity, GeoZoneProvenance::Integrity::Unchecked);
    QCOMPARE(compressedFeed->provenance().sha256, sha256);

    // Data which doesn't match isn't loaded
    manager.repository()->addUrl(server.url(QStringLiteral("/tampered")));
    QVERIFY(failedSpy.wait(10000));
    QVERIFY2(failedSpy.last()[1].toString().contains(QStringLiteral("The integrity check failed: the SHA-256 of the data doesn't match its digest header.")),
             qPrintable(failedSpy.last()[1].toString()));
    QVERIFY(!manager.repository()->source(server.url(QStringLiteral("/tampered"))));

    // The SHA-256 a service publishes next to its data
    GeoZoneService service;
    service.id = QStringLiteral("signed-test");
    service.sha256Url = QStringLiteral("{url}.sha256");
    const QString signedUrl = server.url(QStringLiteral("/signed.json"));
    manager.addService(signedUrl, service.toVariantMap(), QString());
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    GeoZoneSource *const signedFeed = manager.repository()->source(signedUrl);
    QVERIFY(signedFeed->verified());
    QCOMPARE(signedFeed->provenance().integrityText, server.url(QStringLiteral("/signed.json.sha256")));
    QCOMPARE(signedFeed->provenance().providerId, QStringLiteral("signed-test"));
    QVERIFY(!signedFeed->integrityFailed());

    // A refresh which doesn't match keeps the verified zones and marks the source
    server.files.insert("/signed.json.sha256", QCryptographicHash::hash("other", QCryptographicHash::Sha256).toHex());
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(signedFeed->integrityFailed());
    QVERIFY2(signedFeed->errorString().contains(QStringLiteral("doesn't match %1").arg(server.url(QStringLiteral("/signed.json.sha256")))), qPrintable(signedFeed->errorString()));
    QVERIFY(signedFeed->verified());
    QCOMPARE(signedFeed->zoneCount(), 3);
    QVERIFY(GeoZoneSourceRepository::sourceReport(*signedFeed).value("integrityFailed").toBool());

    // As does a SHA-256 which can't be read
    server.files.remove("/signed.json.sha256");
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(signedFeed->integrityFailed());
    QVERIFY(signedFeed->errorString().contains(QStringLiteral("could not be downloaded")));
    server.files.insert("/signed.json.sha256", "not a hash");
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(signedFeed->errorString().contains(QStringLiteral("has no SHA-256")));

    server.files.insert("/signed.json.sha256", sha256.toHex().toUpper());
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(!signedFeed->integrityFailed());
    QVERIFY(signedFeed->errorString().isEmpty());
}

void GeoZoneRepositoryTest::_testFeedDatasetLink()
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
                                             ("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: max-age=60\r\nContent-Length: " + QByteArray::number(page.size()) + "\r\n");
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
    // The page is the one of a feed of the catalogue, which links its datasets like NATS
    GeoZoneFeedCatalogue::Catalogue catalogue = GeoZoneFeedCatalogue::builtIn();
    GeoZoneFeedCatalogue::Catalogue overlay;
    QString catalogueError;
    QVERIFY2(GeoZoneFeedCatalogue::parse(QStringLiteral(R"json({ "feeds": [ { "id": "test-page", "name": "Test page", "url": "%1", "format": "aixm", "airac": true,
        "datasetLink": { "pattern": "href=\"([^\"]{0,300}UAS_AREA_1/[^\"]{0,300}_FULL_(\\d{8})_XML\\.zip)\"" } } ] })json").arg(url).toUtf8(),
        QStringLiteral("feeds.json"), overlay, catalogueError), qPrintable(catalogueError));
    QVERIFY2(overlay.warnings.isEmpty(), qPrintable(overlay.warnings.join(QLatin1Char('\n'))));
    GeoZoneFeedCatalogue::apply(catalogue, overlay);
    GeoZoneProviders::setCatalogue(catalogue.feeds, catalogue.templates);
    const auto restoreCatalogue = qScopeGuard([]() { GeoZoneProviders::resetCatalogue(); });
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
        manager.repository()->addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.repository()->busy());
        GeoZoneSource *const feed = manager.repository()->source(url);
        QVERIFY(feed);
        QVERIFY(feed->zoneCount() > 0);
        QVERIFY(feed->datasetUrl().endsWith(QStringLiteral("EG_FULL_20200101_XML.zip")));
        // The page is current for a minute, which isn't how long the dataset is
        QCOMPARE(manager.repository()->scheduler()->refreshIntervalSecs(*feed), 24 * 3600);

        // The page is read again without validators, the unchanged dataset is confirmed by its own
        const int zoneCount = manager.zoneCount();
        manager.repository()->refreshFeeds();
        QTRY_VERIFY(!manager.repository()->busy());
        QVERIFY(!lastRequest("/page").contains("if-none-match"));
        QVERIFY(!lastRequest("/page").contains("if-modified-since"));
        QVERIFY(lastRequest("/d/UAS_AREA_1/EG_FULL_20200101_XML.zip").contains("if-none-match"));
        QCOMPARE(manager.zoneCount(), zoneCount);
        QVERIFY(feed->errorString().isEmpty());

        // A new cycle on the page is downloaded
        page = "<html><body><a href=\"/d/UAS_AREA_1/EG_FULL_20200101_XML.zip\">XML</a>"
               "<a href=\"/d/UAS_AREA_1/EG_FULL_20210101_XML.zip\">XML</a></body></html>";
        manager.repository()->refreshFeeds();
        QTRY_VERIFY(!manager.repository()->busy());
        QVERIFY(feed->datasetUrl().endsWith(QStringLiteral("EG_FULL_20210101_XML.zip")));
        QVERIFY(!lastRequest("/d/UAS_AREA_1/EG_FULL_20210101_XML.zip").isEmpty());
        QVERIFY(!lastRequest("/d/UAS_AREA_1/EG_FULL_20210101_XML.zip").contains("if-none-match"));
    }

    // The dataset is remembered with its validators
    GeoZoneManager restarted(storageDir.path());
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QVERIFY(restarted.repository()->source(url));
    QVERIFY(restarted.repository()->source(url)->datasetUrl().endsWith(QStringLiteral("EG_FULL_20210101_XML.zip")));
    QCOMPARE(restarted.repository()->source(url)->etag(), QStringLiteral("\"20210101_XML.zip\""));
}

void GeoZoneRepositoryTest::_testPageMerge()
{
    // A response downloaded in three pages, which the loader puts together before parsing and storing it
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const GeoZoneStore store(dir.filePath(QStringLiteral("store")));
    const QGeoCoordinate center(47, 8);
    const auto writePages = [&dir, &center](GeoZoneLoader::Job &job) {
        job = GeoZoneLoader::Job();
        job.key = job.url = QStringLiteral("https://example.com/zones");
        job.downloaded = true;
        job.loadedAt = job.fileDate = QDateTime::currentDateTimeUtc();
        job.storedFileName = QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json");
        job.sourcePath = _writeFeatures(dir, QStringLiteral("page0.part"), QJsonArray{ _circleFeature(QStringLiteral("Z0"), QStringLiteral("PROHIBITED"), center, 100) });
        for (int page = 1; page <= 2; page++) {
            job.pagePaths.append(_writeFeatures(dir, QStringLiteral("page%1.part").arg(page), QJsonArray{
                _circleFeature(QStringLiteral("Z%1").arg(page), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(page * 1000, 90), 100) }));
        }
    };
    const auto downloadsLeft = [&dir]() { return QDir(dir.path()).entryList({ QStringLiteral("*.part") }, QDir::Files); };
    GeoZoneLoader::Job job;

    writePages(job);
    GeoZoneLoader::Result result = GeoZoneLoader::run(job, store);
    QVERIFY2(result.errorString.isEmpty(), qPrintable(result.errorString));
    QCOMPARE(result.result.zones.count(), 3);
    QVERIFY(result.stored);
    QCOMPARE(downloadsLeft(), QStringList());
    QFile stored(store.path(job.storedFileName));
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(stored.readAll()).object().value(QStringLiteral("features")).toArray().count(), 3);
    stored.close();
    store.removeFile(job.storedFileName);

    // A page which can't be read fails the load, rather than storing a response which misses its zones
    const QList<std::function<bool(const QString &pagePath)>> damages = {
        [](const QString &pagePath) { return QFile::remove(pagePath); },
        [](const QString &pagePath) { QFile page(pagePath); return page.open(QIODevice::WriteOnly | QIODevice::Truncate) && (page.write("{ \"features\": [") > 0); },
        [](const QString &pagePath) { QFile page(pagePath); return page.open(QIODevice::WriteOnly | QIODevice::Truncate) && (page.write("{ \"error\": 429 }") > 0); },
    };
    for (const auto &damage : damages) {
        writePages(job);
        QVERIFY(damage(job.pagePaths.constLast()));
        result = GeoZoneLoader::run(job, store);
        QVERIFY(!result.errorString.isEmpty());
        QVERIFY(!result.stored);
        QVERIFY(result.result.zones.isEmpty());
        QVERIFY(!QFile::exists(store.path(job.storedFileName)));
        QCOMPARE(downloadsLeft(), QStringList());
    }

    // So does a first page which can't be replaced by the merged document
    writePages(job);
    QVERIFY(QFile::setPermissions(job.sourcePath, QFile::ReadOwner));
    if (!QFileInfo(job.sourcePath).isWritable()) {
        result = GeoZoneLoader::run(job, store);
        QVERIFY2(result.errorString.startsWith(QStringLiteral("Unable to write file")), qPrintable(result.errorString));
        QVERIFY(!result.stored);
        QVERIFY(!QFile::exists(store.path(job.storedFileName)));
    }
    (void) QFile::remove(job.sourcePath);
    QCOMPARE(downloadsLeft(), QStringList());
}

void GeoZoneRepositoryTest::_testWfsPaging()
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
        manager.repository()->addUrl(url);
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.repository()->busy());
        QCOMPARE(manager.zoneCount(), 5);
        QCOMPARE(queries.count(), 3);
        QCOMPARE(queries[1].queryItemValue(QStringLiteral("STARTINDEX")), QStringLiteral("2"));
        QCOMPARE(queries[2].queryItemValue(QStringLiteral("STARTINDEX")), QStringLiteral("4"));
        QVERIFY(manager.repository()->source(url)->warningText().isEmpty());
    }
    GeoZoneManager restarted(storageDir.path());
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QCOMPARE(restarted.zoneCount(), 5);

    // A page which fails leaves the zones of the others, and the source tells how many are missing
    failThirdPage = true;
    QSignalSpy failedSpy(&restarted, &GeoZoneManager::loadFailed);
    restarted.repository()->refreshFeeds();
    QTRY_VERIFY(!restarted.repository()->busy());
    QCOMPARE(restarted.zoneCount(), 4);
    const GeoZoneSource *const partial = restarted.repository()->source(url);
    QVERIFY(partial);
    QVERIFY(partial->warningText().contains(QStringLiteral("left out 1 zone(s)")));
    // It counts as a failed refresh: the user is told, geo-awareness is degraded and it is retried as one
    QCOMPARE(partial->errorString(), QStringLiteral("The download is incomplete: 1 zone(s) are missing."));
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(GeoZoneAwareness::dataInput(&restarted).failedSources, QStringList{ partial->name() });
    QVERIFY(restarted.repository()->scheduler()->retryAt(url).isValid());
    // also after a restart
    {
        GeoZoneManager reloaded(storageDir.path());
        reloaded.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
        QCOMPARE(reloaded.zoneCount(), 4);
        QCOMPARE(GeoZoneAwareness::dataInput(&reloaded).failedSources.count(), 1);
    }

    // until all its pages are downloaded again
    failThirdPage = false;
    restarted.repository()->refreshFeeds();
    QTRY_VERIFY(!restarted.repository()->busy());
    QCOMPARE(restarted.zoneCount(), 5);
    QCOMPARE(partial->errorString(), QString());
    QVERIFY(GeoZoneAwareness::dataInput(&restarted).failedSources.isEmpty());
    QVERIFY(!restarted.repository()->scheduler()->retryAt(url).isValid());
}

void GeoZoneRepositoryTest::_testPageCap()
{
    QFile airspaces(QStringLiteral(":/unittest/faa_airspace_test.json"));
    QVERIFY(airspaces.open(QIODevice::ReadOnly));
    const QJsonObject arcGisDocument = QJsonDocument::fromJson(airspaces.readAll()).object();
    QFile restrictions(QStringLiteral(":/unittest/france_uas_test.json"));
    QVERIFY(restrictions.open(QIODevice::ReadOnly));
    const QJsonObject wfsCollection = QJsonDocument::fromJson(restrictions.readAll()).object();
    // A WFS with one page of 2 zones more than the pages downloaded
    constexpr int wfsPageSize = 2;
    const QJsonArray wfsZones = wfsFeatures(wfsCollection, (GeoZoneProviders::maxWfsPages + 2) * wfsPageSize);

    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.handler = [&](const QByteArray &request, QByteArray &, QByteArray &content, QByteArray &) {
        if (request.startsWith("GET /arcgis")) {
            // An ArcGIS feature service which always has more features
            QJsonObject page = arcGisDocument;
            page.insert(QStringLiteral("properties"), QJsonObject{ { QStringLiteral("exceededTransferLimit"), true } });
            content = QJsonDocument(page).toJson(QJsonDocument::Compact);
        } else if (request.startsWith("GET /numbered")) {
            // Numbered pages with more pages than downloaded
            content = R"({"page":1,"totalPages":)" + QByteArray::number(GeoZoneProviders::maxWfsPages + 2) + R"(,"items":[]})";
        } else {
            content = wfsPage(wfsCollection, wfsZones, request, wfsPageSize);
        }
        return true;
    };

    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);

    // A response still limited after the most pages downloaded would silently miss zones: it fails instead
    const QString arcGisUrl = server.url(QStringLiteral("/arcgis/rest/services/Layer/FeatureServer/0/query?where=1%3D1&geometry=-77.6,38.3,-76.0,39.6"
                                                        "&geometryType=esriGeometryEnvelope&f=geojson&resultRecordCount=3&resultOffset=0"));
    manager.repository()->addUrl(arcGisUrl);
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 3 * stepTimeoutMSecs);
    QVERIFY2(failedSpy.last()[1].toString().contains(QStringLiteral("more than 51 pages")), qPrintable(failedSpy.last()[1].toString()));
    QCOMPARE(server.requestCount, GeoZoneProviders::maxWfsPages + 1);
    QVERIFY(!manager.repository()->source(arcGisUrl));
    QCOMPARE(manager.zoneCount(), 0);

    // Numbered pages tell their count with the first one, the others aren't downloaded
    const QString numberedUrl = server.url(QStringLiteral("/numbered?page=1"));
    manager.repository()->addUrl(numberedUrl);
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 2, stepTimeoutMSecs);
    QVERIFY2(failedSpy.last()[1].toString().contains(QStringLiteral("more than 51 pages")), qPrintable(failedSpy.last()[1].toString()));
    QCOMPARE(server.requestCount, GeoZoneProviders::maxWfsPages + 2);
    QVERIFY(!manager.repository()->source(numberedUrl));

    // A WFS tells how many zones the pages downloaded miss: they are loaded, and count as a failed refresh
    const QString wfsUrl = server.url(QStringLiteral("/wfs?SERVICE=WFS&VERSION=2.0.0&REQUEST=GetFeature&TYPENAMES=restrictions&OUTPUTFORMAT=application/json"));
    manager.repository()->addUrl(wfsUrl);
    QVERIFY(addedSpy.wait(3 * stepTimeoutMSecs));
    QTRY_VERIFY_WITH_TIMEOUT(!manager.repository()->busy(), stepTimeoutMSecs);
    const GeoZoneSource *const wfs = manager.repository()->source(wfsUrl);
    QVERIFY(wfs);
    QCOMPARE(wfs->zoneCount(), (GeoZoneProviders::maxWfsPages + 1) * wfsPageSize);
    QCOMPARE(wfs->errorString(), QStringLiteral("The download is incomplete: 2 zone(s) are missing."));
    QCOMPARE(failedSpy.count(), 3);
    QCOMPARE(GeoZoneAwareness::dataInput(&manager).failedSources, QStringList{ wfs->name() });
}

void GeoZoneRepositoryTest::_testArcGisPaging()
{
    // An ArcGIS feature service answering the FAA layers by 3 features, with resultOffset, telling whether more follow
    QFile fixture(QStringLiteral(":/unittest/faa_airspace_test.json"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const QJsonObject document = QJsonDocument::fromJson(fixture.readAll()).object();
    const QJsonArray allFeatures = document.value(QStringLiteral("features")).toArray();
    QCOMPARE(allFeatures.count(), 7);
    constexpr int pageSize = 3;
    bool rateLimited = false;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.handler = [&](const QByteArray &request, QByteArray &, QByteArray &content, QByteArray &) {
        const QUrlQuery query(QUrl(QString::fromLatin1(request.split(' ').value(1))));
        const int offset = query.queryItemValue(QStringLiteral("resultOffset")).toInt();
        if (rateLimited && (offset > 0)) {
            content = R"({"error":{"code":429,"message":"Unable to perform query. Too many requests.","details":[]}})";
            return true;
        }
        QJsonObject page = document;
        QJsonArray features;
        for (int i = offset; (i < offset + pageSize) && (i < allFeatures.count()); i++) {
            features.append(allFeatures[i]);
        }
        page.insert(QStringLiteral("features"), features);
        if (offset + pageSize < allFeatures.count()) {
            page.insert(QStringLiteral("properties"), QJsonObject{ { QStringLiteral("exceededTransferLimit"), true } });
        }
        content = QJsonDocument(page).toJson(QJsonDocument::Compact);
        return true;
    };
    const QString url = server.url(QStringLiteral("/arcgis/rest/services/Layer/FeatureServer/0/query?where=1%3D1&geometry=-77.6,38.3,-76.0,39.6"
                                                  "&geometryType=esriGeometryEnvelope&f=geojson&resultRecordCount=3&resultOffset=0"));

    // All pages are downloaded and stored together
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    manager.repository()->addUrl(url);
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(manager.zoneCount(), 6);
    QCOMPARE(server.requestCount, 3);
    QVERIFY(server.requestedPaths[1].contains("resultOffset=3"));
    QVERIFY(server.requestedPaths[2].contains("resultOffset=6"));
    GeoZoneSource *const source = manager.repository()->source(url);
    QVERIFY(source->errorString().isEmpty());
    QCOMPARE(source->area(), QGeoRectangle());
    QVERIFY2(source->name().contains(QStringLiteral("area around")), qPrintable(source->name()));

    // A page answered with an error fails the download, which keeps the zones instead of losing some
    rateLimited = true;
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(manager.zoneCount(), 6);
    QVERIFY2(source->errorString().contains(QStringLiteral("Too many requests")), qPrintable(source->errorString()));
}

GEOZONE_TEST(GeoZoneRepositoryTest)
