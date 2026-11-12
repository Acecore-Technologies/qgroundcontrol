/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneServiceTest.h"
#include "GeoZoneCredentials.h"
#include "GeoZoneTestBase.h"
#include "GeoZoneTestHelpers.h"

#include <QtCore/QCryptographicHash>

using namespace GeoZoneTestHelpers;

namespace {

/// Captures the messages logged while in scope, with the debug messages of the geozone categories enabled
class ScopedLogCapture
{
public:
    ScopedLogCapture()
    {
        _instance = this;
        // Called at once for the existing categories, which the previous filter configured already
        _previousFilter = QLoggingCategory::installFilter([](QLoggingCategory *category) {
            if (_instance->_previousFilter) {
                _instance->_previousFilter(category);
            }
            if (QByteArray(category->categoryName()).startsWith("qgc.geozone")) {
                category->setEnabled(QtDebugMsg, true);
            }
        });
        _previousHandler = qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
            const QMutexLocker locker(&_instance->_mutex);
            _instance->_messages.append(message);
        });
    }

    ~ScopedLogCapture()
    {
        (void) qInstallMessageHandler(_previousHandler);
        // Applies the previous filter to the categories again
        (void) QLoggingCategory::installFilter(_previousFilter);
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
    QLoggingCategory::CategoryFilter _previousFilter = nullptr;
    QtMessageHandler _previousHandler = nullptr;
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

void GeoZoneServiceTest::_testServiceApiKeys()
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
        QTRY_VERIFY(!manager.repository()->busy());
        QCOMPARE(TestFeedServer::header(server.requests.last(), "X-Api-Key"), QByteArray("header-s3cret"));
        GeoZoneSource *const headerFeed = manager.repository()->source(headerUrl);
        QVERIFY(headerFeed);
        QCOMPARE(manager.zoneCount(), 3);
        QVERIFY(headerFeed->needsCredentials());
        QVERIFY(!headerFeed->credentialsMissing());

        // An API key in the query, which the source's URL doesn't show
        service.auth = GeoZoneService::Auth::ApiKeyQuery;
        service.authParameter = QStringLiteral("api_key");
        manager.addService(queryUrl, service.toVariantMap(), QStringLiteral("query-s3cret"));
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.repository()->busy());
        QCOMPARE(server.requestedPaths.last(), QByteArray("/query?api_key=query-s3cret"));
        QVERIFY(manager.repository()->source(queryUrl));

        // A static bearer token
        service.auth = GeoZoneService::Auth::Bearer;
        service.authParameter.clear();
        manager.addService(server.url(QStringLiteral("/bearer")), service.toVariantMap(), QStringLiteral("bearer-s3cret"));
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.repository()->busy());
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
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    GeoZoneSource *const queryFeed = restarted.repository()->source(queryUrl);
    QVERIFY(queryFeed);
    QCOMPARE(queryFeed->service().auth, GeoZoneService::Auth::ApiKeyQuery);
    server.requestedPaths.clear();
    restarted.repository()->refreshFeeds();
    QTRY_VERIFY(!restarted.repository()->busy());
    QVERIFY(server.requestedPaths.contains("/query?api_key=query-s3cret"));

    // Without its secret, a service isn't requested
    restarted.repository()->setSecret(queryUrl, QString());
    QVERIFY(queryFeed->credentialsMissing());
    restarted.repository()->refreshFeeds();
    QTRY_VERIFY(!restarted.repository()->busy());
    QCOMPARE(queryFeed->errorString(), QStringLiteral("The credentials of the service are missing."));
    // The zones of the three feeds are kept, the same zones counting once
    QCOMPARE(restarted.zoneCount() + restarted.mergedZoneCount(), 9);
    QCOMPARE(restarted.zoneCount(), 3);
    restarted.repository()->setSecret(queryUrl, QStringLiteral("new-s3cret"));
    QVERIFY(!queryFeed->credentialsMissing());
    QTRY_VERIFY(!restarted.repository()->busy());
    QVERIFY(server.requestedPaths.contains("/query?api_key=new-s3cret"));
    QVERIFY(queryFeed->errorString().isEmpty());

    // Removing the sources removes their secrets
    restarted.repository()->clear();
    QVERIFY(restarted.repository()->credentials().secret(queryUrl).isEmpty());
    QVERIFY(restarted.repository()->credentials().secret(headerUrl).isEmpty());
}

