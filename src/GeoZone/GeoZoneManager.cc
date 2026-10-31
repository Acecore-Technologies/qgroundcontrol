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
#include "QGCApplication.h"
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
    loadStoredSources(true /* background */);
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
    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    const QFileInfo info(localFileName);

    LoadJob job;
    job.key = info.fileName();
    job.sourcePath = localFileName;
    job.storedFileName = GeoZoneStore::newFileName();
    job.loadedAt = QDateTime::currentDateTimeUtc();
    job.fileDate = info.lastModified().toUTC();
    job.userInitiated = true;
    job.added = true;
    job.generation = ++_generations[job.key];
    _startLoad(job, true /* background */);
}

bool GeoZoneManager::loadFile(const QString &fileName, QString &errorString)
{
    errorString.clear();

    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    const QFileInfo info(localFileName);

    LoadJob job;
    job.key = info.fileName();
    job.sourcePath = localFileName;
    job.storedFileName = GeoZoneStore::newFileName();
    job.loadedAt = QDateTime::currentDateTimeUtc();
    job.fileDate = info.lastModified().toUTC();
    job.generation = ++_generations[job.key];

    const LoadResult loadResult = _runLoad(job, _store);
    if (!loadResult.errorString.isEmpty()) {
        errorString = tr("Geozone file load failed. %1").arg(loadResult.errorString);
        return false;
    }
    _applyLoad(job, loadResult);
    setVisible(true);
    return true;
}

void GeoZoneManager::addUrl(const QString &url)
{
    const QString trimmedUrl = url.trimmed();
    const QUrl parsedUrl(trimmedUrl);
    const bool web = QStringList({ QStringLiteral("http"), QStringLiteral("https") }).contains(parsedUrl.scheme()) && !parsedUrl.host().isEmpty();
    if (!parsedUrl.isValid() || (!web && !parsedUrl.isLocalFile())) {
        qgcApp()->showAppMessage(tr("Geozone download failed. %1 is not a valid http, https or file address.").arg(trimmedUrl));
        return;
    }
    _download(trimmedUrl, true /* userInitiated */, true /* added */);
}

void GeoZoneManager::refreshFeeds()
{
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        if (source->isFeed()) {
            _download(source->url(), true /* userInitiated */);
        }
    }
}

void GeoZoneManager::_refreshDueFeeds()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        if (!source->busy() && source->refreshDue(now)) {
            _download(source->url(), false /* userInitiated */);
        }
    }
}

