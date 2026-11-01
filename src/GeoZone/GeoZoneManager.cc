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
#include "GeoZoneSettings.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/qapplicationstatic.h>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QFutureWatcher>
#include <QtCore/QLocale>
#include <QtCore/QMap>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtQml/QQmlEngine>

#include <algorithm>
#include <numeric>

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
    , _store(storageDir.isEmpty() ? defaultStorageDir() : storageDir)
    , _downloader(new GeoZoneDownloader(this))
    , _zones(new QmlObjectListModel(this))
    , _sources(new QmlObjectListModel(this))
    , _activeTimer(new QTimer(this))
    , _refreshTimer(new QTimer(this))
{
    GeoZoneSettings *const settings = SettingsManager::instance()->geoZoneSettings();
    (void) connect(settings->showZones(), &Fact::rawValueChanged, this, [this]() { emit visibleChanged(visible()); });
    // The age at which data is outdated may have changed
    (void) connect(settings->staleDays(), &Fact::rawValueChanged, this, &GeoZoneManager::_updateValidity);
    (void) connect(settings->liveStaleHours(), &Fact::rawValueChanged, this, &GeoZoneManager::_updateValidity);

    _activeTimer->setInterval(_activeUpdateIntervalMSecs);
    (void) connect(_activeTimer, &QTimer::timeout, this, &GeoZoneManager::_updateActive);

    _refreshTimer->setInterval(_refreshCheckIntervalMSecs);
    (void) connect(_refreshTimer, &QTimer::timeout, this, &GeoZoneManager::_refreshDueFeeds);

    (void) connect(_downloader, &GeoZoneDownloader::progressChanged, this, &GeoZoneManager::progressChanged);
}

GeoZoneManager::~GeoZoneManager()
{
    // Shapes are owned by the sources
    _shapes.clear();
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
}

void GeoZoneManager::init()
{
    _startupRefreshPending = true;
    _startupCheckPending = true;
    loadStoredSources(LoadMode::Background);
    _refreshTimer->start();
    if (!busy()) {
        _idle();
    }
}

QStringList GeoZoneManager::fileDialogFilters() const
{
    return QStringList(tr("ED-269/ED-318 Geozone Files (*.json *.geojson)"));
}

QVariantList GeoZoneManager::feedPresets()
{
    const auto preset = [](const QString &name, const QString &url) {
        return QVariantMap{ { QStringLiteral("name"), name }, { QStringLiteral("url"), url } };
    };
    return QVariantList{
        preset(tr("Finland (Traficom)"),        QStringLiteral("https://eservices.traficom.fi/Ilmatilasovellus/api/uas-reservations/json?lang=en")),
        preset(tr("Latvia (LGS)"),              QStringLiteral("https://drz.lv/api/v1/export-history/UASZoneVersion")),
        preset(tr("Luxembourg (Geoportail)"),   QStringLiteral("https://drones.geoportail.lu/zones")),
        preset(tr("Sweden (LFV)"),              QStringLiteral("https://dronechart.lfv.se/data/uas_zones_ED318.json")),
        preset(tr("United States TFRs (FAA)"),  QStringLiteral("https://tfr.faa.gov/geoserver/TFR/ows?service=WFS&version=1.1.0&request=GetFeature&typeName=TFR:V_TFR_LOC&outputFormat=application/json")),
        preset(tr("Switzerland (FOCA)"),        QStringLiteral("https://data.geo.admin.ch/ch.bazl.einschraenkungen-drohnen/einschraenkungen-drohnen/einschraenkungen-drohnen_4326.json")),
    };
}

bool GeoZoneManager::visible() const
{
    return SettingsManager::instance()->geoZoneSettings()->showZones()->rawValue().toBool();
}

void GeoZoneManager::setVisible(bool visible)
{
    SettingsManager::instance()->geoZoneSettings()->showZones()->setRawValue(visible);
}

GeoZoneSource *GeoZoneManager::source(const QString &key) const
{
    for (int i = 0; i < _sources->count(); i++) {
        GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        if (source->key() == key) {
            return source;
        }
    }
    return nullptr;
}

