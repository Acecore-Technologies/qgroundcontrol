/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneTestHelpers.h"

namespace GeoZoneTestHelpers {

GeoZone *ZoneOwner::add(const GeoZone::Data &data)
{
    zones.append(QSharedPointer<GeoZone>::create(data));
    return zones.last().data();
}

ScopedEnvironment::ScopedEnvironment()
    : _saved(GeoZoneEnvironment::hooks())
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    _state->msecs = now - (now % 1000);
    GeoZoneEnvironment::Hooks hooks = _saved;
    hooks.currentDateTimeUtc = [state = _state]() { return QDateTime::fromMSecsSinceEpoch(state->msecs, QTimeZone::UTC); };
    hooks.terrainElevation = [state = _state](const QGeoCoordinate &coordinate) {
        const QMutexLocker locker(&state->mutex);
        return state->terrain ? state->terrain(coordinate) : qQNaN();
    };
    hooks.qnhHPa = [state = _state]() { return state->qnhHPa.load(); };
    GeoZoneEnvironment::setHooks(hooks);
}

ScopedEnvironment::~ScopedEnvironment()
{
    // Evaluations on worker threads may still use the hooks
    (void) QThreadPool::globalInstance()->waitForDone(10000);
    GeoZoneEnvironment::setHooks(_saved);
}

void ScopedEnvironment::setTerrain(const Terrain &terrain)
{
    const QMutexLocker locker(&_state->mutex);
    _state->terrain = terrain;
}

bool _settleWorkers(int timeoutMSecs)
{
    if (!QThreadPool::globalInstance()->waitForDone(timeoutMSecs)) {
        return false;
    }
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    return true;
}

GeoZoneParser::Result _parseResource(const QString &name)
{
    GeoZoneParser::Result result;
    QString errorString;
    if (!GeoZoneParser::parseFile(QStringLiteral(":/unittest/%1").arg(name), result, errorString)) {
        qWarning() << "Parse failed" << name << errorString;
    }
    return result;
}

QJsonObject _circleFeature(const QString &identifier, const QString &restriction, const QGeoCoordinate &center, double radius)
{
    return QJsonObject{
        { "identifier", identifier },
        { "restriction", restriction },
        { "geometry", QJsonArray{ QJsonObject{ { "horizontalProjection", QJsonObject{
            { "type", "Circle" }, { "center", QJsonArray{ center.longitude(), center.latitude() } }, { "radius", radius } } } } } },
    };
}

QJsonObject _polygonFeature(const QString &identifier, const QString &restriction, const QList<QGeoCoordinate> &ring)
{
    QJsonArray positions;
    for (const QGeoCoordinate &coordinate : ring) {
        positions.append(QJsonArray{ coordinate.longitude(), coordinate.latitude() });
    }
    positions.append(positions.first());
    return QJsonObject{
        { "identifier", identifier },
        { "restriction", restriction },
        { "geometry", QJsonArray{ QJsonObject{ { "horizontalProjection", QJsonObject{
            { "type", "Polygon" }, { "coordinates", QJsonArray{ positions } } } } } } },
    };
}

QString _writeFeatures(const QTemporaryDir &dir, const QString &name, const QJsonArray &features)
{
    const QString fileName = dir.filePath(name);
    QFile file(fileName);
    if (file.open(QIODevice::WriteOnly)) {
        (void) file.write(QJsonDocument(QJsonObject{ { "features", features } }).toJson(QJsonDocument::Compact));
    }
    return fileName;
}

QList<QGeoCoordinate> _circleRing(const QGeoCoordinate &center, double radius, int vertexCount)
{
    QList<QGeoCoordinate> ring;
    for (int i = 0; i < vertexCount; i++) {
        ring.append(center.atDistanceAndAzimuth(radius, i * 360.0 / vertexCount));
    }
    return ring;
}

