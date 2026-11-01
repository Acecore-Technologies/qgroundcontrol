/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneManager.h"
#include "GeoZoneDisplay.h"
#include "GeoZoneEnvironment.h"
#include "GeoZonePlanning.h"
#include "GeoZoneInfo.h"
#include "GeoZoneSettings.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "GeoZoneSourceRepository.h"
#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include <QtCore/qapplicationstatic.h>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QLocale>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

#include <algorithm>

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
    , _sources(new QmlObjectListModel(this))
    , _repository(new GeoZoneSourceRepository(storageDir.isEmpty() ? defaultStorageDir() : storageDir, this))
    , _display(new GeoZoneDisplay(this))
    , _planning(new GeoZonePlanning(QDir(_repository->storageDir()).filePath(GeoZoneAuthorisations::fileName), this))
    , _activeTimer(new QTimer(this))
{
    (void) connect(_repository, &GeoZoneSourceRepository::sourceAppended, _sources, [this](GeoZoneSource *source) { _sources->append(source); });
    (void) connect(_repository, &GeoZoneSourceRepository::sourceRemoved, _sources, [this](int index) { (void) _sources->removeAt(index); });

    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    // The age at which data is outdated may have changed
    (void) connect(settings->staleDays(), &Fact::rawValueChanged, _repository, &GeoZoneSourceRepository::updateValidity);
    (void) connect(settings->liveStaleHours(), &Fact::rawValueChanged, _repository, &GeoZoneSourceRepository::updateValidity);
    // Limits published as flight levels are converted with the QNH, so checks and texts using them are updated
    (void) connect(settings->qnhHPa(), &Fact::rawValueChanged, this, [this]() { emit zonesChanged(nullptr); });
    _repository->setAreaMargin(settings->areaOfInterestMargin()->rawValue().toDouble());
    _repository->setFollowArea(settings->followAreaOfInterest()->rawValue().toBool());
    (void) connect(settings->areaOfInterestMargin(), &Fact::rawValueChanged, _repository, [this](const QVariant &value) { _repository->setAreaMargin(value.toDouble()); });
    (void) connect(settings->followAreaOfInterest(), &Fact::rawValueChanged, _repository, [this](const QVariant &value) { _repository->setFollowArea(value.toBool()); });

    _activeTimer->setInterval(_activeUpdateIntervalMSecs);
    (void) connect(_activeTimer, &QTimer::timeout, this, &GeoZoneManager::_updateActive);

    (void) connect(_repository, &GeoZoneSourceRepository::sourceAboutToChange, this, &GeoZoneManager::zonesAboutToChange);
    (void) connect(_repository, &GeoZoneSourceRepository::sourceChanged, this, &GeoZoneManager::_endZonesChange);
    (void) connect(_repository, &GeoZoneSourceRepository::userLoaded, _display, [this]() { _display->setVisible(true); });
    (void) connect(_repository, &GeoZoneSourceRepository::sourceAdded, this, &GeoZoneManager::sourceAdded);
    (void) connect(_repository, &GeoZoneSourceRepository::loadFailed, this, &GeoZoneManager::loadFailed);
    (void) connect(_repository, &GeoZoneSourceRepository::storageFailed, this, &GeoZoneManager::storageFailed);
    (void) connect(_planning->authorisations(), &GeoZoneAuthorisations::saveFailed, this, &GeoZoneManager::storageFailed);
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

void GeoZoneManager::init()
{
    _repository->init();

    // The services taking the area of interest download the zones around the active vehicle
    MultiVehicleManager *const vehicles = MultiVehicleManager::instance();
    (void) connect(vehicles, &MultiVehicleManager::activeVehicleChanged, this, &GeoZoneManager::_activeVehicleChanged);
    _activeVehicleChanged(vehicles->activeVehicle());
}

void GeoZoneManager::_activeVehicleChanged(Vehicle *vehicle)
{
    (void) disconnect(_vehicleConnection);
    if (!vehicle) {
        _repository->setVehiclePosition(QGeoCoordinate());
        return;
    }
    _vehicleConnection = connect(vehicle, &Vehicle::coordinateChanged, _repository, &GeoZoneSourceRepository::setVehiclePosition);
    _repository->setVehiclePosition(vehicle->coordinate());
}

QStringList GeoZoneManager::fileDialogFilters() const
{
    // The format is recognized by the content, zip packages are unpacked
    return { tr("Geozone Files (*.json *.geojson *.xml *.zip)"), tr("All Files (*)") };
}

bool GeoZoneManager::exportProvenance(const QString &fileName)
{
    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    QSaveFile file(localFileName);
    const QByteArray json = QJsonDocument(provenanceReport()).toJson();
    if (!file.open(QIODevice::WriteOnly) || (file.write(json) != json.size()) || !file.commit()) {
        emit fileOperationFinished(false, tr("The geozone source provenance could not be saved to %1: %2").arg(localFileName, file.errorString()));
        return false;
    }
    emit fileOperationFinished(true, tr("The geozone source provenance was saved to %1.").arg(localFileName));
    return true;
}

bool GeoZoneManager::exportPack(const QString &fileName, const QStringList &keys)
{
    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    QStringList sourceKeys = keys;
    if (sourceKeys.isEmpty()) {
        for (const GeoZoneSource *const source : _repository->sources()) {
            if (source->enabled()) {
                sourceKeys.append(source->key());
            }
        }
    }
    QString errorString;
    if (!_repository->exportPack(sourceKeys, localFileName, errorString)) {
        emit fileOperationFinished(false, tr("The geozone pack could not be saved to %1: %2").arg(localFileName, errorString));
        return false;
    }
    emit fileOperationFinished(true, tr("The geozone pack was saved to %1.").arg(localFileName));
    return true;
}

bool GeoZoneManager::importPack(const QString &fileName)
{
    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    QString errorString;
    const int count = _repository->importPack(localFileName, errorString);
    if (count < 0) {
        emit fileOperationFinished(false, errorString);
        return false;
    }
    QString message = tr("%n geozone source(s) imported from %1.", nullptr, count).arg(QFileInfo(localFileName).fileName());
    if (!errorString.isEmpty()) {
        message += QStringLiteral("\n") + tr("Left out:") + QStringLiteral("\n") + errorString;
    }
    emit fileOperationFinished(errorString.isEmpty(), message);
    return true;
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

int GeoZoneManager::disabledSourceCount() const
{
    const QList<const GeoZoneSource*> sources = _repository->sourceList();
    return static_cast<int>(std::count_if(sources.cbegin(), sources.cend(), [](const GeoZoneSource *source) { return !source->enabled(); }));
}

void GeoZoneManager::_endZonesChange(GeoZoneSource *source)
{
    const QGeoRectangle previousBoundingBox = _catalog.boundingBox();
    QList<const GeoZoneSource*> sources = _repository->sourceList();
    (void) sources.removeIf([](const GeoZoneSource *enabledSource) { return !enabledSource->enabled(); });
    _catalog.rebuild(sources);

    const QList<GeoZone*> &zones = _catalog.zones();
    if (zones.isEmpty()) {
        _activeTimer->stop();
    } else if (!_activeTimer->isActive()) {
        _activeTimer->start();
    }

    emit zonesChanged(source);
    emit zoneCountChanged();
    if (_catalog.boundingBox() != previousBoundingBox) {
        emit boundingBoxChanged();
    }
}

void GeoZoneManager::_updateActive()
{
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    bool changed = false;
    for (GeoZone *zone : _catalog.zones()) {
        changed = zone->updateActive(now) || changed;
    }
    _repository->updateValidity();
    if (changed) {
        emit activeZonesChanged();
    }
}

QVariantList GeoZoneManager::labelZones(const QGeoRectangle &region) const
{
    return GeoZoneInfo::toVariantList(_catalog.labelZones(region, maxLabelCount));
}

QVariantList GeoZoneManager::zonesAt(const QGeoCoordinate &coordinate) const
{
    QList<const GeoZone*> zones = _catalog.zonesAt(coordinate);
    const int hidden = _display->hiddenRestrictions();
    (void) zones.removeIf([hidden](const GeoZone *zone) { return (hidden & (1 << zone->restriction())) != 0; });
    return GeoZoneInfo::toVariantList(zones);
}

QVariantList GeoZoneManager::searchZones(const QString &text, int maxResults) const
{
    return GeoZoneInfo::toVariantList(_catalog.search(text, maxResults));
}

void GeoZoneManager::showZone(const QVariant &zone)
{
    if (GeoZoneInfo::fromVariant(zone)) {
        emit showZoneRequested(zone);
    }
}