void GeoZoneManager::addFile(const QString &fileName)
{
    GeoZoneLoader::Job job = GeoZoneLoader::fileJob(fileName);
    job.request = GeoZoneLoader::Request::Add;
    job.generation = ++_generations[job.key];
    _startLoad(job, LoadMode::Background);
}

bool GeoZoneManager::loadFile(const QString &fileName, QString &errorString)
{
    GeoZoneLoader::Job job = GeoZoneLoader::fileJob(fileName);
    job.generation = ++_generations[job.key];
    _prepareJob(job);

    const GeoZoneLoader::Result loadResult = GeoZoneLoader::run(job, _store);
    _applyLoad(job, loadResult);
    if (!loadResult.errorString.isEmpty()) {
        errorString = tr("Geozone file load failed. %1").arg(loadResult.errorString);
        return false;
    }
    errorString.clear();
    setVisible(true);
    return true;
}

void GeoZoneManager::addUrl(const QString &url)
{
    const QString trimmedUrl = url.trimmed();
    const QUrl parsedUrl(trimmedUrl);
    const bool web = QStringList({ QStringLiteral("http"), QStringLiteral("https") }).contains(parsedUrl.scheme()) && !parsedUrl.host().isEmpty();
    if (!parsedUrl.isValid() || (!web && !parsedUrl.isLocalFile())) {
        emit loadFailed(trimmedUrl, tr("Geozone download failed. %1 is not a valid http, https or file address.").arg(trimmedUrl));
        return;
    }
    _download(trimmedUrl, GeoZoneLoader::Request::Add);
}

void GeoZoneManager::refreshFeeds()
{
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        if (source->isFeed()) {
            _download(source->url(), GeoZoneLoader::Request::User);
        }
    }
}

void GeoZoneManager::_refreshDueFeeds()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        if (!source->busy() && source->refreshDue(now)) {
            _download(source->url(), GeoZoneLoader::Request::Automatic);
        }
    }
}

void GeoZoneManager::_download(const QString &url, GeoZoneLoader::Request request)
{
    const int generation = ++_generations[url];
    GeoZoneSource *const existing = source(url);
    if (existing) {
        existing->setBusy(true);
    }

    qCDebug(GeoZoneManagerLog) << "Downloading" << url;

    if (QUrl(url).isLocalFile()) {
        GeoZoneLoader::Job job = GeoZoneLoader::feedJob(url);
        job.request = request;
        job.generation = generation;
        _startLoad(job, LoadMode::Background);
        return;
    }

    QString etag;
    QString lastModified;
    if (existing && existing->loaded()) {
        etag = existing->etag();
        lastModified = existing->lastModified();
    }

    _setPending(1);
    _downloader->get(url, etag, lastModified, [this, url, generation, request](const GeoZoneDownloader::Result &download) {
        _downloadComplete(download, url, generation, request);
        _setPending(-1);
    });
}

double GeoZoneManager::progress() const
{
    // Parsing has no progress to report
    return _downloader->progress();
}

QString GeoZoneManager::progressText() const
{
    if (_downloader->count() == 0) {
        return busy() ? tr("Loading geozones...") : QString();
    }
    return _downloader->progressText();
}