void GeoZoneManager::_download(const QString &url, bool userInitiated, bool added)
{
    const int generation = ++_generations[url];
    if (GeoZoneSource *const existing = source(url)) {
        existing->setBusy(true);
    }

    qCDebug(GeoZoneManagerLog) << "Downloading" << url;

    const QUrl parsedUrl(url);
    if (parsedUrl.isLocalFile()) {
        // Read in place, the file belongs to the user
        const QFileInfo info(parsedUrl.toLocalFile());
        LoadJob job;
        job.key = url;
        job.url = url;
        job.sourcePath = info.filePath();
        job.storedFileName = GeoZoneStore::newFileName();
        job.loadedAt = QDateTime::currentDateTimeUtc();
        job.fileDate = info.lastModified().toUTC();
        job.userInitiated = userInitiated;
        job.added = added;
        job.generation = generation;
        _startLoad(job, true /* background */);
        return;
    }

    QString etag;
    QString lastModified;
    if (const GeoZoneSource *const existing = source(url); existing && existing->loaded()) {
        etag = existing->etag();
        lastModified = existing->lastModified();
    }

    _setPending(1);
    _downloader->get(url, etag, lastModified, [this, url, generation, userInitiated, added](const GeoZoneDownloader::Result &download) {
        _downloadComplete(download, url, generation, userInitiated, added);
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

void GeoZoneManager::_downloadComplete(const GeoZoneDownloader::Result &download, const QString &url, int generation, bool userInitiated, bool added)
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
        _saveIndex();
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
        if (userInitiated) {
            qgcApp()->showAppMessage(tr("Geozone download from %1 failed. %2").arg(url, error));
        }
        return;
    }

    LoadJob job;
    job.key = url;
    job.url = url;
    job.data = download.data;
    if (QUrl(url).host().endsWith(QStringLiteral("faa.gov")) || (existing && (existing->format() == GeoZoneParser::Format::FaaTfr))) {
        job.formatHint = GeoZoneParser::Format::FaaTfr;
    }
    job.etag = download.etag;
    job.lastModified = download.lastModified;
    job.storedFileName = GeoZoneStore::newFileName();
    job.loadedAt = QDateTime::currentDateTimeUtc();
    job.fileDate = job.loadedAt;
    job.userInitiated = userInitiated;
    job.added = added;
    job.generation = generation;
    _startLoad(job, true /* background */);
}

GeoZoneManager::LoadResult GeoZoneManager::_runLoad(const LoadJob &job, const GeoZoneStore &store)
{
    LoadResult loadResult;

    QByteArray bytes = job.data;
    if (bytes.isEmpty()) {
        QFile file(job.sourcePath);
        if (!file.open(QIODevice::ReadOnly)) {
            loadResult.errorString = tr("Unable to open file %1: %2").arg(job.sourcePath, file.errorString());
            return loadResult;
        }
        if (file.size() > GeoZoneDownloader::defaultMaxBytes) {
            loadResult.errorString = tr("The file %1 is larger than %2.").arg(job.sourcePath, QLocale().formattedDataSize(GeoZoneDownloader::defaultMaxBytes));
            return loadResult;
        }
        bytes = file.readAll();
    }

    QString parseError;
    if (!GeoZoneParser::parse(bytes, loadResult.result, parseError, job.formatHint)) {
        loadResult.errorString = parseError;
        return loadResult;
    }

    QString storeError;
    if (!job.storedFileName.isEmpty() && !store.writeFile(job.storedFileName, bytes, storeError)) {
        qCWarning(GeoZoneManagerLog) << "Unable to store copy of geozone source" << job.key << storeError;
    }

    return loadResult;
}

void GeoZoneManager::_startLoad(LoadJob job, bool background)
{
    if (GeoZoneSource *const existing = source(job.key)) {
        existing->setBusy(true);
    }
    _setPending(1);

    if (!background) {
        _applyLoad(job, _runLoad(job, _store));
        _setPending(-1);
        return;
    }

    QFutureWatcher<LoadResult> *const watcher = new QFutureWatcher<LoadResult>(this);
    (void) connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, job]() {
        _applyLoad(job, watcher->result());
        watcher->deleteLater();
        _setPending(-1);
    });
    watcher->setFuture(QtConcurrent::run(&GeoZoneManager::_runLoad, job, _store));
}

void GeoZoneManager::_applyLoad(const LoadJob &job, const LoadResult &loadResult)
{
    if (_generations.value(job.key) != job.generation) {
        // Removed or loaded again in the meantime
        _store.removeFile(job.storedFileName);
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
        if (job.userInitiated) {
            qgcApp()->showAppMessage(tr("Geozone file load failed. %1").arg(loadResult.errorString));
        }
        return;
    }

    // Live feeds are downloaded often and mostly unchanged, keep their zones so maps and alerts aren't reset
    if (source && source->loaded() && !loadResult.result.contentHash.isEmpty() && (source->contentHash() == loadResult.result.contentHash) &&
        (source->format() == loadResult.result.format)) {
        _setStoredFileName(source, job.storedFileName);
        source->markCurrent(job.loadedAt, loadResult.result.issued);
        source->setErrorString(QString());
        if (source->isFeed() && !job.data.isEmpty()) {
            source->setValidators(job.etag, job.lastModified);
        }
        _saveIndex();
        _updateValidity();
        qCDebug(GeoZoneManagerLog) << "Zones unchanged" << job.key;
        return;
    }

    _beginZonesChange();
    if (!source) {
        source = new GeoZoneSource(job.key, job.url, this);
        _sources->append(source);
    }
    _setStoredFileName(source, job.storedFileName);
    source->setResult(loadResult.result, job.loadedAt, job.fileDate);
    source->setErrorString(QString());
    if (source->isFeed() && !job.data.isEmpty()) {
        source->setValidators(job.etag, job.lastModified);
    }
    _endZonesChange();

    _saveIndex();
    _updateValidity();

    qCDebug(GeoZoneManagerLog) << "Loaded" << source->zoneCount() << source->formatName() << "zones from" << job.key;

    if (job.userInitiated) {
        setVisible(true);
    }
    // Maps move to added sources, which refreshing the feeds must not do
    if (job.added) {
        emit sourceAdded(source);
    }
}

