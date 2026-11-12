/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePackTest.h"
#include "GeoZoneCredentials.h"
#include "GeoZonePackExchange.h"
#include "GeoZonePackSignature.h"
#include "GeoZoneTestBase.h"
#include "GeoZoneTestHelpers.h"

#include <QtCore/QCryptographicHash>

using namespace GeoZoneTestHelpers;

namespace {

/// @return The files of a pack by name
QHash<QString, QByteArray> _packFiles(const QString &packPath)
{
    QHash<QString, QByteArray> files;
    QFile pack(packPath);
    if (pack.open(QIODevice::ReadOnly)) {
        QString zipError;
        for (const GeoZoneZip::File &file : GeoZoneZip::files(pack.readAll(), { QStringLiteral(".json"), QStringLiteral(".xml"), QStringLiteral(".zip") }, GeoZonePack::maxBytes, zipError)) {
            files.insert(file.name, file.data);
        }
    }
    return files;
}

/// Writes a pack with the files of another whose sources in the manifest are edited, like a pack made elsewhere
bool _rewritePack(const QString &packPath, const QString &copyPath, const std::function<void(QJsonObject &source)> &edit)
{
    const QHash<QString, QByteArray> files = _packFiles(packPath);
    QFile copy(copyPath);
    if (files.isEmpty() || !copy.open(QIODevice::WriteOnly)) {
        return false;
    }
    GeoZoneZip::Writer writer(&copy);
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QByteArray data = it.value();
        if (it.key() == QStringLiteral("manifest.json")) {
            QJsonObject manifest = QJsonDocument::fromJson(data).object();
            QJsonArray sources;
            for (const QJsonValue &value : manifest.value(QStringLiteral("sources")).toArray()) {
                QJsonObject source = value.toObject();
                edit(source);
                sources.append(source);
            }
            manifest.insert(QStringLiteral("sources"), sources);
            data = QJsonDocument(manifest).toJson();
        }
        if (!writer.add(it.key(), data)) {
            return false;
        }
    }
    return writer.finish();
}

/// A pack file
struct PackEntry {
    QString name;
    QByteArray data;
};

bool _writePack(const QString &packPath, const QList<PackEntry> &entries)
{
    QFile pack(packPath);
    if (!pack.open(QIODevice::WriteOnly)) {
        return false;
    }
    GeoZoneZip::Writer writer(&pack);
    for (const PackEntry &entry : entries) {
        if (!writer.add(entry.name, entry.data)) {
            return false;
        }
    }
    return writer.finish();
}

/// @return The manifest of a pack with the sources
QByteArray _manifest(const QJsonArray &sources)
{
    return QJsonDocument(QJsonObject{ { QStringLiteral("format"), QStringLiteral("qgc-geozone-pack") }, { QStringLiteral("version"), 1 },
                                      { QStringLiteral("sources"), sources } }).toJson();
}

/// @return A source of a manifest whose copy is the data, with its SHA-256
QJsonObject _manifestSource(const QString &key, const QString &file, const QByteArray &data)
{
    return QJsonObject{ { QStringLiteral("key"), key }, { QStringLiteral("name"), key }, { QStringLiteral("file"), file },
                        { QStringLiteral("fileSha256"), QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) } };
}

} // namespace

