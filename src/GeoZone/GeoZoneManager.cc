/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneManager.h"
#include "GeoZoneFenceImporter.h"
#include "GeoZoneMapModel.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMonitor.h"
#include "GeoZoneProfile.h"
#include "GeoZoneRoutePlanner.h"
#include "GeoZoneSettings.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "GeoZoneSourceRepository.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"

#include <QtCore/qapplicationstatic.h>
#include <QtCore/QDir>
#include <QtCore/QLocale>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtQml/QQmlEngine>

QGC_LOGGING_CATEGORY(GeoZoneManagerLog, "qgc.geozone.geozonemanager")

Q_APPLICATION_STATIC(GeoZoneManager, _geoZoneManager);

namespace {

QString defaultStorageDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("GeoZones"));
}

} // namespace

GeoZoneManager::GeoZoneManager(const QString &storageDir, QObject *parent)
    : QObject(parent)
    , _repository(new GeoZoneSourceRepository(storageDir.isEmpty() ? defaultStorageDir() : storageDir, this))
    , _zones(new QmlObjectListModel(this))
    , _activeTimer(new QTimer(this))
{
    GeoZoneSettings *const settings = SettingsManager::instance()->geoZoneSettings();
    (void) connect(settings->showZones(), &Fact::rawValueChanged, this, [this]() { emit visibleChanged(visible()); });

    _activeTimer->setInterval(_activeUpdateIntervalMSecs);
    (void) connect(_activeTimer, &QTimer::timeout, this, &GeoZoneManager::_updateActive);

    (void) connect(_repository, &GeoZoneSourceRepository::sourceAboutToChange, this, &GeoZoneManager::zonesAboutToChange);
    (void) connect(_repository, &GeoZoneSourceRepository::sourceChanged, this, &GeoZoneManager::_endZonesChange);
    (void) connect(_repository, &GeoZoneSourceRepository::userLoaded, this, [this]() { setVisible(true); });
    (void) connect(_repository, &GeoZoneSourceRepository::sourceAdded, this, &GeoZoneManager::sourceAdded);
    (void) connect(_repository, &GeoZoneSourceRepository::loadFailed, this, &GeoZoneManager::loadFailed);
    (void) connect(_repository, &GeoZoneSourceRepository::storageFailed, this, &GeoZoneManager::storageFailed);
    (void) connect(_repository, &GeoZoneSourceRepository::outdatedDataFound, this, &GeoZoneManager::outdatedDataFound);
    (void) connect(_repository, &GeoZoneSourceRepository::busyChanged, this, &GeoZoneManager::busyChanged);
    (void) connect(_repository, &GeoZoneSourceRepository::progressChanged, this, &GeoZoneManager::progressChanged);
    (void) connect(_repository, &GeoZoneSourceRepository::validityWarningChanged, this, &GeoZoneManager::validityWarningChanged);
}

GeoZoneManager::~GeoZoneManager()
{
}

GeoZoneManager *GeoZoneManager::instance()
{
    return _geoZoneManager();
}

void GeoZoneManager::registerQmlTypes()
{
    (void) qmlRegisterUncreatableType<GeoZone>("QGroundControl.GeoZone", 1, 0, "GeoZone", "Reference only");
    (void) qmlRegisterUncreatableType<GeoZoneMonitor>("QGroundControl.GeoZone", 1, 0, "GeoZoneMonitor", "Reference only");
    (void) qmlRegisterType<GeoZoneMapModel>("QGroundControl.GeoZone", 1, 0, "GeoZoneMapModel");
    (void) qmlRegisterType<GeoZoneMissionCheck>("QGroundControl.GeoZone", 1, 0, "GeoZoneMissionCheck");
    (void) qmlRegisterType<GeoZoneFenceImporter>("QGroundControl.GeoZone", 1, 0, "GeoZoneFenceImporter");
    (void) qmlRegisterType<GeoZoneProfile>("QGroundControl.GeoZone", 1, 0, "GeoZoneProfile");
    (void) qmlRegisterType<GeoZoneRoutePlanner>("QGroundControl.GeoZone", 1, 0, "GeoZoneRoutePlanner");
}

void GeoZoneManager::init()
{
    _repository->init();
}

QStringList GeoZoneManager::fileDialogFilters() const
{
    return QStringList(tr("ED-269/ED-318 Geozone Files (*.json *.geojson)"));
}

bool GeoZoneManager::addArea(const QString &provider, const QGeoRectangle &area)
{
    const QString url = areaFeedUrl(provider, area);
    if (url.isEmpty()) {
        emit loadFailed(provider, tr("Geozones can be downloaded for an area up to %1 degrees wide and high. Zoom in on the map.").arg(GeoZoneProviders::maxAreaDegrees));
        return false;
    }
    addUrl(url);
    return true;
}

bool GeoZoneManager::visible() const
{
    return SettingsManager::instance()->geoZoneSettings()->showZones()->rawValue().toBool();
}

void GeoZoneManager::setPlannedStart(const QDateTime &plannedStart)
{
    const QDateTime start = plannedStart.isValid() ? plannedStart.toUTC() : QDateTime();
    if (start != _plannedStart) {
        _plannedStart = start;
        emit plannedStartChanged();
    }
}

void GeoZoneManager::setVisible(bool visible)
{
    SettingsManager::instance()->geoZoneSettings()->showZones()->setRawValue(visible);
}

void GeoZoneManager::_endZonesChange(GeoZoneSource *source)
{
    const QGeoRectangle previousBoundingBox = _catalog.boundingBox();
    _catalog.rebuild(_repository->sourceList());

    // A single reset, so bindings to the count don't see the zones of other sources disappear
    const QList<GeoZone*> &zones = _catalog.zones();
    (void) _zones->swapObjectList(QList<QObject*>(zones.cbegin(), zones.cend()));
    if (zones.isEmpty()) {
        _activeTimer->stop();
    } else if (!_activeTimer->isActive()) {
        _activeTimer->start();
    }

    emit zonesChanged(source);
    if (_catalog.boundingBox() != previousBoundingBox) {
        emit boundingBoxChanged();
    }
}

void GeoZoneManager::_updateActive()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < _zones->count(); i++) {
        _zones->value<GeoZone*>(i)->updateActive(now);
    }
    _repository->updateValidity();
}

QList<QObject*> GeoZoneManager::labelZones(const QGeoRectangle &region) const
{
    const QList<GeoZone*> zones = _catalog.labelZones(region, maxLabelCount);
    return QList<QObject*>(zones.cbegin(), zones.cend());
}

QList<QObject*> GeoZoneManager::zonesAt(const QGeoCoordinate &coordinate) const
{
    const QList<GeoZone*> zones = _catalog.zonesAt(coordinate);
    return QList<QObject*>(zones.cbegin(), zones.cend());
}

QList<QObject*> GeoZoneManager::searchZones(const QString &text, int maxResults) const
{
    const QList<GeoZone*> zones = _catalog.search(text, maxResults);
    return QList<QObject*>(zones.cbegin(), zones.cend());
}