void GeoZoneManager::_downloadComplete(const GeoZoneDownloader::Result &download, const QString &url, int generation, GeoZoneLoader::Request request)
{
    if (_generations.value(url) != generation) {
        // Removed or downloaded again in the meantime
        return;
    }

    GeoZoneSource *const existing = source(url);
    if ((download.status == GeoZoneDownloader::Result::Status::NotModified) && existing) {
        qCDebug(GeoZoneManagerLog) << "Feed not modified" << url;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        existing->setStoredDates(now, now);
        existing->setBusy(false);
        existing->setErrorString(QString());
        // The stored copy is only confirmed current when it is the version the validators belong to
        if (auto it = _storedEntries.find(url); (it != _storedEntries.end()) && (it->etag == existing->etag()) && (it->lastModified == existing->lastModified())) {
            it->loadedAt = now;
            it->fileDate = now;
            _saveIndex();
        }
        _updateValidity();
        return;
    }

    if (download.status != GeoZoneDownloader::Result::Status::Ok) {
        const QString error = download.errorString.isEmpty() ? tr("The server responded with status %1.").arg(304) : download.errorString;
        qCWarning(GeoZoneManagerLog) << "Download failed" << url << error;
        if (existing) {
            existing->setBusy(false);
            existing->setErrorString(error);
        }
        if (request != GeoZoneLoader::Request::Automatic) {
            emit loadFailed(url, tr("Geozone download from %1 failed. %2").arg(url, error));
        }
        return;
    }

    GeoZoneLoader::Job job = GeoZoneLoader::feedJob(url);
    job.data = download.data;
    if (existing && (existing->format() == GeoZoneParser::Format::FaaTfr)) {
        job.formatHint = GeoZoneParser::Format::FaaTfr;
    }
    job.etag = download.etag;
    job.lastModified = download.lastModified;
    job.request = request;
    job.generation = generation;
    _startLoad(job, LoadMode::Background);
}

void GeoZoneManager::_prepareJob(GeoZoneLoader::Job &job) const
{
    if (_storeReadOnly) {
        job.storedFileName.clear();
    }
    // Read here, the default locale changes on this thread with the language setting
    job.language = QLocale().name().left(2).toLower();
}

void GeoZoneManager::_startLoad(GeoZoneLoader::Job job, LoadMode mode)
{
    if (GeoZoneSource *const existing = source(job.key)) {
        existing->setBusy(true);
    }
    _prepareJob(job);
    _setPending(1);

    if (mode == LoadMode::Synchronous) {
        _applyLoad(job, GeoZoneLoader::run(job, _store));
        _setPending(-1);
        return;
    }

    QFutureWatcher<GeoZoneLoader::Result> *const watcher = new QFutureWatcher<GeoZoneLoader::Result>(this);
    (void) connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, job]() {
        _applyLoad(job, watcher->result());
        watcher->deleteLater();
        _setPending(-1);
    });
    watcher->setFuture(QtConcurrent::run(&GeoZoneLoader::run, job, _store));
}

void GeoZoneManager::_applyLoad(const GeoZoneLoader::Job &job, const GeoZoneLoader::Result &loadResult)
{
    if (_generations.value(job.key) != job.generation) {
        // Removed or loaded again in the meantime
        if (loadResult.stored) {
            _store.removeFile(job.storedFileName);
        }
        return;
    }

    GeoZoneSource *source = this->source(job.key);
    if (source) {
        source->setBusy(false);
    }

    if (!loadResult.errorString.isEmpty()) {
        qCWarning(GeoZoneManagerLog) << "Load failed" << job.key << loadResult.errorString;
        if (source) {
            source->setErrorString(loadResult.errorString);
        }
        if (job.request != GeoZoneLoader::Request::Automatic) {
            emit loadFailed(job.key, tr("Geozone file load failed. %1").arg(loadResult.errorString));
        }
        return;
    }

    // Live feeds are downloaded often and mostly unchanged, keep their zones so maps and alerts aren't reset
    const bool unchanged = source && source->loaded() && !loadResult.result.contentHash.isEmpty() &&
                           (source->contentHash() == loadResult.result.contentHash) && (source->format() == loadResult.result.format);
    if (unchanged) {
        source->markCurrent(loadResult.result, job.loadedAt, job.fileDate);
        qCDebug(GeoZoneManagerLog) << "Zones unchanged" << job.key;
    } else {
        if (!source) {
            source = new GeoZoneSource(job.key, job.url, this);
            _sources->append(source);
        }
        _beginZonesChange(source);
        source->setResult(loadResult.result, job.loadedAt, job.fileDate);
        _endZonesChange(source);
        qCDebug(GeoZoneManagerLog) << "Loaded" << source->zoneCount() << source->formatName() << "zones from" << job.key;
    }
    source->setErrorString(QString());
    if (source->isFeed() && !job.data.isEmpty()) {
        source->setValidators(job.etag, job.lastModified);
    }

    _updateStoredEntry(job, loadResult);
    _updateValidity();

    if (unchanged) {
        return;
    }
    if (job.request != GeoZoneLoader::Request::Automatic) {
        setVisible(true);
    }
    // Maps move to added sources, which refreshing the feeds must not do
    if (job.request == GeoZoneLoader::Request::Add) {
        emit sourceAdded(source);
    }
}