void GeoZonePackTest::_testPackExtraction()
{
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString packPath = dir.filePath(QStringLiteral("pack.zip"));
    const QString extractDir = dir.filePath(QStringLiteral("extracted"));
    const auto extractedFiles = [&extractDir]() { return QDir(extractDir).entryList(QDir::Files); };
    // Larger than the manifest, so that it is extracted first
    const QByteArray data = R"({ "features": [] })" + QByteArray(4096, ' ');
    const QString file = QStringLiteral("sources/a.json");
    const QString notAPack = QStringLiteral("The file is not a geozone pack.");

    // The copy of a source is extracted for the caller
    QVERIFY(_writePack(packPath, { { file, data }, { QStringLiteral("manifest.json"), _manifest({ _manifestSource(QStringLiteral("/zones/a.json"), file, data) }) } }));
    {
        const GeoZonePack::Contents contents = GeoZonePack::read(packPath, extractDir);
        QVERIFY2(contents.errorString.isEmpty(), qPrintable(contents.errorString));
        QVERIFY(contents.errors.isEmpty());
        QCOMPARE(contents.sources.count(), 1);
        QCOMPARE(extractedFiles(), QStringList{ QFileInfo(contents.sources.constFirst().filePath).fileName() });
        QVERIFY(QFile::remove(contents.sources.constFirst().filePath));
    }

    // A copy which can't be written is why the pack can't be read, not that it isn't a pack
    {
        const QString blocked = dir.filePath(QStringLiteral("blocked"));
        QFile blocker(blocked);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        const GeoZonePack::Contents contents = GeoZonePack::read(packPath, blocked + QStringLiteral("/extracted"));
        QVERIFY(contents.sources.isEmpty());
        QVERIFY2(contents.errorString.startsWith(QStringLiteral("The files of the pack could not be extracted to")), qPrintable(contents.errorString));
        QVERIFY(contents.errorString != notAPack);
    }

    // The copy of a source without key isn't left behind
    QVERIFY(_writePack(packPath, { { file, data }, { QStringLiteral("manifest.json"), _manifest({ _manifestSource(QString(), file, data) }) } }));
    {
        const GeoZonePack::Contents contents = GeoZonePack::read(packPath, extractDir);
        QVERIFY2(contents.errorString.isEmpty(), qPrintable(contents.errorString));
        QCOMPARE(contents.errors.count(), 1);
        QVERIFY(contents.sources.isEmpty());
        QCOMPARE(extractedFiles(), QStringList());
    }

    // nor are the copies of a pack with two files of one name, which is refused
    QVERIFY(_writePack(packPath, { { file, data }, { file, data + ' ' },
                                   { QStringLiteral("manifest.json"), _manifest({ _manifestSource(QStringLiteral("/zones/a.json"), file, data) }) } }));
    {
        const GeoZonePack::Contents contents = GeoZonePack::read(packPath, extractDir);
        QVERIFY(contents.sources.isEmpty());
        QVERIFY2(contents.errorString.contains(QStringLiteral("several files named sources/a.json")), qPrintable(contents.errorString));
        QCOMPARE(extractedFiles(), QStringList());
    }

    // nor those the manifest doesn't name, or of a file which isn't a pack
    QVERIFY(_writePack(packPath, { { file, data }, { QStringLiteral("sources/b.json"), data }, { QStringLiteral("manifest.json"), _manifest({}) } }));
    QVERIFY(GeoZonePack::read(packPath, extractDir).errorString.isEmpty());
    QCOMPARE(extractedFiles(), QStringList());
    QVERIFY(_writePack(packPath, { { file, data }, { QStringLiteral("manifest.json"), "{}" } }));
    QCOMPARE(GeoZonePack::read(packPath, extractDir).errorString, notAPack);
    QCOMPARE(extractedFiles(), QStringList());

    // A pack whose signature file isn't one is refused, also by builds which take signed packs as unsigned
    const QByteArray manifest = _manifest({ _manifestSource(QStringLiteral("/zones/a.json"), file, data) });
    const auto isKnown = [](const QString &) { return false; };
    for (const QByteArray &signature : { QByteArray("{ \"algorithm\": \"Ed25519\", "), QByteArray("[]"), QByteArray("signed") }) {
        QVERIFY(_writePack(packPath, { { file, data }, { QStringLiteral("manifest.json"), manifest }, { QStringLiteral("signature.json"), signature } }));
        for (const bool available : { true, false }) {
            std::optional<GeoZonePackSignature::ScopedUnavailable> unavailable;
            if (!available) {
                unavailable.emplace();
            }
            const GeoZonePackExchange::Import import = GeoZonePackExchange::read(packPath, extractDir, isKnown, QDateTime::currentDateTimeUtc());
            QVERIFY(import.sources.isEmpty());
            QVERIFY2(import.errorString.contains(QStringLiteral("Its signature file is damaged")), qPrintable(import.errorString));
            QCOMPARE(extractedFiles(), QStringList());
        }
    }
    // One without signature file is read
    QVERIFY(_writePack(packPath, { { file, data }, { QStringLiteral("manifest.json"), manifest } }));
    const GeoZonePackExchange::Import import = GeoZonePackExchange::read(packPath, extractDir, isKnown, QDateTime::currentDateTimeUtc());
    QVERIFY2(import.errorString.isEmpty(), qPrintable(import.errorString));
    QCOMPARE(import.sources.count(), 1);
    QCOMPARE(import.signature, GeoZoneProvenance::PackSignature::Unsigned);
    QVERIFY(QFile::remove(import.sources.constFirst().job.sourcePath));
}

