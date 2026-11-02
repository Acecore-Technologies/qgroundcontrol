/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "GeoZone.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneBriefing.h"
#include "GeoZoneCatalog.h"
#include "GeoZoneDownloader.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneFeedFetch.h"
#include "GeoZoneFormat.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneHash.h"
#include "GeoZoneIndex.h"
#include "GeoZoneInfo.h"
#include "GeoZoneLoader.h"
#include "GeoZonePack.h"
#include "GeoZoneParseCache.h"
#include "GeoZoneParser.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZonePolicy.h"
#include "GeoZoneProviders.h"
#include "GeoZoneRefreshScheduler.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "GeoZoneSourceRepository.h"
#include "GeoZoneSourceVersion.h"
#include "GeoZoneStore.h"
#include "GeoZoneTerrain.h"
#include "GeoZoneZip.h"
#ifndef GEOZONE_CORE_TEST
#include "GeoFenceController.h"
#include "GeoZoneAlertHistory.h"
#include "GeoZoneDisplay.h"
#include "GeoZoneFenceImporter.h"
#include "GeoZoneGuidedCheck.h"
#include "GeoZoneManager.h"
#include "GeoZoneMapLayer.h"
#include "GeoZoneMapModel.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMonitor.h"
#include "GeoZonePlanData.h"
#include "GeoZonePlanning.h"
#include "GeoZoneProfile.h"
#include "GeoZoneRoutePlanner.h"
#include "GeoZoneSettings.h"
#include "MissionController.h"
#include "MissionSettingsItem.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QGCMapPolygon.h"
#include "QGCZip.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "SimpleMissionItem.h"
#include "SurveyComplexItem.h"
#include "UnitsSettings.h"
#include "Vehicle.h"
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#endif
#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QMutex>
#include <QtCore/QRandomGenerator>
#include <QtCore/QScopeGuard>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThreadPool>
#include <QtCore/QTimeZone>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>

/// Fixtures and helpers shared by the geozone tests
namespace GeoZoneTestHelpers {

/// Owns the zones a test creates, as sources do
struct ZoneOwner {
    QList<QSharedPointer<GeoZone>> zones;

    GeoZone *add(const GeoZone::Data &data);
};

/// Replaces the clock, the terrain and the QNH of GeoZoneEnvironment while in scope, so that tests don't depend on when
/// and where they run. The clock stands still at the second it was created, or the time it is set to, until the test
/// advances it. The terrain is unknown until set, the QNH standard. The previous hooks are restored when the workers
/// are done.
class ScopedEnvironment
{
public:
    using Terrain = std::function<double(const QGeoCoordinate &coordinate)>;

    ScopedEnvironment();
    ~ScopedEnvironment();

    Q_DISABLE_COPY_MOVE(ScopedEnvironment)

    QDateTime now() const { return QDateTime::fromMSecsSinceEpoch(_state->msecs, QTimeZone::UTC); }
    void setNow(const QDateTime &now) { _state->msecs = now.toMSecsSinceEpoch(); }
    void addSecs(qint64 secs) { _state->msecs += secs * 1000; }

    /// @param terrain Ground elevation in meters AMSL, NaN where unknown
    void setTerrain(const Terrain &terrain);
    void setTerrain(double elevation) { setTerrain([elevation](const QGeoCoordinate &) { return elevation; }); }
    void setQnh(double qnhHPa) { _state->qnhHPa = qnhHPa; }

private:
    struct State {
        std::atomic<qint64> msecs = 0;
        std::atomic<double> qnhHPa = GeoZoneEnvironment::kStandardPressureHPa;
        QMutex mutex;
        Terrain terrain;
    };

    const GeoZoneEnvironment::Hooks _saved;
    const std::shared_ptr<State> _state = std::make_shared<State>();
};

/// Waits for the evaluations running on the global thread pool, then delivers the results they posted, so that a test
/// can check that outdated results are dropped without waiting for a fixed time
/// @return false when they didn't finish within the timeout
bool _settleWorkers(int timeoutMSecs = 10000);

GeoZoneParser::Result _parseResource(const QString &name);
inline QDateTime _utc(const QDate &date, const QTime &time) { return QDateTime(date, time, QTimeZone::UTC); }
QJsonObject _circleFeature(const QString &identifier, const QString &restriction, const QGeoCoordinate &center, double radius);
QJsonObject _polygonFeature(const QString &identifier, const QString &restriction, const QList<QGeoCoordinate> &ring);

/// Writes an ED-269 file with the features to the directory
QString _writeFeatures(const QTemporaryDir &dir, const QString &name, const QJsonArray &features);

QList<QGeoCoordinate> _circleRing(const QGeoCoordinate &center, double radius, int vertexCount);

/// Prohibited zones on a grid of columns by rows cells of the pitch in meters, north-east of the corner: circles and
/// irregular polygons of random sizes and positions, so that about half the gaps between them are narrower than twice
/// the default margin of the route search, a maze
QJsonArray _mazeFeatures(const QGeoCoordinate &southWest, int columns, int rows, double pitch, quint32 seed);

double _distanceToRing(const QPointF &point, const QList<QPointF> &ring);

/// @return The ED-269 feature with limits in meters of its first layer
QJsonObject _withLimits(QJsonObject feature, double lower, const QString &lowerReference, double upper, const QString &upperReference);

/// @return Path through the coordinates at the altitudes AMSL, unknown where not given
GeoZonePathEvaluator::Path _evaluatorPath(const QList<QGeoCoordinate> &coordinates, const QList<double> &altitudes = {}, const QString &label = QString());

/// @return Smallest horizontal distance from the path, sampled every few meters, to the zones
double _pathClearance(const QList<QGeoCoordinate> &path, const QList<const GeoZone*> &zones, double step = 5);

/// @return true if the value is within the tolerance of the expected one, which is logged otherwise
bool _near(double value, double expected, double tolerance = 1);

/// Shows heights in feet or in meters until the returned guard restores the previous units: those of the
/// QGroundControl settings, or of GeoZoneEnvironment without the application
QScopeGuard<std::function<void()>> _setVerticalUnits(bool feet);

/// Loads files into the sources of a repository and indexes the enabled ones in a catalog, like GeoZoneManager, with
/// the GeoZoneCore library only
class ZoneCatalog
{
public:
    /// @param storageDir Directory in which the repository keeps the copies of the sources
    explicit ZoneCatalog(const QString &storageDir) : _repository(storageDir) {}