void GeoZoneManager::_updateStoredEntry(const GeoZoneLoader::Job &job, const GeoZoneLoader::Result &loadResult)
{
    if (job.storedFileName.isEmpty()) {
        // Loaded from the stored copy itself
        return;
    }
    if (!loadResult.stored) {
        // Keep the previous copy rather than losing the source
        _reportStorageError(tr("A copy of the geozone source %1 could not be saved, the previous data will be loaded at the next start. %2")
                                .arg(job.key, loadResult.storeError));
        return;
    }

    GeoZoneStore::Entry &entry = _storedEntries[job.key];
    const QString previousFileName = entry.fileName;
    entry.key = job.key;
    entry.url = job.url;
    entry.fileName = job.storedFileName;
    entry.loadedAt = job.loadedAt;
    entry.fileDate = job.fileDate;
    if (!job.data.isEmpty()) {
        entry.etag = job.etag;
        entry.lastModified = job.lastModified;
    }
    if (previousFileName != entry.fileName) {
        _store.removeFile(previousFileName);
    }
    _saveIndex();
}

void GeoZoneManager::_reportStorageError(const QString &message)
{
    qCWarning(GeoZoneManagerLog) << message;
    if (!_storageErrorReported) {
        _storageErrorReported = true;
        emit storageFailed(message);
    }
}

void GeoZoneManager::_setPending(int delta)
{
    const bool wasBusy = busy();
    _pendingCount += delta;
    if (busy() != wasBusy) {
        emit busyChanged(busy());
        emit progressChanged();
    }
    if (!busy() && wasBusy) {
        _idle();
    }
}

void GeoZoneManager::_idle()
{
    if (_startupRefreshPending) {
        _startupRefreshPending = false;
        _refreshDueFeeds();
        if (busy()) {
            return;
        }
    }

    if (_startupCheckPending) {
        _startupCheckPending = false;
        _updateValidity();
        if (!_validityWarning.isEmpty()) {
            emit outdatedDataFound(tr("Some geozone data may be outdated. Load current files in Application Settings > Geozones.\n\n%1").arg(_validityWarning));
        }
    }
}

void GeoZoneManager::removeSource(int index)
{
    if ((index < 0) || (index >= _sources->count())) {
        return;
    }

    GeoZoneSource *const source = _sources->value<GeoZoneSource*>(index);
    // Drops results of loads and downloads in progress
    _generations[source->key()]++;
    if (source->busy()) {
        source->setBusy(false);
    }

    _beginZonesChange(source);
    (void) _sources->removeAt(index);
    _endZonesChange(source);
    source->deleteLater();

    if (!_storeReadOnly) {
        _store.removeFile(_storedEntries.take(source->key()).fileName);
        _saveIndex();
    }
    _updateValidity();
}

void GeoZoneManager::clear()
{
    while (_sources->count() > 0) {
        removeSource(_sources->count() - 1);
    }
    // Drops results of new sources still loading
    for (auto it = _generations.begin(); it != _generations.end(); ++it) {
        it.value()++;
    }
}

void GeoZoneManager::_beginZonesChange(GeoZoneSource *source)
{
    emit zonesAboutToChange(source);
}

