/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneManager.h"
#include "GeoZoneMapShape.h"
#include "GeoZoneParser.h"
#include "QGCApplication.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"

#include <QtCore/qapplicationstatic.h>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtPositioning/QGeoPolygon>

QGC_LOGGING_CATEGORY(GeoZoneManagerLog, "qgc.geozone.geozonemanager")

Q_APPLICATION_STATIC(GeoZoneManager, _geoZoneManager);

GeoZoneManager::GeoZoneManager(const QString &storageDir, QObject *parent)
    : QObject(parent)
    , _storageDir(storageDir)
    , _zones(new QmlObjectListModel(this))
    , _polygonShapes(new QmlObjectListModel(this))
    , _circleShapes(new QmlObjectListModel(this))
    , _activeTimer(new QTimer(this))
{
    if (_storageDir.isEmpty()) {
        _storageDir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("GeoZones"));
    }

    QSettings settings;
    settings.beginGroup(_settingsGroup);
    _visible = settings.value(_visibleKey, true).toBool();
    settings.endGroup();

    _activeTimer->setInterval(_activeUpdateIntervalMSecs);
    (void) connect(_activeTimer, &QTimer::timeout, this, &GeoZoneManager::_updateActive);

    (void) _loadStoredFile();
}

GeoZoneManager::~GeoZoneManager()
{

}

GeoZoneManager *GeoZoneManager::instance()
{
    return _geoZoneManager();
}

QString GeoZoneManager::storedFilePath() const
{
    return QDir(_storageDir).filePath(_storedFileName);
}

QStringList GeoZoneManager::fileDialogFilters() const
{
    return QStringList(tr("ED-269/ED-318 Geozone Files (*.json *.geojson)"));
}

void GeoZoneManager::setVisible(bool visible)
{
    if (visible == _visible) {
        return;
    }

    _visible = visible;

    QSettings settings;
    settings.beginGroup(_settingsGroup);
    settings.setValue(_visibleKey, _visible);
    settings.endGroup();

    emit visibleChanged(_visible);
}

bool GeoZoneManager::loadFile(const QString &fileName)
{
    QString errorString;
    if (!loadFile(fileName, errorString)) {
        qgcApp()->showAppMessage(errorString);
        return false;
    }
    return true;
}

bool GeoZoneManager::loadFile(const QString &fileName, QString &errorString)
{
    errorString.clear();

    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;

    GeoZoneParser::Result result;
    QString parseError;
    if (!GeoZoneParser::parseFile(localFileName, result, parseError)) {
        errorString = tr("Geozone file load failed. %1").arg(parseError);
        return false;
    }

    const QFileInfo sourceInfo(localFileName);
    const QString storedPath = storedFilePath();
    if (sourceInfo.canonicalFilePath() != QFileInfo(storedPath).canonicalFilePath()) {
        (void) QDir().mkpath(_storageDir);
        (void) QFile::remove(storedPath);
        if (!QFile::copy(localFileName, storedPath)) {
            qCWarning(GeoZoneManagerLog) << "Unable to store copy of geozone file at" << storedPath;
        } else {
            // Copies of read-only sources must stay replaceable and removable
            (void) QFile::setPermissions(storedPath, QFile::permissions(storedPath) | QFileDevice::WriteOwner | QFileDevice::WriteUser);
        }
    }

    _sourceName = result.title.isEmpty() ? sourceInfo.fileName() : result.title;
    _formatName = GeoZoneParser::formatToString(result.format);
    _skippedCount = result.skippedFeatures;

    QSettings settings;
    settings.beginGroup(_settingsGroup);
    settings.setValue(_sourceNameKey, _sourceName);
    settings.endGroup();

    _setZones(result.zones);
    setVisible(true);

    qCDebug(GeoZoneManagerLog) << "Loaded" << _zones->count() << _formatName << "zones from" << localFileName;

    return true;
}