QJsonArray _mazeFeatures(const QGeoCoordinate &southWest, int columns, int rows, double pitch, quint32 seed)
{
    QRandomGenerator random(seed);
    const GeoZoneGeometry::LocalProjection projection(southWest);
    QJsonArray features;
    for (int row = 0; row < rows; row++) {
        for (int column = 0; column < columns; column++) {
            const QPointF offset((random.generateDouble() - 0.5) * 0.2 * pitch, (random.generateDouble() - 0.5) * 0.2 * pitch);
            const QGeoCoordinate center = projection.toGeo((QPointF(column + 0.5, row + 0.5) * pitch) + offset);
            const double radius = pitch * (0.18 + (0.2 * random.generateDouble()));
            const QString identifier = QStringLiteral("Maze %1-%2").arg(column).arg(row);
            if (random.bounded(2) == 0) {
                features.append(_circleFeature(identifier, QStringLiteral("PROHIBITED"), center, radius));
                continue;
            }
            QList<QGeoCoordinate> ring;
            const int vertexCount = 5 + random.bounded(8);
            for (int vertex = 0; vertex < vertexCount; vertex++) {
                ring.append(center.atDistanceAndAzimuth(radius * (0.6 + (0.4 * random.generateDouble())), vertex * 360.0 / vertexCount));
            }
            features.append(_polygonFeature(identifier, QStringLiteral("PROHIBITED"), ring));
        }
    }
    return features;
}

double _distanceToRing(const QPointF &point, const QList<QPointF> &ring)
{
    double minDistance = std::numeric_limits<double>::max();
    for (qsizetype i = 0; i < ring.count(); i++) {
        const QPointF a = ring[i];
        const QPointF b = ring[(i + 1) % ring.count()];
        const QPointF ab = b - a;
        const double lengthSquared = QPointF::dotProduct(ab, ab);
        const double t = (lengthSquared > 0) ? std::clamp(QPointF::dotProduct(point - a, ab) / lengthSquared, 0.0, 1.0) : 0.0;
        const QPointF offset = a + (t * ab) - point;
        minDistance = qMin(minDistance, std::hypot(offset.x(), offset.y()));
    }
    return minDistance;
}

QJsonObject _withLimits(QJsonObject feature, double lower, const QString &lowerReference, double upper, const QString &upperReference)
{
    QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    layer[QStringLiteral("lowerLimit")] = lower;
    layer[QStringLiteral("lowerVerticalReference")] = lowerReference;
    layer[QStringLiteral("upperLimit")] = upper;
    layer[QStringLiteral("upperVerticalReference")] = upperReference;
    geometry[0] = layer;
    feature[QStringLiteral("geometry")] = geometry;
    return feature;
}

GeoZonePathEvaluator::Path _evaluatorPath(const QList<QGeoCoordinate> &coordinates, const QList<double> &altitudes, const QString &label)
{
    GeoZonePathEvaluator::Path path;
    path.label = label;
    for (qsizetype i = 0; i < coordinates.count(); i++) {
        path.vertices.append(GeoZonePathEvaluator::Vertex{ coordinates[i], altitudes.value(i, qQNaN()), qQNaN(), QDateTime() });
    }
    return path;
}

double _pathClearance(const QList<QGeoCoordinate> &path, const QList<const GeoZone*> &zones, double step)
{
    double clearance = std::numeric_limits<double>::infinity();
    for (qsizetype i = 1; i < path.count(); i++) {
        const double length = path[i - 1].distanceTo(path[i]);
        const double azimuth = path[i - 1].azimuthTo(path[i]);
        for (double along = 0; along <= length; along += step) {
            const QGeoCoordinate point = path[i - 1].atDistanceAndAzimuth(along, azimuth);
            for (const GeoZone *zone : zones) {
                clearance = (std::min)(clearance, zone->distanceTo(point));
            }
        }
    }
    return clearance;
}

bool _near(double value, double expected, double tolerance)
{
    if (qAbs(value - expected) <= tolerance) {
        return true;
    }
    qWarning() << "Expected" << expected << "got" << value;
    return false;
}

QScopeGuard<std::function<void()>> _setVerticalUnits(bool feet)
{
#ifdef GEOZONE_CORE_TEST
    const GeoZoneEnvironment::Hooks saved = GeoZoneEnvironment::hooks();
    GeoZoneEnvironment::Hooks hooks = saved;
    hooks.verticalDistanceUnits = [feet]() { return feet ? QStringLiteral("ft") : QStringLiteral("m"); };
    hooks.metersToVerticalDistanceUnits = [feet](double meters) { return feet ? (meters / 0.3048) : meters; };
    GeoZoneEnvironment::setHooks(hooks);
    return QScopeGuard<std::function<void()>>([saved]() { GeoZoneEnvironment::setHooks(saved); });
#else
    Fact *const verticalUnits = SettingsManager::instance()->unitsSettings()->verticalDistanceUnits();
    const QVariant saved = verticalUnits->rawValue();
    verticalUnits->setRawValue(feet ? UnitsSettings::VerticalDistanceUnitsFeet : UnitsSettings::VerticalDistanceUnitsMeters);
    return QScopeGuard<std::function<void()>>([verticalUnits, saved]() { verticalUnits->setRawValue(saved); });
#endif
}

