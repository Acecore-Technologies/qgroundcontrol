/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "GeoFenceController.h"
#include "GeoZone.h"
#include "GeoZoneDownloader.h"
#include "GeoZoneAlertLog.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneFenceImporter.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneGuidedCheck.h"
#include "GeoZoneIndex.h"
#include "GeoZoneInfo.h"
#include "GeoZoneLoader.h"
#include "GeoZoneProviders.h"
#include "GeoZoneManager.h"
#include "GeoZoneDisplay.h"
#include "GeoZonePlanning.h"
#include "GeoZoneMapLayer.h"
#include "GeoZoneMapModel.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMonitor.h"
#include "GeoZoneParser.h"
#include "GeoZonePlanData.h"
#include "GeoZoneProfile.h"
#include "GeoZoneRoutePlanner.h"
#include "GeoZoneSettings.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "GeoZoneTerrain.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "QGCMapPolygon.h"
#include "QGCZip.h"
#include "SettingsManager.h"
#include "MissionSettingsItem.h"
#include "SimpleMissionItem.h"
#include "SurveyComplexItem.h"
#include "UnitsSettings.h"
#include "Vehicle.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QRandomGenerator>
#include <QtCore/QScopeGuard>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimeZone>
#include <QtCore/QUrl>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <algorithm>
#include <cmath>
#include <limits>

/// Fixtures and helpers shared by the geozone tests
namespace GeoZoneTestHelpers {

/// Owns the zones a test creates, as sources do
struct ZoneOwner {
    QList<QSharedPointer<GeoZone>> zones;

    GeoZone *add(const GeoZone::Data &data)
    {
        zones.append(QSharedPointer<GeoZone>::create(data));
        return zones.last().data();
    }
};


inline GeoZoneParser::Result _parseResource(const QString &name)
{
    GeoZoneParser::Result result;
    QString errorString;
    if (!GeoZoneParser::parseFile(QStringLiteral(":/unittest/%1").arg(name), result, errorString)) {
        qWarning() << "Parse failed" << name << errorString;
    }
    return result;
}

inline QDateTime _utc(const QDate &date, const QTime &time)
{
    return QDateTime(date, time, QTimeZone::UTC);
}

inline QJsonObject _circleFeature(const QString &identifier, const QString &restriction, const QGeoCoordinate &center, double radius)
{
    return QJsonObject{
        { "identifier", identifier },
        { "restriction", restriction },
        { "geometry", QJsonArray{ QJsonObject{ { "horizontalProjection", QJsonObject{
            { "type", "Circle" }, { "center", QJsonArray{ center.longitude(), center.latitude() } }, { "radius", radius } } } } } },
    };
}

inline QJsonObject _polygonFeature(const QString &identifier, const QString &restriction, const QList<QGeoCoordinate> &ring)
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

/// Writes an ED-269 file with the features to the directory
inline QString _writeFeatures(const QTemporaryDir &dir, const QString &name, const QJsonArray &features)
{
    const QString fileName = dir.filePath(name);
    QFile file(fileName);
    if (file.open(QIODevice::WriteOnly)) {
        (void) file.write(QJsonDocument(QJsonObject{ { "features", features } }).toJson(QJsonDocument::Compact));
    }
    return fileName;
}

inline QList<QGeoCoordinate> _circleRing(const QGeoCoordinate &center, double radius, int vertexCount)
{
    QList<QGeoCoordinate> ring;
    for (int i = 0; i < vertexCount; i++) {
        ring.append(center.atDistanceAndAzimuth(radius, i * 360.0 / vertexCount));
    }
    return ring;
}

inline double _distanceToRing(const QPointF &point, const QList<QPointF> &ring)
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



/// Answers requests by path: /ok with the ED-318 test file, /missing 404, /broken 500, /empty an empty 200 and
/// /hang never. Counts the requests.
class TestFeedServer : public QTcpServer
{
public:
    TestFeedServer()
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
                    if (request.contains("\r\n\r\n")) {
                        requestCount++;
                        _respond(socket, request.split(' ').value(1));
                    }
                });
                (void) connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QString url(const QString &path) const { return QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path); }

    QByteArray body;
    int requestCount = 0;
    QByteArray okPathStatus = "200 OK";     ///< Status of /ok, to make a feed fail later
    QHash<QByteArray, QByteArray> files;    ///< Content served by path
    QList<QByteArray> requestedPaths;

private:
    void _respond(QTcpSocket *socket, const QByteArray &path)
    {
        QByteArray status = "200 OK";
        QByteArray content;
        requestedPaths.append(path);
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
        (void) socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                             QByteArray::number(content.size()) + "\r\n\r\n" + content);
        socket->disconnectFromHost();
    }
};


} // namespace GeoZoneTestHelpers