void GeoZonePackTest::_testOfflinePack()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QFile ed269(QStringLiteral(":/unittest/ed269_test.json"));
    QVERIFY(ed269.open(QIODevice::ReadOnly));
    server.files.insert("/keyed", server.body);
    server.files.insert("/plain", ed269.readAll());
    server.etags.insert("/plain", "\"e1\"");
    server.files.insert("/area", server.body);

    const QTemporaryDir storageDir;
    const QTemporaryDir importDir;
    const QTemporaryDir packDir;
    const QTemporaryDir sourceDir;
    QVERIFY(storageDir.isValid() && importDir.isValid() && packDir.isValid() && sourceDir.isValid());
    const QString packPath = packDir.filePath(QStringLiteral("field.zip"));
    const QString keyedUrl = server.url(QStringLiteral("/keyed"));
    const QString plainUrl = server.url(QStringLiteral("/plain"));
    const QString areaUrl = server.url(QStringLiteral("/area?bbox={bbox}"));

    GeoZoneManager manager(storageDir.path());
    manager.repository()->setVehiclePosition(QGeoCoordinate(47.0, 8.0));
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    GeoZoneService keyed;
    keyed.auth = GeoZoneService::Auth::ApiKeyHeader;
    keyed.authParameter = QStringLiteral("X-Api-Key");
    keyed.headers.append({ QStringLiteral("X-Client-Key"), QStringLiteral("pack-header-s3cret") });
    manager.addService(keyedUrl, keyed.toVariantMap(), QStringLiteral("pack-s3cret"));
    QVERIFY(addedSpy.wait(10000));
    manager.repository()->addUrl(plainUrl);
    QVERIFY(addedSpy.wait(10000));
    manager.addService(areaUrl, GeoZoneService().toVariantMap(), QString());
    QVERIFY(addedSpy.wait(10000));
    QString errorString;
    const QString fileName = _writeFeatures(sourceDir, QStringLiteral("local.json"), QJsonArray{
        _circleFeature(QStringLiteral("L1"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.0, 8.0), 500) });
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(manager.sources()->count(), 4);

    // The sources turned on are exported
    manager.repository()->source(GeoZoneLoader::fileKey(fileName))->setEnabled(false);
    QSignalSpy fileSpy(&manager, &GeoZoneManager::fileOperationFinished);
    QVERIFY(manager.exportPack(packPath));
    QVERIFY(fileSpy.last()[0].toBool());

    // without secrets, nor the headers of the services, which may hold keys
    QFile pack(packPath);
    QVERIFY(pack.open(QIODevice::ReadOnly));
    const QByteArray packBytes = pack.readAll();
    QString zipError;
    const QList<GeoZoneZip::File> files = GeoZoneZip::files(packBytes, { QStringLiteral(".json") }, GeoZonePack::maxBytes, zipError);
    QVERIFY2(zipError.isEmpty(), qPrintable(zipError));
    QCOMPARE(files.count(), 4);
    QVERIFY(!packBytes.contains("pack-s3cret"));
    QJsonObject manifest;
    for (const GeoZoneZip::File &file : files) {
        QVERIFY(!file.data.contains("pack-s3cret"));
        QVERIFY(!file.data.contains("pack-header-s3cret"));
        if (file.name == QStringLiteral("manifest.json")) {
            manifest = QJsonDocument::fromJson(file.data).object();
        }
    }
    QVERIFY(!QJsonDocument(GeoZoneSourceRepository::sourceReport(*manager.repository()->source(keyedUrl))).toJson().contains("pack-header-s3cret"));
    QCOMPARE(manager.repository()->source(keyedUrl)->service().headers.count(), 1);
    QCOMPARE(manifest.value("format").toString(), QStringLiteral("qgc-geozone-pack"));
    const QJsonArray packSources = manifest.value("sources").toArray();
    QCOMPARE(packSources.count(), 3);
    QCOMPARE(packSources.first().toObject().value("service").toObject().value("authParameter").toString(), QStringLiteral("X-Api-Key"));

    // Imported on another computer, which doesn't have the secret
    GeoZoneCredentials().remove(keyedUrl);
    GeoZoneManager imported(importDir.path());
    QSignalSpy importSpy(&imported, &GeoZoneManager::fileOperationFinished);
    QVERIFY(imported.importPack(packPath));
    QVERIFY(importSpy.last()[0].toBool());
    QCOMPARE(importSpy.last()[1].toString(), QStringLiteral("3 geozone source(s) imported from field.zip. The pack is not signed."));
    QTRY_VERIFY(!imported.repository()->busy());
    QCOMPARE(imported.sources()->count(), 3);
    QVERIFY(!imported.repository()->source(GeoZoneLoader::fileKey(fileName)));
    const auto zoneHashes = [](const GeoZoneSource *source) {
        QList<QByteArray> hashes;
        for (const GeoZone *zone : source->zones()) {
            hashes.append(zone->identifier().toUtf8() + zone->data().contentHash);
        }
        std::sort(hashes.begin(), hashes.end());
        return hashes;
    };
    for (const QString &key : { keyedUrl, plainUrl, areaUrl }) {
        const GeoZoneSource *const original = manager.repository()->source(key);
        const GeoZoneSource *const copy = imported.repository()->source(key);
        QVERIFY2(copy, qPrintable(key));
        QCOMPARE(copy->zoneCount(), original->zoneCount());
        QCOMPARE(zoneHashes(copy), zoneHashes(original));
        QCOMPARE(copy->formatName(), original->formatName());
        QCOMPARE(copy->loadedAt(), original->loadedAt());
        QCOMPARE(copy->dataDate(), original->dataDate());
        QCOMPARE(copy->provenance().finalUrl, original->provenance().finalUrl);
        QCOMPARE(copy->provenance().sha256, original->provenance().sha256);
        QCOMPARE(copy->provenance().contentHash, original->provenance().contentHash);
        QCOMPARE(copy->provenance().fetchedAt, original->provenance().fetchedAt);
        QVERIFY(copy->imported());
        QCOMPARE(copy->provenance().packName, QStringLiteral("field.zip"));
        QCOMPARE(copy->provenance().importedAt, environment.now());
    }
    QCOMPARE(imported.zoneCount(), manager.zoneCount());
    QCOMPARE(imported.repository()->source(plainUrl)->etag(), QStringLiteral("\"e1\""));
    QCOMPARE(imported.repository()->source(areaUrl)->area(), manager.repository()->source(areaUrl)->area());
    QVERIFY(imported.repository()->source(areaUrl)->followsArea());
    QVERIFY(imported.repository()->source(keyedUrl)->credentialsMissing());
    QVERIFY(imported.repository()->source(keyedUrl)->provenanceText().startsWith(QStringLiteral("imported from field.zip")));

    // and kept
    GeoZoneManager restarted(importDir.path());
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QCOMPARE(restarted.sources()->count(), 3);
    QVERIFY(restarted.repository()->source(keyedUrl)->imported());
    QCOMPARE(restarted.repository()->source(keyedUrl)->service().auth, GeoZoneService::Auth::ApiKeyHeader);

    // Data which doesn't match the hash of its provenance is left out
    const QString damagedPath = packDir.filePath(QStringLiteral("damaged.zip"));
    {
        QFile damaged(damagedPath);
        QVERIFY(damaged.open(QIODevice::WriteOnly));
        GeoZoneZip::Writer writer(&damaged);
        QJsonObject source = packSources.first().toObject();
        QVERIFY(writer.add(source.value("file").toString(), server.body + " "));
        QVERIFY(writer.add(QStringLiteral("manifest.json"), QJsonDocument(QJsonObject{ { "format", "qgc-geozone-pack" }, { "version", 1 }, { "sources", QJsonArray{ source } } }).toJson()));
        QVERIFY(writer.finish());
    }
    const QTemporaryDir otherDir;
    GeoZoneManager other(otherDir.path());
    QSignalSpy otherSpy(&other, &GeoZoneManager::fileOperationFinished);
    QVERIFY(other.importPack(damagedPath));
    QVERIFY(!otherSpy.last()[0].toBool());
    QVERIFY2(otherSpy.last()[1].toString().contains(QStringLiteral("the data in the pack is damaged")), qPrintable(otherSpy.last()[1].toString()));
    QCOMPARE(other.sources()->count(), 0);
    QVERIFY(!other.importPack(fileName));
    QVERIFY(otherSpy.last()[1].toString().startsWith(QStringLiteral("The geozone pack could not be read.")));
    QCOMPARE(QDir(QDir(otherDir.path()).filePath(QStringLiteral("downloads"))).entryList(QDir::Files).count(), 0);

    // Packs imported together are reported once
    otherSpy.clear();
    QVERIFY(!other.importPacks({ packPath, fileName }));
    QTRY_VERIFY(!other.repository()->busy());
    QCOMPARE(otherSpy.count(), 1);
    QVERIFY(!otherSpy.last()[0].toBool());
    QVERIFY(otherSpy.last()[1].toString().startsWith(QStringLiteral("3 geozone source(s) imported from field.zip. The pack is not signed.\n\nThe geozone pack could not be read.")));
    QCOMPARE(other.sources()->count(), 3);
}