bool GeoZoneManager::_loadStoredFile()
{
    const QString storedPath = storedFilePath();
    if (!QFile::exists(storedPath)) {
        return false;
    }

    GeoZoneParser::Result result;
    QString errorString;
    if (!GeoZoneParser::parseFile(storedPath, result, errorString)) {
        qCWarning(GeoZoneManagerLog) << "Unable to reload stored geozone file" << storedPath << errorString;
        return false;
    }

    QSettings settings;
    settings.beginGroup(_settingsGroup);
    _sourceName = settings.value(_sourceNameKey).toString();
    settings.endGroup();
    if (_sourceName.isEmpty()) {
        _sourceName = result.title.isEmpty() ? QFileInfo(storedPath).fileName() : result.title;
    }
    _formatName = GeoZoneParser::formatToString(result.format);
    _skippedCount = result.skippedFeatures;

    _setZones(result.zones);

    return true;
}

void GeoZoneManager::clear()
{
    if (QFile::exists(storedFilePath()) && !QFile::remove(storedFilePath())) {
        qCWarning(GeoZoneManagerLog) << "Unable to remove stored geozone file" << storedFilePath();
    }

    QSettings settings;
    settings.beginGroup(_settingsGroup);
    settings.remove(_sourceNameKey);
    settings.endGroup();

    _sourceName.clear();
    _formatName.clear();
    _skippedCount = 0;
    _clearZones();

    emit zonesChanged();
}

void GeoZoneManager::_clearZones()
{
    _activeTimer->stop();
    _boundingBox = QGeoRectangle();
    _polygonShapes->clearAndDeleteContents();
    _circleShapes->clearAndDeleteContents();
    _zones->clearAndDeleteContents();
}

void GeoZoneManager::_setZones(const QList<GeoZone::Data> &zonesData)
{
    _clearZones();

    QList<QObject*> zones;
    QList<QObject*> polygonShapes;
    QList<QObject*> circleShapes;
    for (const GeoZone::Data &zoneData : zonesData) {
        GeoZone *const zone = new GeoZone(zoneData, this);
        zones.append(zone);
        if (zone->boundingBox().isValid()) {
            _boundingBox = _boundingBox.isValid() ? _boundingBox.united(zone->boundingBox()) : zone->boundingBox();
        }

        // Volumes stacked at different altitudes often share the same footprint, only draw it once
        QList<QList<QList<QGeoCoordinate>>> drawnPolygons;
        QList<QGeoCircle> drawnCircles;
        for (const GeoZone::Volume &volume : zoneData.volumes) {
            if (volume.isCircle) {
                const QGeoCircle circle(volume.center, volume.radius);
                if (!drawnCircles.contains(circle)) {
                    drawnCircles.append(circle);
                    circleShapes.append(new GeoZoneMapShape(zone, volume.center, volume.radius, this));
                }
            } else if (!drawnPolygons.contains(volume.rings)) {
                drawnPolygons.append(volume.rings);
                QGeoPolygon polygon(volume.rings.first());
                for (qsizetype i = 1; i < volume.rings.count(); i++) {
                    polygon.addHole(volume.rings[i]);
                }
                polygonShapes.append(new GeoZoneMapShape(zone, polygon, this));
            }
        }
    }

    // Appending an empty list would insert an invalid row range
    if (!zones.isEmpty()) {
        _zones->append(zones);
    }
    if (!polygonShapes.isEmpty()) {
        _polygonShapes->append(polygonShapes);
    }
    if (!circleShapes.isEmpty()) {
        _circleShapes->append(circleShapes);
    }

    if (_zones->count() > 0) {
        _activeTimer->start();
    }

    emit zonesChanged();
}

void GeoZoneManager::_updateActive()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < _zones->count(); i++) {
        _zones->value<GeoZone*>(i)->updateActive(now);
    }
}

QList<GeoZone*> GeoZoneManager::zonesInArea(const QList<GeoZone::Restriction> &restrictions, const QGeoRectangle &area) const
{
    QList<GeoZone*> zones;
    for (int i = 0; i < _zones->count(); i++) {
        GeoZone *const zone = _zones->value<GeoZone*>(i);
        if (!restrictions.contains(zone->restriction())) {
            continue;
        }
        if (area.isValid() && !area.intersects(zone->boundingBox())) {
            continue;
        }
        zones.append(zone);
    }
    return zones;
}