void GeoZoneServiceTest::_testClearForgetsPendingServices()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/zones", server.body);
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    const QString url = server.url(QStringLiteral("/zones"));
    GeoZoneManager manager(storageDir.path());
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);

    // A service removed with all sources while it is being added
    GeoZoneService service;
    service.auth = GeoZoneService::Auth::ApiKeyHeader;
    service.authParameter = QStringLiteral("X-Api-Key");
    manager.addService(url, service.toVariantMap(), QStringLiteral("pending-s3cret"));
    QVERIFY(manager.repository()->busy());
    QCOMPARE(manager.repository()->credentials().secret(url), QStringLiteral("pending-s3cret"));
    manager.repository()->clear();
    QVERIFY(!manager.repository()->busy());
    QCOMPARE(manager.sources()->count(), 0);

    // leaves neither its secret nor its service, which a feed added at its address later would send
    QVERIFY(manager.repository()->credentials().secret(url).isEmpty());
    server.requests.clear();
    manager.repository()->addUrl(url);
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    const GeoZoneSource *const feed = manager.repository()->source(url);
    QVERIFY(feed);
    QVERIFY(feed->service().isEmpty());
    QVERIFY(!feed->needsCredentials());
    QVERIFY(!server.requests.isEmpty());
    for (const QByteArray &request : std::as_const(server.requests)) {
        QVERIFY(TestFeedServer::header(request, "X-Api-Key").isEmpty());
    }
    manager.repository()->clear();
}

void GeoZoneServiceTest::_testSecretsOnlyOfServices()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/plain", server.body);
    server.files.insert("/keyed", server.body);
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    const QString plainUrl = server.url(QStringLiteral("/plain"));
    const QString keyedUrl = server.url(QStringLiteral("/keyed"));
    const QString unknownUrl = server.url(QStringLiteral("/unknown"));
    GeoZoneManager manager(storageDir.path());
    GeoZoneSourceRepository *const repository = manager.repository();
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    repository->addUrl(plainUrl);
    QVERIFY(addedSpy.wait(10000));
    GeoZoneService service;
    service.auth = GeoZoneService::Auth::ApiKeyHeader;
    service.authParameter = QStringLiteral("X-Api-Key");
    manager.addService(keyedUrl, service.toVariantMap(), QStringLiteral("keyed-s3cret"));
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!repository->busy());
    const GeoZoneSource *const plain = repository->source(plainUrl);
    const GeoZoneSource *const keyed = repository->source(keyedUrl);
    QVERIFY(plain && keyed);

    // A secret is only kept for a service which sends one: not for a plain feed, nor for a source which isn't there
    const qsizetype requests = server.requests.count();
    repository->setSecret(plainUrl, QStringLiteral("stray-s3cret"));
    repository->setSecret(unknownUrl, QStringLiteral("stray-s3cret"));
    QVERIFY(repository->credentials().secret(plainUrl).isEmpty());
    QVERIFY(repository->credentials().secret(unknownUrl).isEmpty());
    QVERIFY(!repository->busy());
    QCOMPARE(server.requests.count(), requests);

    // and a feed without service doesn't miss the secret it never had
    repository->setSecret(plainUrl, QString());
    QVERIFY(!plain->credentialsMissing());
    repository->refreshSource(plainUrl);
    QTRY_VERIFY(!repository->busy());
    QVERIFY2(plain->errorString().isEmpty(), qPrintable(plain->errorString()));

    // The source of a service follows what is kept for it
    repository->setSecret(keyedUrl, QString());
    QVERIFY(keyed->credentialsMissing());
    repository->setSecret(keyedUrl, QStringLiteral("new-s3cret"));
    QVERIFY(!keyed->credentialsMissing());
    QCOMPARE(repository->credentials().secret(keyedUrl), QStringLiteral("new-s3cret"));
    QTRY_VERIFY(!repository->busy());
    QCOMPARE(TestFeedServer::header(server.requests.last(), "X-Api-Key"), QByteArray("new-s3cret"));
    repository->clear();
}