void GeoZonePackTest::_testPackServiceTrust()
{
    ScopedEnvironment environment;
    TestFeedServer server;
    TestFeedServer hostile;
    QVERIFY(server.listen(QHostAddress::LocalHost) && hostile.listen(QHostAddress::LocalHost));
    const QByteArray clientCredentials = "Basic " + QByteArray("client:client-s3cret").toBase64();
    int tokenRequests = 0;
    server.handler = [&](const QByteArray &request, QByteArray &status, QByteArray &content, QByteArray &) {
        const QByteArray path = request.split(' ').value(1);
        if (path == "/token") {
            tokenRequests++;
            if (TestFeedServer::header(request, "Authorization") != clientCredentials) {
                status = "401 Unauthorized";
            } else {
                content = R"({ "access_token": "token-1", "token_type": "Bearer", "expires_in": 3600 })";
            }
            return true;
        }
        if (path == "/zones") {
            if (TestFeedServer::header(request, "Authorization") != "Bearer token-1") {
                status = "401 Unauthorized";
            } else {
                content = server.body;
            }
            return true;
        }
        return false;
    };

    const QTemporaryDir storageDir;
    const QTemporaryDir otherDir;
    const QTemporaryDir packDir;
    QVERIFY(storageDir.isValid() && otherDir.isValid() && packDir.isValid());
    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);
    GeoZoneService service;
    service.auth = GeoZoneService::Auth::OAuth2ClientCredentials;
    service.tokenUrl = server.url(QStringLiteral("/token"));
    service.user = QStringLiteral("client");
    const QString url = server.url(QStringLiteral("/zones"));

    // A token URL on another server is only used once the user confirmed it
    GeoZoneService elsewhere = service;
    elsewhere.tokenUrl = hostile.url(QStringLiteral("/token"));
    QVERIFY(manager.serviceOtherOrigins(url, service.toVariantMap()).isEmpty());
    QCOMPARE(manager.serviceOtherOrigins(url, elsewhere.toVariantMap()), QStringList{ hostile.url(QString()) });
    manager.addService(url, elsewhere.toVariantMap(), QStringLiteral("client-s3cret"));
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY2(failedSpy.last()[1].toString().contains(hostile.url(QString())), qPrintable(failedSpy.last()[1].toString()));
    QVERIFY(manager.repository()->credentials().secret(url).isEmpty());

    manager.addService(url, service.toVariantMap(), QStringLiteral("client-s3cret"));
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    GeoZoneSource *const feed = manager.repository()->source(url);
    QVERIFY(feed);
    QCOMPARE(tokenRequests, 1);
    const QString packPath = packDir.filePath(QStringLiteral("feed.zip"));
    QVERIFY(manager.exportPack(packPath));

    // A pack redirecting the secret of a source and the area of interest to another server, with headers of its own
    const QString hostilePackPath = packDir.filePath(QStringLiteral("hostile.zip"));
    QVERIFY(_rewritePack(packPath, hostilePackPath, [&hostile](QJsonObject &source) {
        QJsonObject packService = source.value(QStringLiteral("service")).toObject();
        packService.insert(QStringLiteral("tokenUrl"), hostile.url(QStringLiteral("/token")));
        packService.insert(QStringLiteral("sha256Url"), hostile.url(QStringLiteral("/hash?of={url}")));
        packService.insert(QStringLiteral("headers"), QJsonObject{ { QStringLiteral("X-Pack"), QStringLiteral("injected") } });
        source.insert(QStringLiteral("service"), packService);
    }));

    // updates the zones of the source, which keeps its service and secret
    QVERIFY(manager.importPack(hostilePackPath));
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(feed->imported());
    QCOMPARE(feed->service(), service);
    QCOMPARE(manager.repository()->credentials().secret(url), QStringLiteral("client-s3cret"));
    environment.addSecs(2 * 3600);
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY2(feed->errorString().isEmpty(), qPrintable(feed->errorString()));
    QCOMPARE(tokenRequests, 2);
    for (const QByteArray &request : std::as_const(server.requests)) {
        QVERIFY(TestFeedServer::header(request, "X-Pack").isEmpty());
    }

    // The secret entered for a token URL is forgotten when the user changes it
    GeoZoneService moved = service;
    moved.tokenUrl = server.url(QStringLiteral("/token2"));
    const qsizetype failures = failedSpy.count();
    manager.addService(url, moved.toVariantMap(), QString());
    QTRY_COMPARE(failedSpy.count(), failures + 1);
    QVERIFY(feed->credentialsMissing());
    QVERIFY(manager.repository()->credentials().secret(url).isEmpty());
    QCOMPARE(tokenRequests, 2);

    // A new source of a pack doesn't take a service sending requests to another server
    GeoZoneCredentials().setSecret(url, QStringLiteral("client-s3cret"));
    GeoZoneManager other(otherDir.path());
    QSignalSpy otherSpy(&other, &GeoZoneManager::fileOperationFinished);
    QVERIFY(other.importPack(hostilePackPath));
    QVERIFY(!otherSpy.last()[0].toBool());
    QVERIFY2(otherSpy.last()[1].toString().contains(hostile.url(QString())), qPrintable(otherSpy.last()[1].toString()));
    QCOMPARE(other.sources()->count(), 0);
    QCOMPARE(QDir(QDir(otherDir.path()).filePath(QStringLiteral("downloads"))).entryList(QDir::Files).count(), 0);

    // nor a source requested elsewhere than the address its service was checked against
    const QString mismatchPackPath = packDir.filePath(QStringLiteral("mismatch.zip"));
    QVERIFY(_rewritePack(packPath, mismatchPackPath, [&hostile](QJsonObject &source) {
        source.insert(QStringLiteral("url"), hostile.url(QStringLiteral("/zones")));
        QJsonObject packService = source.value(QStringLiteral("service")).toObject();
        packService.insert(QStringLiteral("tokenUrl"), hostile.url(QStringLiteral("/token")));
        source.insert(QStringLiteral("service"), packService);
    }));
    QVERIFY(other.importPack(mismatchPackPath));
    QVERIFY(!otherSpy.last()[0].toBool());
    QVERIFY2(otherSpy.last()[1].toString().contains(QStringLiteral("its address doesn't match the source")), qPrintable(otherSpy.last()[1].toString()));
    QCOMPARE(other.sources()->count(), 0);
    QVERIFY(manager.importPack(mismatchPackPath));
    QCOMPARE(feed->service(), moved);

    // nor the headers of the pack, nor the secret kept for its URL, which the user enters again
    const QString headersPackPath = packDir.filePath(QStringLiteral("headers.zip"));
    QVERIFY(_rewritePack(packPath, headersPackPath, [](QJsonObject &source) {
        QJsonObject packService = source.value(QStringLiteral("service")).toObject();
        packService.insert(QStringLiteral("headers"), QJsonObject{ { QStringLiteral("X-Pack"), QStringLiteral("injected") } });
        source.insert(QStringLiteral("service"), packService);
    }));
    QVERIFY(other.importPack(headersPackPath));
    QVERIFY(otherSpy.last()[0].toBool());
    QTRY_VERIFY(!other.repository()->busy());
    GeoZoneSource *const copy = other.repository()->source(url);
    QVERIFY(copy);
    QVERIFY(copy->service().headers.isEmpty());
    QCOMPARE(copy->service().tokenUrl, service.tokenUrl);
    QVERIFY(copy->credentialsMissing());
    QVERIFY(GeoZoneCredentials().secret(url).isEmpty());
    other.repository()->setSecret(url, QStringLiteral("client-s3cret"));
    QTRY_VERIFY(!other.repository()->busy());
    QVERIFY2(copy->errorString().isEmpty(), qPrintable(copy->errorString()));
    QCOMPARE(tokenRequests, 3);

    QVERIFY(hostile.requests.isEmpty());
    other.repository()->clear();
    manager.repository()->clear();
}