void GeoZoneManager::_endZonesChange(GeoZoneSource *source)
{
    _zoneList.clear();
    _shapes.clear();
    QGeoRectangle boundingBox;
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const zoneSource = _sources->value<GeoZoneSource*>(i);
        _zoneList.append(zoneSource->zones());
        _shapes.append(zoneSource->shapes());
        if (zoneSource->boundingBox().isValid()) {
            boundingBox = boundingBox.isValid() ? boundingBox.united(zoneSource->boundingBox()) : zoneSource->boundingBox();
        }
    }
    const bool boundingBoxChanged = (boundingBox != _boundingBox);
    _boundingBox = boundingBox;

    QHash<const GeoZone*, qsizetype> zoneIndexes;
    zoneIndexes.reserve(_zoneList.count());
    for (qsizetype i = 0; i < _zoneList.count(); i++) {
        zoneIndexes.insert(_zoneList[i], i);
    }
    _zoneShapes = QList<QList<const GeoZoneShape*>>(_zoneList.count());
    for (const GeoZoneShape *shape : std::as_const(_shapes)) {
        _zoneShapes[zoneIndexes.value(shape->zone())].append(shape);
    }
    _index.build(_zoneList);

    // A single reset, so bindings to the count don't see the zones of other sources disappear
    (void) _zones->swapObjectList(QList<QObject*>(_zoneList.cbegin(), _zoneList.cend()));
    if (_zoneList.isEmpty()) {
        _activeTimer->stop();
    } else if (!_activeTimer->isActive()) {
        _activeTimer->start();
    }

    emit zonesChanged(source);
    if (boundingBoxChanged) {
        emit this->boundingBoxChanged();
    }
}

QList<const GeoZoneShape*> GeoZoneManager::shapesInArea(double south, double west, double north, double east) const
{
    QList<const GeoZoneShape*> shapes;
    for (const int index : _index.query(south, west, north, east)) {
        shapes.append(_zoneShapes[index]);
    }
    return shapes;
}

void GeoZoneManager::loadStoredSources(LoadMode mode)
{
    const GeoZoneStore::Index index = _store.readIndex();
    switch (index.status) {
    case GeoZoneStore::IndexStatus::Ok:
    case GeoZoneStore::IndexStatus::Missing:
        break;
    case GeoZoneStore::IndexStatus::Unreadable: {
        qCWarning(GeoZoneManagerLog) << "Unreadable geozone source index" << index.errorString;
        const QString backupDir = _store.moveToBackup();
        _reportStorageError(backupDir.isEmpty() ?
            tr("The list of stored geozone sources could not be read (%1). Add the sources again in Application Settings > Geozones.").arg(index.errorString) :
            tr("The list of stored geozone sources could not be read (%1). The stored files were moved to %2. Add the sources again in Application Settings > Geozones.").arg(index.errorString, backupDir));
        return;
    }
    case GeoZoneStore::IndexStatus::Newer:
        _storeReadOnly = true;
        _reportStorageError(tr("The geozone sources were saved by a newer version of QGroundControl and are not loaded. Sources added now will not be kept."));
        return;
    }

    QList<GeoZoneLoader::Job> jobs;
    for (const GeoZoneStore::Entry &entry : index.entries) {
        GeoZoneLoader::Job job;
        job.key = entry.key;
        job.url = entry.url;
        job.sourcePath = _store.path(entry.fileName);
        job.loadedAt = entry.loadedAt;
        job.fileDate = entry.fileDate;
        job.formatHint = GeoZoneLoader::formatHint(job.url);
        job.etag = entry.etag;
        job.lastModified = entry.lastModified;
        if (job.key.isEmpty() || entry.fileName.isEmpty() || !QFile::exists(job.sourcePath) || source(job.key)) {
            qCWarning(GeoZoneManagerLog) << "Skipping stored geozone source" << job.key << entry.fileName;
            continue;
        }
        job.generation = ++_generations[job.key];

        // Keep the order of the sources while they load
        GeoZoneSource *const placeholder = new GeoZoneSource(job.key, job.url, this);
        placeholder->setStoredDates(job.loadedAt, job.fileDate);
        placeholder->setValidators(job.etag, job.lastModified);
        _sources->append(placeholder);
        _storedEntries.insert(job.key, entry);
        jobs.append(job);
    }

    QSet<QString> referenced;
    for (const GeoZoneStore::Entry &entry : std::as_const(_storedEntries)) {
        referenced.insert(entry.fileName);
    }
    _store.removeOrphanFiles(referenced);
    _saveIndex();

    for (const GeoZoneLoader::Job &job : std::as_const(jobs)) {
        _startLoad(job, mode);
    }
}