void GeoZoneServiceTest::_testServiceOAuth2()
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
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(tokenRequests, 2);
    QCOMPARE(manager.zoneCount(), 3);
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(tokenRequests, 2);
    QVERIFY(manager.repository()->source(url)->errorString().isEmpty());

    // A token the server no longer accepts is replaced
    currentToken = "revoked";
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(tokenRequests, 3);
    QVERIFY(manager.repository()->source(url)->errorString().isEmpty());

    // An expired token is replaced before the download
    environment.addSecs(3600);
    const qsizetype requests = server.requests.count();
    manager.repository()->refreshFeeds();
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(tokenRequests, 4);
    QCOMPARE(server.requests.count(), requests + 2);
    QVERIFY(manager.repository()->source(url)->errorString().isEmpty());

    QVERIFY(!_directoryContent(storageDir.path()).contains("client-s3cret"));
    QVERIFY(!_directoryContent(storageDir.path()).contains("token-"));
    QVERIFY(!log.text().contains(QStringLiteral("client-s3cret")));
    QVERIFY(!log.text().contains(QStringLiteral("token-")));
    manager.repository()->clear();
}

void GeoZoneServiceTest::_testEd318Service()
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
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(manager.zoneCount(), 3);
    GeoZoneSource *const feed = manager.repository()->source(url);
    QVERIFY(feed);
    QCOMPARE(feed->name(), service.name);
    QCOMPARE(feed->formatName(), QStringLiteral("ED-318"));
    QCOMPARE(query.value("startDateTime").toString(), environment.now().toString(Qt::ISODate));
    QCOMPARE(query.value("endDateTime").toString(), environment.now().addSecs(6 * 3600).toString(Qt::ISODate));
    QVERIFY(!query.contains("geometry"));

    // The stored copy is the zones' document
    QFile stored(QDir(storageDir.path()).filePath(manager.repository()->storedFileName(url)));
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(stored.readAll()).object(), QJsonDocument::fromJson(server.body).object());

    // Zones in a member of the response, like the GeoJSON of the FAA NOTAM Management Service
    service = GeoZoneService();
    service.responseMember = QStringLiteral("data.geojson");
    manager.addService(server.url(QStringLiteral("/wrapped")), service.toVariantMap(), QString());
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    // The same zones as the other feed, counting once
    QCOMPARE(manager.zoneCount(), 3);
    QCOMPARE(manager.mergedZoneCount(), 3);
    service.responseMember = QStringLiteral("data.aixm");
    manager.addService(server.url(QStringLiteral("/wrapped")), service.toVariantMap(), QString());
    QVERIFY(failedSpy.wait(10000));
    QVERIFY2(failedSpy.last()[1].toString().contains(QStringLiteral("has no data.aixm")), qPrintable(failedSpy.last()[1].toString()));
    QCOMPARE(manager.mergedZoneCount(), 3);

    // The FAA NOTAM Management Service isn't offered while its NOTAMs can't be read
    const QList<GeoZoneProviders::ServiceTemplate> templates = GeoZoneProviders::serviceTemplates();
    QVERIFY(std::none_of(templates.cbegin(), templates.cend(), [](const GeoZoneProviders::ServiceTemplate &serviceTemplate) {
        return serviceTemplate.service.id == QStringLiteral("faa-nms");
    }));
    const auto dfs = std::find_if(templates.cbegin(), templates.cend(), [](const GeoZoneProviders::ServiceTemplate &serviceTemplate) {
        return serviceTemplate.service.id == QStringLiteral("dfs-ed318");
    });
    QVERIFY(dfs != templates.cend());
    QCOMPARE(GeoZoneService::fromVariantMap(dfs->service.toVariantMap()), dfs->service);

    // The Add Service form starts from the fields of a template, or another service last, and keeps the rest of it
    const QVariantList templateList = GeoZoneProviders::serviceTemplateList();
    QCOMPARE(templateList.count(), templates.count() + 1);
    const int dfsIndex = static_cast<int>(dfs - templates.cbegin());
    QVariantMap form = templateList[dfsIndex].toMap().value(QStringLiteral("form")).toMap();
    QCOMPARE(form.value(QStringLiteral("auth")).toInt(), static_cast<int>(GeoZoneService::Auth::Basic));
    QCOMPARE(GeoZoneProviders::serviceFromForm(dfsIndex, form), dfs->service);
    form.insert(QStringLiteral("auth"), static_cast<int>(GeoZoneService::Auth::OAuth2ClientCredentials));
    form.insert(QStringLiteral("user"), QStringLiteral(" pilot "));
    form.insert(QStringLiteral("method"), static_cast<int>(GeoZoneService::Method::Get));
    const GeoZoneService edited = GeoZoneProviders::serviceFromForm(dfsIndex, form);
    QCOMPARE(edited.auth, GeoZoneService::Auth::OAuth2ClientCredentials);
    QCOMPARE(edited.user, QStringLiteral("pilot"));
    QCOMPARE(edited.method, GeoZoneService::Method::Get);
    QCOMPARE(edited.maxAreaKm2, dfs->service.maxAreaKm2);
    QCOMPARE(edited.id, dfs->service.id);
    const GeoZoneService other = GeoZoneProviders::serviceFromForm(static_cast<int>(templates.count()), form);
    QVERIFY((other.maxAreaKm2 == 0) && other.id.isEmpty());
    QCOMPARE(other.auth, GeoZoneService::Auth::OAuth2ClientCredentials);
    manager.repository()->clear();
}