void GeoZonePackTest::_testSignedPacks()
{
    if (!GeoZonePackSignature::available()) {
        QSKIP("This build can't sign packs");
    }
    const ScopedEnvironment environment;
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    const auto restore = qScopeGuard([settings]() {
        settings->packSigningKeyFile()->setRawValue(QString());
        settings->packPublisherName()->setRawValue(QString());
        settings->trustedPackPublishers()->setRawValue(QString());
    });

    // Keys made for the test
    const QByteArray privateKey = GeoZonePackSignature::generatePrivateKey();
    QVERIFY(privateKey.startsWith("-----BEGIN PRIVATE KEY-----"));
    QString errorString;
    const QByteArray publicKey = GeoZonePackSignature::publicKey(privateKey, errorString);
    QCOMPARE(publicKey.size(), GeoZonePackSignature::publicKeySize);
    const QByteArray otherPublicKey = GeoZonePackSignature::publicKey(GeoZonePackSignature::generatePrivateKey(), errorString);
    QVERIFY(!otherPublicKey.isEmpty() && (otherPublicKey != publicKey));
    const QByteArray signature = GeoZonePackSignature::sign(privateKey, "manifest", errorString);
    QCOMPARE(signature.size(), GeoZonePackSignature::signatureSize);
    QVERIFY(GeoZonePackSignature::verify(publicKey, "manifest", signature));
    QVERIFY(!GeoZonePackSignature::verify(publicKey, "manifest!", signature));
    QVERIFY(!GeoZonePackSignature::verify(otherPublicKey, "manifest", signature));
    QVERIFY(GeoZonePackSignature::publicKey("not a key", errorString).isEmpty());
    QVERIFY(!errorString.isEmpty());
    // Encrypted keys are refused rather than asking for their passphrase
    QVERIFY(GeoZonePackSignature::publicKey("-----BEGIN ENCRYPTED PRIVATE KEY-----\n"
                                            "MIGbMFcGCSqGSIb3DQEFDTBKMCkGCSqGSIb3DQEFDDAcBAg7UG1U/8LOlQICCAAw\n"
                                            "DAYIKoZIhvcNAgkFADAdBglghkgBZQMEASoEECmJlPVm8BujMIh6J03w/1gEQIt+\n"
                                            "hodb50orx9YuXR+6Ieaq/t1vGeVWASzoilaIFhl3GOg9EMeMIMS48tcEiCzRlnIQ\n"
                                            "XhE+UU0t/+4fa2ADlQ4=\n"
                                            "-----END ENCRYPTED PRIVATE KEY-----\n", errorString).isEmpty());

    // Public keys are given in base64 or PEM, as openssl pkey -pubout writes them
    const QByteArray spki = QByteArray::fromHex("302a300506032b6570032100") + publicKey;
    QCOMPARE(GeoZonePackSignature::parsePublicKey(QString::fromLatin1(publicKey.toBase64())), publicKey);
    QCOMPARE(GeoZonePackSignature::parsePublicKey(QStringLiteral("-----BEGIN PUBLIC KEY-----\n%1\n-----END PUBLIC KEY-----\n").arg(QString::fromLatin1(spki.toBase64()))), publicKey);
    QVERIFY(GeoZonePackSignature::parsePublicKey(QStringLiteral("c2hvcnQ=")).isEmpty());

    const QTemporaryDir storageDir;
    const QTemporaryDir importDir;
    const QTemporaryDir packDir;
    QVERIFY(storageDir.isValid() && importDir.isValid() && packDir.isValid());
    GeoZoneManager manager(storageDir.path());
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(packDir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("S1"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.0, 8.0), 500) }), errorString), qPrintable(errorString));
    const QString key = manager.sources()->value<GeoZoneSource*>(0)->key();

    // Packs are unsigned without a key, their manifest holds the SHA-256 of the copies
    const QString unsignedPath = packDir.filePath(QStringLiteral("unsigned.zip"));
    QVERIFY(manager.exportPack(unsignedPath));
    QHash<QString, QByteArray> files = _packFiles(unsignedPath);
    QVERIFY(!files.contains(QStringLiteral("signature.json")));
    const QJsonObject manifestSource = QJsonDocument::fromJson(files.value(QStringLiteral("manifest.json"))).object().value(QStringLiteral("sources")).toArray().first().toObject();
    QCOMPARE(manifestSource.value(QStringLiteral("fileSha256")).toString(),
             QString::fromLatin1(QCryptographicHash::hash(files.value(manifestSource.value(QStringLiteral("file")).toString()), QCryptographicHash::Sha256).toHex()));

    // and signed with one
    const QString keyPath = packDir.filePath(QStringLiteral("publisher.pem"));
    {
        QFile keyFile(keyPath);
        QVERIFY(keyFile.open(QIODevice::WriteOnly) && (keyFile.write(privateKey) == privateKey.size()));
    }
    settings->packSigningKeyFile()->setRawValue(keyPath);
    settings->packPublisherName()->setRawValue(QStringLiteral("Survey Team"));
    QCOMPARE(manager.packPublicKey(), QString::fromLatin1(publicKey.toBase64()));
    QCOMPARE(manager.packKeyId(), GeoZonePackSignature::keyId(publicKey));
    const QString signedPath = packDir.filePath(QStringLiteral("signed.zip"));
    QVERIFY(manager.exportPack(signedPath));
    files = _packFiles(signedPath);
    const QJsonObject signatureObject = QJsonDocument::fromJson(files.value(QStringLiteral("signature.json"))).object();
    QCOMPARE(signatureObject.value(QStringLiteral("algorithm")).toString(), QStringLiteral("Ed25519"));
    QCOMPARE(signatureObject.value(QStringLiteral("publisher")).toString(), QStringLiteral("Survey Team"));
    QCOMPARE(signatureObject.value(QStringLiteral("publicKey")).toString(), QString::fromLatin1(publicKey.toBase64()));
    QVERIFY(GeoZonePackSignature::verify(publicKey, files.value(QStringLiteral("manifest.json")),
                                         QByteArray::fromBase64(signatureObject.value(QStringLiteral("signature")).toString().toLatin1())));

    // Imported packs are marked unsigned
    GeoZoneManager imported(importDir.path());
    QSignalSpy importSpy(&imported, &GeoZoneManager::fileOperationFinished);
    QVERIFY(imported.importPack(unsignedPath));
    QVERIFY2(importSpy.last()[1].toString().endsWith(QStringLiteral("The pack is not signed.")), qPrintable(importSpy.last()[1].toString()));
    QTRY_VERIFY(!imported.repository()->busy());
    GeoZoneSource *const source = imported.repository()->source(key);
    QVERIFY(source);
    QCOMPARE(source->provenance().packSignature, GeoZoneProvenance::PackSignature::Unsigned);
    QVERIFY2(source->provenanceText().contains(QStringLiteral("unsigned")), qPrintable(source->provenanceText()));

    // untrusted when signed by a publisher who isn't trusted
    QVERIFY(imported.importPack(signedPath));
    QVERIFY2(importSpy.last()[1].toString().endsWith(QStringLiteral("The pack is signed by \"Survey Team\" with the key %1, which is not trusted.").arg(GeoZonePackSignature::keyId(publicKey))),
             qPrintable(importSpy.last()[1].toString()));
    QTRY_VERIFY(!imported.repository()->busy());
    QVERIFY(source->untrustedPack());
    QVERIFY2(source->provenanceText().contains(QStringLiteral("untrusted signature of \"Survey Team\"")), qPrintable(source->provenanceText()));

    // and signed by the publisher once trusted
    QVERIFY(imported.trustSourcePublisher(key));
    QCOMPARE(imported.trustedPublishers().count(), 1);
    QCOMPARE(source->provenance().packSignature, GeoZoneProvenance::PackSignature::Signed);
    QVERIFY(!source->untrustedPack());
    QVERIFY(imported.trustPublisher(QStringLiteral("Field survey"), QString::fromLatin1(publicKey.toBase64())));
    QCOMPARE(imported.trustedPublishers().count(), 1);
    QVERIFY(imported.importPack(signedPath));
    QVERIFY2(importSpy.last()[1].toString().endsWith(QStringLiteral("The pack is signed by Field survey.")), qPrintable(importSpy.last()[1].toString()));
    QTRY_VERIFY(!imported.repository()->busy());
    QVERIFY2(source->provenanceText().contains(QStringLiteral("signed by Field survey")), qPrintable(source->provenanceText()));
    {
        GeoZoneManager restarted(importDir.path());
        restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
        QCOMPARE(restarted.repository()->source(key)->provenance().packSignature, GeoZoneProvenance::PackSignature::Signed);
        QCOMPARE(restarted.repository()->source(key)->provenance().packPublicKey, publicKey);
    }

    // A pack changed after it was signed isn't imported
    const QString changedPath = packDir.filePath(QStringLiteral("changed.zip"));
    QVERIFY(_rewritePack(signedPath, changedPath, [](QJsonObject &packSource) { packSource.insert(QStringLiteral("name"), QStringLiteral("Changed")); }));
    QVERIFY(!imported.importPack(changedPath));
    QVERIFY2(importSpy.last()[1].toString().contains(QStringLiteral("changed after it was signed")), qPrintable(importSpy.last()[1].toString()));
    QCOMPARE(QDir(QDir(importDir.path()).filePath(QStringLiteral("downloads"))).entryList(QDir::Files).count(), 0);

    // nor are copies which don't match the SHA-256 of the manifest
    const QString replacedPath = packDir.filePath(QStringLiteral("replaced.zip"));
    {
        QFile replaced(replacedPath);
        QVERIFY(replaced.open(QIODevice::WriteOnly));
        GeoZoneZip::Writer writer(&replaced);
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            QVERIFY(writer.add(it.key(), it.key().startsWith(QStringLiteral("sources/")) ? (it.value() + " ") : it.value()));
        }
        QVERIFY(writer.finish());
    }
    QVERIFY(imported.importPack(replacedPath));
    QVERIFY(!importSpy.last()[0].toBool());
    QVERIFY2(importSpy.last()[1].toString().contains(QStringLiteral("the data in the pack is damaged")), qPrintable(importSpy.last()[1].toString()));

    // A publisher no longer trusted makes the source untrusted again
    imported.distrustPublisher(QString::fromLatin1(publicKey.toBase64()));
    QVERIFY(imported.trustedPublishers().isEmpty());
    QVERIFY(source->untrustedPack());
}