bool ZoneCatalog::loadFile(const QString &fileName, QString &errorString)
{
    const bool loaded = _repository.loadFile(fileName, errorString);
    QList<const GeoZoneSource*> sources = _repository.sourceList();
    (void) sources.removeIf([](const GeoZoneSource *source) { return !source->enabled(); });
    _catalog.rebuild(sources);
    return loaded;
}

bool BriefingMission::load(QString &errorString)
{
    QJsonObject controlZone = _circleFeature(QStringLiteral("CTR-1"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1000, 90), 200);
    controlZone[QStringLiteral("name")] = QStringLiteral("CTR <script>alert(1)</script>");
    controlZone[QStringLiteral("country")] = QStringLiteral("CHE");
    controlZone[QStringLiteral("message")] = QStringLiteral("Call the tower");
    controlZone[QStringLiteral("zoneAuthority")] = QJsonArray{ QJsonObject{ { "name", "Skyguide" }, { "email", "ops@example.org" }, { "phone", "+41 43 931 61 11" } } };
    controlZone[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-01-01T10:45:00Z" }, { "endDateTime", "2027-01-01T12:00:00Z" } } };
    QJsonObject geometry = controlZone[QStringLiteral("geometry")].toArray()[0].toObject();
    geometry[QStringLiteral("uomDimensions")] = QStringLiteral("M");
    geometry[QStringLiteral("lowerLimit")] = 0;
    geometry[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AGL");
    geometry[QStringLiteral("upperLimit")] = 500;
    geometry[QStringLiteral("upperVerticalReference")] = QStringLiteral("AMSL");
    controlZone[QStringLiteral("geometry")] = QJsonArray{ geometry };
    QJsonObject prison = _circleFeature(QStringLiteral("P-1"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2500, 90), 200);
    prison[QStringLiteral("name")] = QStringLiteral("Prison");
    if (!zones.loadFile(_writeFeatures(_dir, QStringLiteral("zones.json"), QJsonArray{
            controlZone, prison,
            _circleFeature(QStringLiteral("NEAR"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1500, 90).atDistanceAndAzimuth(400, 0), 150),
            _circleFeature(QStringLiteral("FAR"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 0), 150),
            _circleFeature(QStringLiteral("RET"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2000, 90).atDistanceAndAzimuth(1000, 180), 200),
            _circleFeature(QStringLiteral("INFO"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(1500, 90).atDistanceAndAzimuth(400, 180), 100),
        }), errorString)) {
        return false;
    }
    const GeoZone *const ctr = zones.zoneList().constFirst();
    authorisations.set(GeoZoneAuthorisation{ GeoZoneAuthorisations::zoneKey(ctr), ctr->name(), QStringLiteral("P-77"), QDateTime(), QDateTime() });
    return true;
}

GeoZoneBriefing BriefingMission::build() const
{
    GeoZonePathEvaluator::Path path = GeoZonePathEvaluator::Path::fromPositions({ GeoZone::Position{ center, 100 }, GeoZone::Position{ center.atDistanceAndAzimuth(4000, 90), 100 } });
    path.setTimes(start, start.addSecs(600));
    const QGeoCoordinate halfway = center.atDistanceAndAzimuth(2000, 90);
    GeoZonePathEvaluator::Path contingency = GeoZonePathEvaluator::Path::fromPositions({ GeoZone::Position{ halfway, 100 },
                                                                                         GeoZone::Position{ halfway.atDistanceAndAzimuth(1000, 180), 100 } },
                                                                                       QStringLiteral("a return to rally point 1"));
    contingency.setTimes(start.addSecs(300), QDateTime());
    GeoZoneBriefing::Input input;
    input.mission.name = QStringLiteral("Survey");
    input.mission.plannedStart = start;
    input.mission.plannedEnd = start.addSecs(600);
    input.paths = { path };
    input.contingencyPaths = { contingency };
    input.items = { { 1, 0 }, { 2, 4000 } };
    input.start = start;
    input.end = start.addSecs(600);
    input.authorisations = &authorisations;
    input.sources = GeoZoneSourceVersion::covering(zones.repository()->sourceList(), GeoZoneBox());
    return GeoZoneBriefing::build(zones.catalog(), input);
}

#ifndef GEOZONE_CORE_TEST
ScopedMetricUnits::ScopedMetricUnits()
{
    UnitsSettings *const units = SettingsManager::instance()->unitsSettings();
    _set(units->horizontalDistanceUnits(), UnitsSettings::HorizontalDistanceUnitsMeters);
    _set(units->verticalDistanceUnits(), UnitsSettings::VerticalDistanceUnitsMeters);
}

ScopedMetricUnits::~ScopedMetricUnits()
{
    for (const auto &[fact, value] : std::as_const(_saved)) {
        fact->setRawValue(value);
    }
}

void ScopedMetricUnits::_set(Fact *fact, const QVariant &value)
{
    _saved.append({ fact, fact->rawValue() });
    fact->setRawValue(value);
}

TestFeedServer::TestFeedServer()
{
    QFile fixture(QStringLiteral(":/unittest/ed318_test.json"));
    if (fixture.open(QIODevice::ReadOnly)) {
        body = fixture.readAll();
    }
    (void) connect(this, &QTcpServer::newConnection, this, [this]() {
        while (QTcpSocket *const socket = nextPendingConnection()) {
            (void) connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                const qsizetype headerEnd = request.indexOf("\r\n\r\n");
                if ((headerEnd < 0) || ((request.size() - headerEnd - 4) < header(request, "Content-Length").toLongLong())) {
                    return;
                }
                requestCount++;
                socket->setProperty("counted", true);
                requests.append(request);
                const QByteArray path = request.split(' ').value(1);
                requestedPaths.append(path);
                if (delays.value(path) > 0) {
                    QTimer::singleShot(delays.value(path), socket, [this, socket, path, request]() { _respond(socket, path, request); });
                } else {
                    _respond(socket, path, request);
                }
            });
            (void) connect(socket, &QTcpSocket::disconnected, socket, [this, socket]() {
                if (socket->property("counted").toBool() && !socket->property("answered").toBool()) {
                    abandonedCount++;
                }
                socket->deleteLater();
            });
        }
    });
}

QByteArray TestFeedServer::header(const QByteArray &request, const QByteArray &name)
{
    for (const QByteArray &line : request.left(request.indexOf("\r\n\r\n")).split('\n')) {
        const qsizetype colon = line.indexOf(':');
        if ((colon > 0) && (line.left(colon).trimmed().compare(name, Qt::CaseInsensitive) == 0)) {
            return line.mid(colon + 1).trimmed();
        }
    }
    return QByteArray();
}

void TestFeedServer::_respond(QTcpSocket *socket, const QByteArray &target, const QByteArray &request)
{
    const QByteArray path = files.contains(target) ? target : target.split('?').first();
    QByteArray status = "200 OK";
    QByteArray content;
    QByteArray extraHeaders = headers.value(path);
    if (handler && handler(request, status, content, extraHeaders)) {
        _write(socket, status, content, extraHeaders);
        return;
    }
    if (const QByteArray location = redirects.value(path); !location.isEmpty()) {
        _write(socket, "302 Found", QByteArray(), extraHeaders + "Location: " + location + "\r\n");
        return;
    }
    if (const QByteArray etag = etags.value(path); !etag.isEmpty()) {
        extraHeaders += "ETag: " + etag + "\r\n";
        // Header names are case insensitive, Qt sends them in lower case
        if (request.toLower().contains("if-none-match: " + etag.toLower())) {
            _answer(socket, "HTTP/1.1 304 Not Modified\r\n" + extraHeaders + "Content-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }
    }
    if (files.contains(path)) {
        content = files.value(path);
    } else if (path == "/ok") {
        status = okPathStatus;
        content = status.startsWith("200") ? body : QByteArray();
    } else if (path == "/missing") {
        status = "404 Not Found";
    } else if (path == "/broken") {
        status = "500 Internal Server Error";
    } else if (path == "/hang") {
        return;
    }
    _write(socket, status, content, extraHeaders);
}

void TestFeedServer::_write(QTcpSocket *socket, const QByteArray &status, const QByteArray &content, const QByteArray &extraHeaders)
{
    _answer(socket, "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nConnection: close\r\n" + extraHeaders + "Content-Length: " +
                        QByteArray::number(content.size()) + "\r\n\r\n" + content);
}

void TestFeedServer::_answer(QTcpSocket *socket, const QByteArray &response)
{
    answeredCount++;
    socket->setProperty("answered", true);
    (void) socket->write(response);
    socket->disconnectFromHost();
}
#endif

} // namespace GeoZoneTestHelpers