void GeoZoneServiceTest::_testServiceAreaOfInterest()
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
        repository->setFollowArea(true);
        QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
        QSignalSpy failedSpy(&manager, &GeoZoneManager::loadFailed);

        // Without a mission or a vehicle, there is no area to download
        manager.addService(url, GeoZoneService().toVariantMap(), QString());
        QTRY_COMPARE(failedSpy.count(), 1);
        QVERIFY2(failedSpy.last()[1].toString().contains(QStringLiteral("Plan a mission, connect a vehicle or show the area on the map")), qPrintable(failedSpy.last()[1].toString()));
        QCOMPARE(manager.sources()->count(), 0);

        // The zones around the vehicle, the URL template filled with the area
        repository->setVehiclePosition(QGeoCoordinate(47.0, 8.0));
        manager.addService(url, GeoZoneService().toVariantMap(), QString());
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.repository()->busy());
        GeoZoneSource *const feed = manager.repository()->source(url);
        QVERIFY(feed);
        QVERIFY(feed->followsArea());
        const QGeoRectangle area = GeoZoneAreaOfInterest::expanded(QGeoRectangle(QGeoCoordinate(47.0, 8.0), QGeoCoordinate(47.0, 8.0)), GeoZoneAreaOfInterest::margin);
        QCOMPARE(feed->area(), area);
        QVERIFY(std::abs(area.height() - (10000.0 / 111320)) < 1e-6);
        QCOMPARE(requestedUrl(), GeoZoneService::resolve(url, area, QDateTime(), QDateTime()));
        QVERIFY2(requestedUrl().contains(QStringLiteral("&lat=47.00000&radius=7.1")), qPrintable(requestedUrl()));

        // Moving within the area downloads nothing, leaving it downloads the new area
        const int requests = server.requestCount;
        repository->setVehiclePosition(QGeoCoordinate(47.005, 8.005));
        QVERIFY(!manager.repository()->busy());
        repository->setVehiclePosition(QGeoCoordinate(47.1, 8.0));
        QVERIFY(manager.repository()->busy());
        QTRY_VERIFY(!manager.repository()->busy());
        QCOMPARE(server.requestCount, requests + 1);
        QVERIFY(feed->area().contains(QGeoCoordinate(47.1, 8.0)));
        QVERIFY(!feed->area().contains(QGeoCoordinate(47.0, 8.0)));

        // The area of interest covers the missions and the vehicle
        const QObject mission;
        const QGeoRectangle missionArea(QGeoCoordinate(47.3, 7.9), QGeoCoordinate(47.2, 8.1));
        manager.repository()->setMissionArea(&mission, missionArea);
        QTRY_VERIFY(!manager.repository()->busy());
        QCOMPARE(repository->areaOfInterest(), QGeoRectangle(QGeoCoordinate(47.3, 7.9), QGeoCoordinate(47.1, 8.1)));
        QVERIFY(feed->area().contains(repository->areaOfInterest()));
        QCOMPARE(server.requestCount, requests + 2);

        // Feeds keep their area while they don't follow it, also when refreshed
        repository->setFollowArea(false);
        const QGeoRectangle kept = feed->area();
        repository->setVehiclePosition(QGeoCoordinate(48.0, 9.0));
        QVERIFY(!manager.repository()->busy());
        manager.repository()->refreshFeeds();
        QTRY_VERIFY(!manager.repository()->busy());
        QCOMPARE(feed->area(), kept);
        repository->setFollowArea(true);
        QTRY_VERIFY(!manager.repository()->busy());
        QVERIFY(feed->area().contains(QGeoCoordinate(48.0, 9.0)));
        manager.repository()->setMissionArea(&mission, QGeoRectangle());

        // ED-318 services get the area as a polygon
        GeoZoneService ed318;
        ed318.method = GeoZoneService::Method::Ed318Post;
        ed318.areaOfInterest = true;
        manager.addService(server.url(QStringLiteral("/uas-zones")), ed318.toVariantMap(), QString());
        QVERIFY(addedSpy.wait(10000));
        QTRY_VERIFY(!manager.repository()->busy());
        const QGeoRectangle ed318Area = manager.repository()->source(server.url(QStringLiteral("/uas-zones")))->area();
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
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    GeoZoneSource *const feed = restarted.repository()->source(url);
    QVERIFY(feed);
    const QGeoRectangle stored = feed->area();
    QVERIFY(stored.contains(QGeoCoordinate(48.0, 9.0)));
    restarted.repository()->refreshFeeds();
    QTRY_VERIFY(!restarted.repository()->busy());
    QVERIFY(feed->errorString().isEmpty());
    const QString storedUrl = GeoZoneService::resolve(url, stored, QDateTime(), QDateTime());
    QVERIFY(std::any_of(server.requestedPaths.cbegin(), server.requestedPaths.cend(), [&](const QByteArray &path) { return server.url(QString::fromUtf8(path)) == storedUrl; }));
    QCOMPARE(feed->area(), stored);

    // Services answering areas up to a size get a smaller margin, larger areas aren't requested
    const QTemporaryDir limitedDir;
    GeoZoneManager limitedManager(limitedDir.path());
    limitedManager.repository()->setVehiclePosition(QGeoCoordinate(47.5, 8.5));
    QSignalSpy limitedSpy(&limitedManager, &GeoZoneManager::sourceAdded);
    GeoZoneService limited;
    limited.areaOfInterest = true;
    limited.maxAreaKm2 = 10;
    const QString limitedUrl = server.url(QStringLiteral("/zones?limited={bbox}"));
    limitedManager.addService(limitedUrl, limited.toVariantMap(), QString());
    QVERIFY(limitedSpy.wait(10000));
    QTRY_VERIFY(!limitedManager.repository()->busy());
    const GeoZoneSource *const limitedFeed = limitedManager.repository()->source(limitedUrl);
    QVERIFY(limitedFeed->area().contains(QGeoCoordinate(47.5, 8.5)));
    QVERIFY(GeoZoneService::areaKm2(limitedFeed->area()) <= 9.5);
    QVERIFY(GeoZoneService::areaKm2(limitedFeed->area()) > 9);
    const QObject largeMission;
    limitedManager.repository()->setMissionArea(&largeMission, QGeoRectangle(QGeoCoordinate(47.6, 8.4), QGeoCoordinate(47.4, 8.6)));
    QTRY_VERIFY(!limitedManager.repository()->busy());
    QVERIFY2(limitedFeed->errorString().contains(QStringLiteral("is larger than the 10 km² the service answers")), qPrintable(limitedFeed->errorString()));
    QCOMPARE(limitedFeed->zoneCount(), 3);
    limitedManager.repository()->setMissionArea(&largeMission, QGeoRectangle());

    // Across the antimeridian
    const QGeoRectangle crossing = GeoZoneAreaOfInterest::expanded(QGeoRectangle(QGeoCoordinate(-17.0, 179.99), QGeoCoordinate(-17.0, 179.99)), 5000);
    QVERIFY(crossing.topLeft().longitude() > 179.9);
    QVERIFY(crossing.bottomRight().longitude() < -179.9);
    QVERIFY(crossing.contains(QGeoCoordinate(-17.0, -179.99)));
}