void GeoZonePackTest::_testPacksWithoutSigning()
{
    // Runs the code of builds without OpenSSL in any build
    const GeoZonePackSignature::ScopedUnavailable unavailable;
    const ScopedEnvironment environment;
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    const auto restore = qScopeGuard([settings]() {
        settings->packSigningKeyFile()->setRawValue(QString());
        settings->packPublisherName()->setRawValue(QString());
        settings->trustedPackPublishers()->setRawValue(QString());
    });
    QVERIFY(!GeoZonePackSignature::available());
    QVERIFY(!GeoZoneManager::packSigningAvailable());
    QVERIFY(GeoZonePackSignature::generatePrivateKey().isEmpty());
    QString errorString;
    QVERIFY(GeoZonePackSignature::sign("-----BEGIN PRIVATE KEY-----", "manifest", errorString).isEmpty());
    QCOMPARE(errorString, QStringLiteral("This build of QGroundControl can't sign packs."));
    const QByteArray publicKey(GeoZonePackSignature::publicKeySize, '\x01');
    QVERIFY(!GeoZonePackSignature::verify(publicKey, "manifest", QByteArray(GeoZonePackSignature::signatureSize, '\x02')));

    const QTemporaryDir storageDir;
    const QTemporaryDir importDir;
    const QTemporaryDir packDir;
    QVERIFY(storageDir.isValid() && importDir.isValid() && packDir.isValid());
    GeoZoneManager manager(storageDir.path());
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(packDir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("S1"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.0, 8.0), 500) }), errorString), qPrintable(errorString));
    const QString key = manager.sources()->value<GeoZoneSource*>(0)->key();

    // Packs are exported unsigned, also with a signing key in the settings shared with other builds
    const QString keyPath = packDir.filePath(QStringLiteral("publisher.pem"));
    {
        QFile keyFile(keyPath);
        QVERIFY(keyFile.open(QIODevice::WriteOnly) && (keyFile.write("-----BEGIN PRIVATE KEY-----\n") > 0));
    }
    settings->packSigningKeyFile()->setRawValue(keyPath);
    settings->packPublisherName()->setRawValue(QStringLiteral("Survey Team"));
    QVERIFY(manager.packPublicKey().isEmpty());
    QSignalSpy fileSpy(&manager, &GeoZoneManager::fileOperationFinished);
    const QString unsignedPath = packDir.filePath(QStringLiteral("unsigned.zip"));
    QVERIFY(manager.exportPack(unsignedPath));
    QVERIFY2(fileSpy.last()[1].toString().endsWith(QStringLiteral(", unsigned.")), qPrintable(fileSpy.last()[1].toString()));
    QHash<QString, QByteArray> files = _packFiles(unsignedPath);
    QVERIFY(!files.contains(QStringLiteral("signature.json")));
    QVERIFY(!manager.createSigningKey(packDir.filePath(QStringLiteral("new.pem"))));
    QVERIFY(!QFile::exists(packDir.filePath(QStringLiteral("new.pem"))));

    // Signed packs are imported as unsigned, also when their key is trusted
    files.insert(QStringLiteral("signature.json"), QJsonDocument(QJsonObject{
        { QStringLiteral("algorithm"), QStringLiteral("Ed25519") },
        { QStringLiteral("publisher"), QStringLiteral("Survey Team") },
        { QStringLiteral("publicKey"), QString::fromLatin1(publicKey.toBase64()) },
        { QStringLiteral("signature"), QString::fromLatin1(QByteArray(GeoZonePackSignature::signatureSize, '\x02').toBase64()) },
    }).toJson());
    const QString signedPath = packDir.filePath(QStringLiteral("signed.zip"));
    {
        QFile signedPack(signedPath);
        QVERIFY(signedPack.open(QIODevice::WriteOnly));
        GeoZoneZip::Writer writer(&signedPack);
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            QVERIFY(writer.add(it.key(), it.value()));
        }
        QVERIFY(writer.finish());
    }
    GeoZoneManager imported(importDir.path());
    QVERIFY(imported.trustPublisher(QStringLiteral("Survey Team"), QString::fromLatin1(publicKey.toBase64())));
    QSignalSpy importSpy(&imported, &GeoZoneManager::fileOperationFinished);
    QVERIFY(imported.importPack(signedPath));
    QVERIFY(importSpy.last()[0].toBool());
    QVERIFY2(importSpy.last()[1].toString().endsWith(QStringLiteral("The pack is signed, but its signature can't be checked in this build, so it is taken as unsigned.")),
             qPrintable(importSpy.last()[1].toString()));
    QTRY_VERIFY(!imported.repository()->busy());
    GeoZoneSource *const source = imported.repository()->source(key);
    QVERIFY(source);
    QCOMPARE(source->provenance().packSignature, GeoZoneProvenance::PackSignature::Unverified);
    QVERIFY(source->provenance().packSigner.isEmpty());
    QVERIFY(source->provenance().packPublicKey.isEmpty());
    QVERIFY(!source->untrustedPack());
    QVERIFY2(source->provenanceText().contains(QStringLiteral("signature not checked, taken as unsigned")), qPrintable(source->provenanceText()));
    QVERIFY(!imported.trustSourcePublisher(key));
    imported.distrustPublisher(QString::fromLatin1(publicKey.toBase64()));
    QVERIFY(imported.trustPublisher(QStringLiteral("Survey Team"), QString::fromLatin1(publicKey.toBase64())));
    QCOMPARE(source->provenance().packSignature, GeoZoneProvenance::PackSignature::Unverified);
}