void GeoZoneManager::_setStoredFileName(GeoZoneSource *source, const QString &storedFileName)
{
    if (storedFileName.isEmpty()) {
        return;
    }
    const QString previousStoredFileName = source->storedFileName();
    source->setStoredFileName(storedFileName);
    if (previousStoredFileName != storedFileName) {
        _store.removeFile(previousStoredFileName);
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
            qgcApp()->showAppMessage(tr("Some geozone data may be outdated. Load current files in Application Settings > Geozones.\n\n%1").arg(_validityWarning), tr("Geozones"));
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

    _beginZonesChange();
    (void) _sources->removeAt(index);
    const QString storedFileName = source->storedFileName();
    source->deleteLater();
    _endZonesChange();

    _store.removeFile(storedFileName);
    _saveIndex();
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

void GeoZoneManager::_beginZonesChange()
{
    if (_changingZones) {
        return;
    }
    _changingZones = true;
    emit zonesAboutToChange();
    _activeTimer->stop();
    _zones->clear();
    _zoneList.clear();
    _index.clear();
    _shapes.clear();
    _boundingBox = QGeoRectangle();
}

void GeoZoneManager::_endZonesChange()
{
    QList<QObject*> zones;
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        zones.append(QList<QObject*>(source->zones().cbegin(), source->zones().cend()));
        _zoneList.append(source->zones());
        _shapes.append(source->shapes());
        if (source->boundingBox().isValid()) {
            _boundingBox = _boundingBox.isValid() ? _boundingBox.united(source->boundingBox()) : source->boundingBox();
        }
    }

    _index.build(_zoneList);

    // Appending an empty list would insert an invalid row range
    if (!zones.isEmpty()) {
        _zones->append(zones);
        _activeTimer->start();
    }

    _changingZones = false;
    emit zonesChanged();
}

void GeoZoneManager::loadStoredSources(bool background)
{
    QList<LoadJob> jobs;
    for (const GeoZoneStore::Entry &entry : _store.readIndex()) {
        LoadJob job;
        job.key = entry.key;
        job.url = entry.url;
        job.sourcePath = _store.path(entry.fileName);
        job.loadedAt = entry.loadedAt;
        job.fileDate = entry.fileDate;
        if (QUrl(job.url).host().endsWith(QStringLiteral("faa.gov"))) {
            job.formatHint = GeoZoneParser::Format::FaaTfr;
        }
        job.etag = entry.etag;
        job.lastModified = entry.lastModified;
        if (job.key.isEmpty() || entry.fileName.isEmpty() || !QFile::exists(job.sourcePath) || source(job.key)) {
            qCWarning(GeoZoneManagerLog) << "Skipping stored geozone source" << job.key << entry.fileName;
            continue;
        }
        job.generation = ++_generations[job.key];

        // Keep the order of the sources while they load
        GeoZoneSource *const placeholder = new GeoZoneSource(job.key, job.url, this);
        placeholder->setStoredFileName(entry.fileName);
        placeholder->setStoredDates(job.loadedAt, job.fileDate);
        placeholder->setValidators(job.etag, job.lastModified);
        _sources->append(placeholder);
        jobs.append(job);
    }

    QSet<QString> referenced;
    for (int i = 0; i < _sources->count(); i++) {
        referenced.insert(_sources->value<GeoZoneSource*>(i)->storedFileName());
    }
    _store.removeOrphanFiles(referenced);
    _saveIndex();

    for (const LoadJob &job : std::as_const(jobs)) {
        _startLoad(job, background);
    }
}

void GeoZoneManager::_saveIndex() const
{
    QList<GeoZoneStore::Entry> entries;
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        if (source->storedFileName().isEmpty()) {
            continue;
        }
        GeoZoneStore::Entry entry;
        entry.key = source->key();
        entry.fileName = source->storedFileName();
        entry.loadedAt = source->loadedAt();
        entry.fileDate = source->fileDate();
        if (source->isFeed()) {
            entry.url = source->url();
            entry.etag = source->etag();
            entry.lastModified = source->lastModified();
        }
        entries.append(entry);
    }
    _store.writeIndex(entries);
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