void GeoZoneServiceTest::_testMapViewAreaOfInterest()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/zones", server.body);
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    const QString url = server.url(QStringLiteral("/zones?bbox={bbox}"));
    GeoZoneManager manager(storageDir.path());
    GeoZoneSourceRepository *const repository = manager.repository();
    repository->setFollowArea(true);
    repository->setFollowView(true);
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);

    // The view is taken once the map stops moving, a service is then added for it alone
    const QGeoRectangle zurich(QGeoCoordinate(47.5, 8.4), QGeoCoordinate(47.3, 8.7));
    manager.setMapView(QGeoRectangle(QGeoCoordinate(46.5, 7.4), QGeoCoordinate(46.3, 7.7)));
    manager.setMapView(zurich);
    QVERIFY(!repository->areaOfInterest().isValid());
    QTRY_COMPARE(repository->areaOfInterest(), zurich);
    manager.addService(url, GeoZoneService().toVariantMap(), QString());
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!repository->busy());
    GeoZoneSource *const feed = repository->source(url);
    QVERIFY(feed);
    QCOMPARE(feed->area(), GeoZoneAreaOfInterest::expanded(zurich, GeoZoneAreaOfInterest::margin));
    const int requests = server.requestCount;

    // Panning and zooming in within the area downloaded requests nothing
    manager.setMapView(QGeoRectangle(QGeoCoordinate(47.52, 8.38), QGeoCoordinate(47.32, 8.68)));
    manager.setMapView(QGeoRectangle(QGeoCoordinate(47.45, 8.5), QGeoCoordinate(47.4, 8.6)));
    QTest::qWait(GeoZoneManager::mapViewDelayMSecs + 500);
    QVERIFY(!repository->busy());
    QCOMPARE(server.requestCount, requests);

    // A pan to a new area is one request, however many moves it took
    for (int step = 1; step <= 5; step++) {
        manager.setMapView(QGeoRectangle(QGeoCoordinate(47.5, 8.4 + (0.2 * step)), QGeoCoordinate(47.3, 8.7 + (0.2 * step))));
    }
    QTRY_COMPARE(server.requestCount, requests + 1);
    QTRY_VERIFY(!repository->busy());
    QTest::qWait(GeoZoneManager::mapViewDelayMSecs + 500);
    QCOMPARE(server.requestCount, requests + 1);
    QVERIFY(feed->area().contains(QGeoRectangle(QGeoCoordinate(47.5, 9.4), QGeoCoordinate(47.3, 9.7))));
    QVERIFY(!feed->area().contains(QGeoCoordinate(47.4, 8.5)));

    // Zoomed out views count with their central 200 km
    const QGeoRectangle europe(QGeoCoordinate(55, 0), QGeoCoordinate(40, 20));
    manager.setMapView(europe);
    QTRY_COMPARE(server.requestCount, requests + 2);
    QTRY_VERIFY(!repository->busy());
    QVERIFY(GeoZoneAreaOfInterest::spanMeters(repository->areaOfInterest()) <= GeoZoneAreaOfInterest::maxViewMeters + 1);
    QVERIFY(repository->areaOfInterest().contains(europe.center()));
    QVERIFY(feed->area().contains(repository->areaOfInterest()));

    // The view joins the vehicle when they fit together, a view elsewhere keeps the zones around the vehicle
    manager.setMapView(zurich);
    QTRY_COMPARE(server.requestCount, requests + 3);
    QTRY_VERIFY(!repository->busy());
    repository->setVehiclePosition(QGeoCoordinate(47.4, 8.9));
    QCOMPARE(repository->areaOfInterest(), QGeoRectangle(QGeoCoordinate(47.5, 8.4), QGeoCoordinate(47.3, 8.9)));
    QTRY_COMPARE(server.requestCount, requests + 4);
    QTRY_VERIFY(!repository->busy());
    manager.setMapView(QGeoRectangle(QGeoCoordinate(39.0, -77.2), QGeoCoordinate(38.8, -76.9)));
    QTRY_COMPARE(repository->areaOfInterest(), QGeoRectangle(QGeoCoordinate(47.4, 8.9), QGeoCoordinate(47.4, 8.9)));
    QTest::qWait(500);
    QVERIFY(!repository->busy());
    QCOMPARE(server.requestCount, requests + 4);

    // Without the vehicle the view is downloaded, unless the setting leaves the views out
    repository->setFollowView(false);
    repository->setVehiclePosition(QGeoCoordinate());
    QVERIFY(!repository->areaOfInterest().isValid());
    QTest::qWait(500);
    QCOMPARE(server.requestCount, requests + 4);
    repository->setFollowView(true);
    QTRY_COMPARE(server.requestCount, requests + 5);
    QTRY_VERIFY(!repository->busy());
    QVERIFY(feed->area().contains(QGeoCoordinate(38.9, -77.0)));

    // Services answering smaller areas get the center of the view
    GeoZoneAreaOfInterest area;
    (void) area.setViewArea(zurich);
    const QGeoRectangle limited = area.interest(10);
    QVERIFY(GeoZoneService::areaKm2(limited) <= 9.5 + 1e-6);
    QVERIFY(limited.contains(zurich.center()));
    (void) area.setVehiclePosition(QGeoCoordinate(47.4, 8.5));
    QCOMPARE(area.interest(10), QGeoRectangle(QGeoCoordinate(47.4, 8.5), QGeoCoordinate(47.4, 8.5)));
    QCOMPARE(area.interest(), zurich);
}