    /// Loads the file as a source, or reloads it, see GeoZoneSourceRepository::loadFile
    bool loadFile(const QString &fileName, QString &errorString);

    const GeoZoneCatalog &catalog() const { return _catalog; }
    const QList<GeoZone*> &zoneList() const { return _catalog.zones(); }
    const GeoZoneSourceRepository *repository() const { return &_repository; }

private:
    GeoZoneSourceRepository _repository;
    GeoZoneCatalog _catalog;
};

/// The mission of the briefing tests and its zones: ten minutes 4 km east of 47 N 8 E at 100 m from 2027-01-01 10:30
/// UTC, entering a control zone (CTR-1) with its authority, message, schedule, limits, a name with markup and an
/// authorisation, then a prison (P-1). A return from halfway enters RET, a restricted zone is near the path (NEAR) and
/// one far from it (FAR), a zone without restriction is near it too (INFO).
class BriefingMission
{
public:
    explicit BriefingMission(const QTemporaryDir &dir)
        : zones(dir.filePath(QStringLiteral("storage")))
        , _dir(dir)
    {
    }

    /// Loads the zones and authorises the control zone
    bool load(QString &errorString);
    GeoZoneBriefing build() const;

    const QGeoCoordinate center{ 47, 8 };
    const QDateTime start = _utc(QDate(2027, 1, 1), QTime(10, 30));
    ZoneCatalog zones;
    GeoZoneAuthorisations authorisations{ QString() };

private:
    const QTemporaryDir &_dir;
};

#ifndef GEOZONE_CORE_TEST
/// Shows horizontal and vertical distances in meters while in scope, restoring the units of the settings after, so that
/// the texts tests compare don't depend on the locale, whose measurement system sets the default units, like feet for
/// en_US. Create it once the application runs, not in the constructor of a test.
class ScopedMetricUnits
{
public:
    ScopedMetricUnits();
    ~ScopedMetricUnits();

    Q_DISABLE_COPY_MOVE(ScopedMetricUnits)

private:
    void _set(Fact *fact, const QVariant &value);

    QList<std::pair<Fact*, QVariant>> _saved;
};

/// Answers requests by path: /ok with the ED-318 test file, /missing 404, /broken 500, /empty an empty 200 and
/// /hang never. Counts the requests and keeps them with their headers and bodies, and counts those answered and those
/// the client abandoned by closing the connection first, like aborted downloads.
class TestFeedServer : public QTcpServer
{
public:
    /// Answers a request instead of the defaults when it returns true
    using Handler = std::function<bool(const QByteArray &request, QByteArray &status, QByteArray &content, QByteArray &headers)>;

    TestFeedServer();

    QString url(const QString &path) const { return QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path); }

    /// @return Value of the header of a request, empty without
    static QByteArray header(const QByteArray &request, const QByteArray &name);
    static QByteArray requestBody(const QByteArray &request) { return request.mid(request.indexOf("\r\n\r\n") + 4); }

    /// @return true when each request was answered or abandoned, so that no response is still to come, like those
    ///         delayed for aborted downloads
    bool settled() const { return (answeredCount + abandonedCount) >= requestCount; }

    QByteArray body;
    int requestCount = 0;
    int answeredCount = 0;
    int abandonedCount = 0;                 ///< Requests whose connection the client closed before the answer
    QByteArray okPathStatus = "200 OK";     ///< Status of /ok, to make a feed fail later
    QHash<QByteArray, QByteArray> files;    ///< Content served by path, with or without the query
    QHash<QByteArray, QByteArray> headers;  ///< Further response headers by path, each ending with CRLF
    QHash<QByteArray, QByteArray> etags;    ///< ETag of the content by path, conditional requests matching it get 304
    QHash<QByteArray, int> delays;          ///< Milliseconds before answering, by path
    QHash<QByteArray, QByteArray> redirects;    ///< Location a path redirects to with 302 Found
    Handler handler;
    QList<QByteArray> requestedPaths;       ///< With the query
    QList<QByteArray> requests;             ///< Request line, headers and body

private:
    void _respond(QTcpSocket *socket, const QByteArray &target, const QByteArray &request);
    void _write(QTcpSocket *socket, const QByteArray &status, const QByteArray &content, const QByteArray &extraHeaders);
    void _answer(QTcpSocket *socket, const QByteArray &response);
};
#endif

} // namespace GeoZoneTestHelpers