void GeoZoneManager::_saveIndex()
{
    if (_storeReadOnly) {
        return;
    }

    QList<GeoZoneStore::Entry> entries;
    for (int i = 0; i < _sources->count(); i++) {
        const auto it = _storedEntries.constFind(_sources->value<GeoZoneSource*>(i)->key());
        if (it != _storedEntries.cend()) {
            entries.append(it.value());
        }
    }

    QString errorString;
    if (!_store.writeIndex(entries, errorString)) {
        _reportStorageError(tr("The list of geozone sources could not be saved, sources may be missing at the next start. %1").arg(errorString));
    }
}

void GeoZoneManager::_updateActive()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < _zones->count(); i++) {
        _zones->value<GeoZone*>(i)->updateActive(now);
    }
    _updateValidity();
}

void GeoZoneManager::_updateValidity()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QStringList lines;
    for (int i = 0; i < _sources->count(); i++) {
        GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        source->updateValidity(now);
        if (!source->validityText().isEmpty()) {
            lines.append(QStringLiteral("%1: %2").arg(source->name(), source->validityText()));
        }
    }

    const QString warning = lines.join(QStringLiteral("\n"));
    if (warning != _validityWarning) {
        _validityWarning = warning;
        emit validityWarningChanged();
    }
}

QList<QObject*> GeoZoneManager::labelZones(const QGeoRectangle &region) const
{
    QList<GeoZone*> candidates;
    for (const int index : _index.query(region)) {
        GeoZone *const zone = _zoneList[index];
        if (region.contains(zone->center())) {
            candidates.append(zone);
        }
    }

    if (candidates.count() > maxLabelCount) {
        std::stable_sort(candidates.begin(), candidates.end(), [](const GeoZone *a, const GeoZone *b) {
            return GeoZone::restrictionSeverity(a->restriction()) < GeoZone::restrictionSeverity(b->restriction());
        });
        candidates.resize(maxLabelCount);
    }

    return QList<QObject*>(candidates.cbegin(), candidates.cend());
}

QList<QObject*> GeoZoneManager::zonesAt(const QGeoCoordinate &coordinate) const
{
    QList<GeoZone*> zones;
    if (!coordinate.isValid()) {
        return {};
    }
    for (const int index : _index.query(coordinate.latitude(), coordinate.longitude(), coordinate.latitude(), coordinate.longitude())) {
        GeoZone *const zone = _zoneList[index];
        const QList<GeoZone::Volume> &volumes = zone->data().volumes;
        if (std::any_of(volumes.cbegin(), volumes.cend(), [&coordinate](const GeoZone::Volume &volume) { return GeoZone::volumeContains(volume, coordinate); })) {
            zones.append(zone);
        }
    }
    std::stable_sort(zones.begin(), zones.end(), [](const GeoZone *a, const GeoZone *b) {
        if (a->active() != b->active()) {
            return a->active();
        }
        return GeoZone::restrictionSeverity(a->restriction()) < GeoZone::restrictionSeverity(b->restriction());
    });
    return QList<QObject*>(zones.cbegin(), zones.cend());
}

