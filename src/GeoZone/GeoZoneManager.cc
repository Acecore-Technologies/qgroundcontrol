/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneManager.h"
#include "GeoZoneInfo.h"
#include "GeoZoneSettings.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "GeoZoneSourceRepository.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"

#include <QtCore/qapplicationstatic.h>
#include <QtCore/QDir>
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
    , _repository(new GeoZoneSourceRepository(storageDir.isEmpty() ? defaultStorageDir() : storageDir, this))
    , _authorisations(new GeoZoneAuthorisations(QDir(_repository->storageDir()).filePath(GeoZoneAuthorisations::fileName), this))
    , _activeTimer(new QTimer(this))
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    (void) connect(settings->showZones(), &Fact::rawValueChanged, this, [this]() { emit visibleChanged(visible()); });
    (void) connect(settings->hiddenRestrictions(), &Fact::rawValueChanged, this, &GeoZoneManager::hiddenRestrictionsChanged);

    _activeTimer->setInterval(_activeUpdateIntervalMSecs);
    (void) connect(_activeTimer, &QTimer::timeout, this, &GeoZoneManager::_updateActive);

    (void) connect(_repository, &GeoZoneSourceRepository::sourceAboutToChange, this, &GeoZoneManager::zonesAboutToChange);
    (void) connect(_repository, &GeoZoneSourceRepository::sourceChanged, this, &GeoZoneManager::_endZonesChange);
    (void) connect(_repository, &GeoZoneSourceRepository::userLoaded, this, [this]() { setVisible(true); });
    (void) connect(_repository, &GeoZoneSourceRepository::sourceAdded, this, &GeoZoneManager::sourceAdded);
    (void) connect(_repository, &GeoZoneSourceRepository::loadFailed, this, &GeoZoneManager::loadFailed);
    (void) connect(_repository, &GeoZoneSourceRepository::storageFailed, this, &GeoZoneManager::storageFailed);
    (void) connect(_authorisations, &GeoZoneAuthorisations::saveFailed, this, &GeoZoneManager::storageFailed);
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
    return GeoZoneSettings::instance()->showZones()->rawValue().toBool();
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
    GeoZoneSettings::instance()->showZones()->setRawValue(visible);
}

int GeoZoneManager::hiddenRestrictions() const
{
    return GeoZoneSettings::instance()->hiddenRestrictions()->rawValue().toInt();
}

void GeoZoneManager::setRestrictionVisible(int restriction, bool visible)
{
    const int bit = 1 << restriction;
    const int hidden = visible ? (hiddenRestrictions() & ~bit) : (hiddenRestrictions() | bit);
    GeoZoneSettings::instance()->hiddenRestrictions()->setRawValue(hidden);
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
    const QDateTime now = QDateTime::currentDateTimeUtc();
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
    QList<GeoZone*> zones = _catalog.zonesAt(coordinate);
    const int hidden = hiddenRestrictions();
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