void GeoZoneServiceTest::_testCredentials()
{
    // The secrets are in a group of the settings, by the hash of the source key
    const QString group = QStringLiteral("GeoZoneCredentialsTest");
    const auto restore = qScopeGuard([&group]() { QSettings().remove(group); });
    const QString key = QStringLiteral("https://example.org/zones");
    const QString id = QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex());
    const auto stored = [&group]() {
        QSettings settings;
        settings.beginGroup(group);
        QVariantMap secrets;
        for (const QString &settingKey : settings.childKeys()) {
            secrets.insert(settingKey, settings.value(settingKey));
        }
        return secrets;
    };
    GeoZoneCredentials credentials(group);
    credentials.setSecret(key, QStringLiteral("s3cret"));
    QCOMPARE(stored(), (QVariantMap{ { id, QStringLiteral("s3cret") } }));
    QCOMPARE(GeoZoneCredentials(group).secret(key), QStringLiteral("s3cret"));
    credentials.setSecret(key, QStringLiteral("new-s3cret"));
    QCOMPARE(credentials.secret(key), QStringLiteral("new-s3cret"));

    // Removing a secret removes it from the settings
    credentials.remove(key);
    QVERIFY(stored().isEmpty());
    QVERIFY(credentials.secret(key).isEmpty());
}