QList<QObject*> GeoZoneManager::searchZones(const QString &text, int maxResults) const
{
    const QString query = text.trimmed();
    if (query.isEmpty() || (maxResults <= 0)) {
        return {};
    }

    struct Match {
        GeoZone *zone;
        bool prefix;
    };
    QList<Match> matches;
    for (GeoZone *zone : _zoneList) {
        const QString name = zone->displayName();
        if (name.startsWith(query, Qt::CaseInsensitive) || zone->identifier().startsWith(query, Qt::CaseInsensitive)) {
            matches.append({ zone, true });
        } else if (name.contains(query, Qt::CaseInsensitive) || zone->identifier().contains(query, Qt::CaseInsensitive)) {
            matches.append({ zone, false });
        }
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        if (a.prefix != b.prefix) {
            return a.prefix;
        }
        const int severityA = GeoZone::restrictionSeverity(a.zone->restriction());
        const int severityB = GeoZone::restrictionSeverity(b.zone->restriction());
        if (severityA != severityB) {
            return severityA < severityB;
        }
        return a.zone->displayName().compare(b.zone->displayName(), Qt::CaseInsensitive) < 0;
    });

    QList<QObject*> zones;
    for (qsizetype i = 0; i < (std::min)(matches.count(), qsizetype{ maxResults }); i++) {
        zones.append(matches[i].zone);
    }
    return zones;
}

QList<GeoZone*> GeoZoneManager::zonesInArea(const QList<GeoZone::Restriction> &restrictions, const QGeoRectangle &area,
                                            const QDateTime &activeFrom, const QDateTime &activeTo) const
{
    QList<int> candidates;
    if (area.isValid()) {
        candidates = _index.query(area);
    } else {
        candidates.resize(_zoneList.count());
        std::iota(candidates.begin(), candidates.end(), 0);
    }

    QList<GeoZone*> zones;
    for (const int index : std::as_const(candidates)) {
        GeoZone *const zone = _zoneList[index];
        if (!restrictions.contains(zone->restriction())) {
            continue;
        }
        if (activeFrom.isValid() && !(activeTo.isValid() ? zone->isActiveDuring(activeFrom, activeTo) : zone->isActiveAt(activeFrom))) {
            continue;
        }
        zones.append(zone);
    }
    return zones;
}

QList<GeoZone*> GeoZoneManager::zonesCrossingPaths(const QList<GeoZone::Restriction> &restrictions, const QList<QList<GeoZone::Position>> &paths) const
{
    // Candidates near any segment, checked once per path
    QMap<int, QList<int>> candidatePaths;
    for (int pathIndex = 0; pathIndex < paths.count(); pathIndex++) {
        const QList<GeoZone::Position> &path = paths[pathIndex];
        for (qsizetype i = 0; i < path.count(); i++) {
            const QGeoCoordinate &from = path[std::max<qsizetype>(i - 1, 0)].coordinate;
            const QGeoCoordinate &to = path[i].coordinate;
            const QList<int> candidates = _index.query(std::min(from.latitude(), to.latitude()), std::min(from.longitude(), to.longitude()),
                                                       std::max(from.latitude(), to.latitude()), std::max(from.longitude(), to.longitude()));
            for (const int index : candidates) {
                QList<int> &candidatePathList = candidatePaths[index];
                if (candidatePathList.isEmpty() || (candidatePathList.last() != pathIndex)) {
                    candidatePathList.append(pathIndex);
                }
            }
        }
    }

    QList<GeoZone*> zones;
    for (auto it = candidatePaths.cbegin(); it != candidatePaths.cend(); ++it) {
        GeoZone *const zone = _zoneList[it.key()];
        if (!restrictions.contains(zone->restriction())) {
            continue;
        }
        const QList<int> &pathIndexes = it.value();
        if (std::any_of(pathIndexes.cbegin(), pathIndexes.cend(), [zone, &paths](int pathIndex) { return zone->intersectsPath(paths[pathIndex]); })) {
            zones.append(zone);
        }
    }
    return zones;
}

QList<GeoZone*> GeoZoneManager::zonesIntersectingAreas(const QList<GeoZone::Restriction> &restrictions, const QList<GeoZone::Area> &areas) const
{
    QList<GeoZone*> zones;
    for (const GeoZone::Area &area : areas) {
        if (area.polygon.count() < 3) {
            continue;
        }
        for (const int index : _index.query(QGeoRectangle(area.polygon))) {
            GeoZone *const zone = _zoneList[index];
            if (restrictions.contains(zone->restriction()) && !zones.contains(zone) && zone->intersectsArea(area)) {
                zones.append(zone);
            }
        }
    }
    return zones;
}