void GeoZonePackTest::_testStoredFileNames()
{
    const ScopedEnvironment environment;
    const QTemporaryDir storageDir;
    const QTemporaryDir importDir;
    const QTemporaryDir fileDir;
    QVERIFY(storageDir.isValid() && importDir.isValid() && fileDir.isValid());
    QFile aixm(QStringLiteral(":/unittest/uk_aixm_test.xml"));
    QVERIFY(aixm.open(QIODevice::ReadOnly));
    const QByteArray xml = aixm.readAll();
    const QString xmlPath = fileDir.filePath(QStringLiteral("zones.xml"));
    const QString zipPath = fileDir.filePath(QStringLiteral("zones.zip"));
    {
        QFile xmlFile(xmlPath);
        QVERIFY(xmlFile.open(QIODevice::WriteOnly) && (xmlFile.write(xml) == xml.size()));
        QFile zipFile(zipPath);
        QVERIFY(zipFile.open(QIODevice::WriteOnly));
        GeoZoneZip::Writer writer(&zipFile);
        QVERIFY(writer.add(QStringLiteral("zones.xml"), xml) && writer.finish());
    }

    // The stored copies are named after their content
    GeoZoneManager manager(storageDir.path());
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(xmlPath, errorString), qPrintable(errorString));
    QVERIFY2(manager.repository()->loadFile(zipPath, errorString), qPrintable(errorString));
    const QString xmlKey = GeoZoneLoader::fileKey(xmlPath);
    const QString zipKey = GeoZoneLoader::fileKey(zipPath);
    const QString xmlFileName = manager.repository()->storedFileName(xmlKey);
    QVERIFY2(xmlFileName.endsWith(QStringLiteral(".xml")), qPrintable(xmlFileName));
    QVERIFY2(manager.repository()->storedFileName(zipKey).endsWith(QStringLiteral(".zip")), qPrintable(manager.repository()->storedFileName(zipKey)));
    const int zoneCount = manager.repository()->source(xmlKey)->zoneCount();
    QVERIFY(zoneCount > 0);
    QCOMPARE(manager.repository()->source(zipKey)->zoneCount(), zoneCount);

    // and kept at the next start
    {
        GeoZoneManager restarted(storageDir.path());
        restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
        QCOMPARE(restarted.repository()->source(xmlKey)->zoneCount(), zoneCount);
        QCOMPARE(restarted.repository()->source(zipKey)->zoneCount(), zoneCount);
        QVERIFY(QFile::exists(QDir(storageDir.path()).filePath(xmlFileName)));
    }

    // Packs hold them with their names
    const QString packPath = fileDir.filePath(QStringLiteral("pack.zip"));
    QVERIFY(manager.exportPack(packPath));
    QHash<QString, QByteArray> files = _packFiles(packPath);
    QCOMPARE(files.value(QStringLiteral("sources/") + xmlFileName), xml);

    // Earlier versions named all copies .json, whatever their content
    const QString oldPackPath = fileDir.filePath(QStringLiteral("old.zip"));
    {
        QJsonObject manifest = QJsonDocument::fromJson(files.value(QStringLiteral("manifest.json"))).object();
        QJsonArray sources;
        for (const QJsonValue &value : manifest.value(QStringLiteral("sources")).toArray()) {
            QJsonObject source = value.toObject();
            if (source.value(QStringLiteral("key")).toString() == xmlKey) {
                source.insert(QStringLiteral("file"), QStringLiteral("sources/old.json"));
                sources.append(source);
            }
        }
        manifest.insert(QStringLiteral("sources"), sources);
        QFile oldPack(oldPackPath);
        QVERIFY(oldPack.open(QIODevice::WriteOnly));
        GeoZoneZip::Writer writer(&oldPack);
        QVERIFY(writer.add(QStringLiteral("sources/old.json"), xml));
        QVERIFY(writer.add(QStringLiteral("manifest.json"), QJsonDocument(manifest).toJson()));
        QVERIFY(writer.finish());
    }
    GeoZoneManager imported(importDir.path());
    QSignalSpy importSpy(&imported, &GeoZoneManager::fileOperationFinished);
    QVERIFY(imported.importPack(oldPackPath));
    QVERIFY2(importSpy.last()[0].toBool(), qPrintable(importSpy.last()[1].toString()));
    QTRY_VERIFY(!imported.repository()->busy());
    QVERIFY(imported.repository()->source(xmlKey));
    QCOMPARE(imported.repository()->source(xmlKey)->zoneCount(), zoneCount);
    QVERIFY2(imported.repository()->storedFileName(xmlKey).endsWith(QStringLiteral(".xml")), qPrintable(imported.repository()->storedFileName(xmlKey)));
    QVERIFY(imported.importPack(packPath));
    QTRY_VERIFY(!imported.repository()->busy());
    QCOMPARE(imported.repository()->source(zipKey)->zoneCount(), zoneCount);
}

GEOZONE_TEST(GeoZonePackTest)