void GeoZoneServiceTest::_testOpenAipService()
{
    // The OpenAIP API: an API key header, the airspaces as the items of numbered pages
    QFile fixture(QStringLiteral(":/unittest/openaip_test.json"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    QJsonArray items;
    for (const QJsonValue &feature : QJsonDocument::fromJson(fixture.readAll()).object().value(QStringLiteral("features")).toArray()) {
        QJsonObject item = feature.toObject().value(QStringLiteral("properties")).toObject();
        item.insert(QStringLiteral("geometry"), feature.toObject().value(QStringLiteral("geometry")));
        items.append(item);
    }
    constexpr int pageSize = 4;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.handler = [&](const QByteArray &request, QByteArray &status, QByteArray &content, QByteArray &) {
        if (TestFeedServer::header(request, "x-openaip-api-key") != "open-s3cret") {
            status = "401 Unauthorized";
            return true;
        }
        const QUrlQuery query(QUrl(QString::fromLatin1(request.split(' ').value(1))));
        const int page = query.queryItemValue(QStringLiteral("page")).toInt();
        QJsonArray pageItems;
        for (int i = (page - 1) * pageSize; (i < page * pageSize) && (i < items.count()); i++) {
            pageItems.append(items[i]);
        }
        content = QJsonDocument(QJsonObject{ { "limit", pageSize }, { "totalCount", items.count() }, { "totalPages", 2 }, { "page", page }, { "items", pageItems } })
                      .toJson(QJsonDocument::Compact);
        return true;
    };

    const QList<GeoZoneProviders::ServiceTemplate> templates = GeoZoneProviders::serviceTemplates();
    const auto openAip = std::find_if(templates.cbegin(), templates.cend(), [](const GeoZoneProviders::ServiceTemplate &serviceTemplate) {
        return serviceTemplate.service.id == QStringLiteral("openaip");
    });
    QVERIFY(openAip != templates.cend());
    const QString url = server.url(QStringLiteral("/api/airspaces?bbox={bbox}&limit=4&page=1"));
    const QTemporaryDir storageDir;
    QVERIFY(storageDir.isValid());
    GeoZoneManager manager(storageDir.path());
    manager.repository()->setVehiclePosition(QGeoCoordinate(47.01, 8.1));
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    manager.addService(url, openAip->service.toVariantMap(), QStringLiteral("open-s3cret"));
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    QCOMPARE(server.requestCount, 2);
    QVERIFY(server.requestedPaths[1].endsWith("&limit=4&page=2"));
    QCOMPARE(manager.zoneCount(), 6);
    const GeoZoneSource *const source = manager.repository()->source(url);
    QCOMPARE(source->format(), GeoZoneParser::Format::OpenAip);
    QCOMPARE(source->name(), QStringLiteral("Airspaces (OpenAIP, unofficial)"));
    QCOMPARE(source->provider(), QStringLiteral("OpenAIP (unofficial, CC BY-NC 4.0)"));
}

GEOZONE_TEST(GeoZoneServiceTest)
