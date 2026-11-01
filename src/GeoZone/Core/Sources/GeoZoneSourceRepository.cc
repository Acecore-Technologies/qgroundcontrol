/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneSourceRepository.h"
#include "GeoZoneAirac.h"
#include "GeoZoneEnvironment.h"
#include "GeoZonePackExchange.h"
#include "GeoZoneProviders.h"
#include "GeoZoneRefreshScheduler.h"
#include "GeoZoneSource.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QFutureWatcher>
#include <QtCore/QJsonArray>
#include <QtCore/QLocale>
#include <QtCore/QScopeGuard>
#include <QtCore/QUrl>
#include <QtCore/QtMath>

#include <algorithm>
#include <cmath>

GeoZoneSourceRepository::GeoZoneSourceRepository(const QString &storageDir, QObject *parent)
    : QObject(parent)
    , _store(storageDir)
    , _index(_store)
    , _downloader(new GeoZoneDownloader(this))
    , _scheduler(new GeoZoneRefreshScheduler([this]() { return _sources; }, this))
{
    _downloader->setDirectory(_store.downloadDir());

    (void) connect(_scheduler, &GeoZoneRefreshScheduler::refreshDue, this, [this](GeoZoneSource *source) {
        _download(source->url(), GeoZoneLoader::Request::Automatic);
    });

    (void) connect(_downloader, &GeoZoneDownloader::progressChanged, this, &GeoZoneSourceRepository::progressChanged);
}

GeoZoneSourceRepository::~GeoZoneSourceRepository()
{
    // Downloads are aborted, loads still running stop before storing their copy and their results are dropped
    for (const GeoZoneCancelToken &token : std::as_const(_operations)) {
        token.cancel();
    }
    for (GeoZoneFeedFetch *const fetch : std::as_const(_fetches)) {
        fetch->cancel();
    }
}

void GeoZoneSourceRepository::init()
{
    _store.clearDownloads();
    _startupRefreshPending = true;
    _startupCheckPending = true;
    loadStoredSources(LoadMode::Background);
    _scheduler->start();
    if (!busy()) {
        _idle();
    }
}

QList<const GeoZoneSource*> GeoZoneSourceRepository::sourceList() const
{
    return QList<const GeoZoneSource*>(_sources.cbegin(), _sources.cend());
}

GeoZoneSource *GeoZoneSourceRepository::source(const QString &key) const
{
    for (GeoZoneSource *const source : _sources) {
        if (source->key() == key) {
            return source;
        }
    }
    return nullptr;
}

void GeoZoneSourceRepository::addFile(const QString &fileName)
{
    GeoZoneLoader::Job job = GeoZoneLoader::fileJob(fileName);
    job.request = GeoZoneLoader::Request::Add;
    job.cancelToken = _beginOperation(job.key);
    _startLoad(job, LoadMode::Background);
}

bool GeoZoneSourceRepository::loadFile(const QString &fileName, QString &errorString)
{
    GeoZoneLoader::Job job = GeoZoneLoader::fileJob(fileName);
    job.cancelToken = _beginOperation(job.key);
    _prepareJob(job);

    const GeoZoneLoader::Result loadResult = GeoZoneLoader::run(job, _store);
    _applyLoad(job, loadResult);
    if (!loadResult.errorString.isEmpty()) {
        errorString = tr("Geozone file load failed. %1").arg(loadResult.errorString);
        return false;
    }
    errorString.clear();
    if (GeoZoneSource *const loaded = source(job.key)) {
        loaded->setEnabled(true);
    }
    emit userLoaded();
    return true;
}

void GeoZoneSourceRepository::addUrl(const QString &url)
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

void GeoZoneSourceRepository::addService(const QString &url, const GeoZoneService &service, const QString &secret, bool allowOtherOrigins)
{
    const QString trimmedUrl = url.trimmed();
    const QUrl parsedUrl(trimmedUrl);
    if (!parsedUrl.isValid() || !QStringList({ QStringLiteral("http"), QStringLiteral("https") }).contains(parsedUrl.scheme()) || parsedUrl.host().isEmpty()) {
        emit loadFailed(trimmedUrl, tr("Geozone download failed. %1 is not a valid http or https address.").arg(trimmedUrl));
        return;
    }
    GeoZoneService feedService = service;
    if (GeoZoneService::hasAreaPlaceholders(trimmedUrl)) {
        feedService.areaOfInterest = true;
    }
    // The client secret and the area of interest only go to another server the user trusts
    if (const QStringList otherOrigins = feedService.otherOrigins(trimmedUrl); !otherOrigins.isEmpty() && !allowOtherOrigins) {
        emit loadFailed(trimmedUrl, tr("Geozone download from %1 failed. The service sends requests to %2, another server than the feed's, which was not confirmed.")
                                        .arg(trimmedUrl, otherOrigins.join(QStringLiteral(", "))));
        return;
    }
    _setService(trimmedUrl, feedService);
    if (!secret.isEmpty()) {
        _services.setSecret(trimmedUrl, secret);
        _updateCredentialsMissing(trimmedUrl);
    }
    _services.forgetToken(trimmedUrl);
    _download(trimmedUrl, GeoZoneLoader::Request::Add);
}

void GeoZoneSourceRepository::setSecret(const QString &key, const QString &secret)
{
    _services.setSecret(key, secret);
    if (GeoZoneSource *const existing = source(key)) {
        existing->setCredentialsMissing(secret.isEmpty());
        if (existing->isFeed() && !secret.isEmpty()) {
            _download(key, GeoZoneLoader::Request::User);
        }
    }
}

void GeoZoneSourceRepository::_setService(const QString &key, const GeoZoneService &service)
{
    _services.setService(key, service);
    if (GeoZoneSource *const existing = source(key)) {
        existing->setService(service);
    }
    _updateCredentialsMissing(key);
    if (_index.setService(key, service.isEmpty() ? QJsonObject() : service.toJson())) {
        _saveIndex();
    }
}

void GeoZoneSourceRepository::_updateCredentialsMissing(const QString &key)
{
    if (GeoZoneSource *const existing = source(key)) {
        existing->setCredentialsMissing(_services.credentialsMissing(key));
    }
}

void GeoZoneSourceRepository::setMissionArea(quintptr owner, const QGeoRectangle &area)
{
    if (!area.isValid()) {
        (void) _missionAreas.remove(owner);
        return;
    }
    if (_missionAreas.value(owner) != area) {
        _missionAreas.insert(owner, area);
        _checkAreas();
    }
}

void GeoZoneSourceRepository::setVehiclePosition(const QGeoCoordinate &position)
{
    if (position.isValid() && (std::abs(position.latitude()) <= 90) && (std::abs(position.longitude()) <= 180) &&
        !((position.latitude() == 0) && (position.longitude() == 0))) {
        _vehiclePosition = QGeoCoordinate(position.latitude(), position.longitude());
        _checkAreas();
    } else {
        _vehiclePosition = QGeoCoordinate();
    }
}

QGeoRectangle GeoZoneSourceRepository::areaOfInterest() const
{
    QList<QGeoCoordinate> coordinates;
    for (const QGeoRectangle &area : _missionAreas) {
        coordinates << area.topLeft() << area.bottomRight();
    }
    if (_vehiclePosition.isValid()) {
        coordinates.append(_vehiclePosition);
    }
    return coordinates.isEmpty() ? QGeoRectangle() : QGeoRectangle(coordinates);
}

void GeoZoneSourceRepository::setAreaMargin(double meters)
{
    _areaMargin = std::clamp(meters, 0.0, 100000.0);
}

void GeoZoneSourceRepository::setFollowArea(bool follow)
{
    if (follow != _followArea) {
        _followArea = follow;
        _checkAreas();
    }
}

QGeoRectangle GeoZoneSourceRepository::expanded(const QGeoRectangle &area, double margin)
{
    if (!area.isValid()) {
        return QGeoRectangle();
    }
    constexpr double metersPerDegree = 111320.0;
    const double latitudeMargin = margin / metersPerDegree;
    const double maxLatitude = (std::max)(std::abs(area.topLeft().latitude()), std::abs(area.bottomRight().latitude()));
    const double longitudeMargin = margin / (metersPerDegree * (std::max)(0.01, std::cos(qDegreesToRadians((std::min)(maxLatitude + latitudeMargin, 89.0)))));
    const double north = (std::min)(90.0, area.topLeft().latitude() + latitudeMargin);
    const double south = (std::max)(-90.0, area.bottomRight().latitude() - latitudeMargin);
    double west = area.topLeft().longitude() - longitudeMargin;
    double east = area.bottomRight().longitude() + longitudeMargin;
    if ((area.width() + (2 * longitudeMargin)) >= 360) {
        west = -180;
        east = 180;
    } else {
        // Areas reaching across the antimeridian continue on the other side
        west = (west < -180) ? (west + 360) : west;
        east = (east > 180) ? (east - 360) : east;
    }
    return QGeoRectangle(QGeoCoordinate(north, west), QGeoCoordinate(south, east));
}

QGeoRectangle GeoZoneSourceRepository::_requestArea(const QString &key) const
{
    const GeoZoneSource *const existing = source(key);
    const QGeoRectangle downloaded = existing ? existing->area() : QGeoRectangle();
    const QGeoRectangle interest = areaOfInterest();
    if (!interest.isValid() || (downloaded.isValid() && (!_followArea || downloaded.contains(interest)))) {
        return downloaded;
    }
    // Services answering areas up to a size get a smaller margin, a little under their limit
    const GeoZoneService service = _services.service(key);
    const QGeoRectangle area = expanded(interest, _areaMargin);
    if ((service.maxAreaKm2 <= 0) || (GeoZoneService::areaKm2(area) <= (0.95 * service.maxAreaKm2))) {
        return area;
    }
    double low = 0;
    double high = _areaMargin;
    for (int i = 0; i < 20; i++) {
        const double margin = (low + high) / 2;
        if (GeoZoneService::areaKm2(expanded(interest, margin)) <= (0.95 * service.maxAreaKm2)) {
            low = margin;
        } else {
            high = margin;
        }
    }
    return expanded(interest, low);
}

void GeoZoneSourceRepository::_checkAreas()
{
    const QGeoRectangle interest = areaOfInterest();
    if (!_followArea || !interest.isValid()) {
        return;
    }
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    for (GeoZoneSource *const source : std::as_const(_sources)) {
        if (!source->isFeed() || !source->service().areaOfInterest || !source->enabled() || source->busy() || source->credentialsMissing() ||
            (source->area().isValid() && source->area().contains(interest))) {
            continue;
        }
        // Services which failed aren't asked again before their retry
        if (const QDateTime retryAt = _scheduler->retryAt(source->key()); retryAt.isValid() && (retryAt > now)) {
            continue;
        }
        qCDebug(GeoZoneSourceLog) << "The area of interest left the area of" << source->key();
        _download(source->key(), GeoZoneLoader::Request::Automatic);
    }
}

void GeoZoneSourceRepository::refreshFeeds()
{
    for (int i = 0; i < _sources.count(); i++) {
        const GeoZoneSource *const source = _sources[i];
        if (source->isFeed()) {
            _download(source->url(), GeoZoneLoader::Request::User);
        }
    }
}

void GeoZoneSourceRepository::_download(const QString &url, GeoZoneLoader::Request request)
{
    GeoZoneSource *const existing = source(url);
    if (existing) {
        existing->setBusy(true);
    }

    if (QUrl(url).isLocalFile()) {
        qCDebug(GeoZoneSourceLog) << "Reading" << url;
        GeoZoneLoader::Job job = GeoZoneLoader::feedJob(url);
        job.request = request;
        job.cancelToken = _beginOperation(url);
        _startLoad(job, LoadMode::Background);
        return;
    }

    GeoZoneFeedFetch::Request fetchRequest;
    fetchRequest.url = url;
    fetchRequest.formatHint = GeoZoneProviders::formatHint(url);
    fetchRequest.companions = _companions.value(url);
    fetchRequest.service = _services.service(url);
    fetchRequest.time = GeoZoneEnvironment::currentDateTimeUtc();
    if (fetchRequest.service.needsSecret()) {
        fetchRequest.secret = _services.secret(url);
    }
    fetchRequest.accessToken = _services.token(url, fetchRequest.time);
    if (fetchRequest.service.areaOfInterest) {
        fetchRequest.area = _requestArea(url);
    }
    if (existing && existing->loaded()) {
        // The validators are those of the area downloaded
        if (!fetchRequest.service.areaOfInterest || (existing->area() == fetchRequest.area)) {
            fetchRequest.etag = existing->etag();
            fetchRequest.lastModified = existing->lastModified();
        }
        fetchRequest.datasetUrl = existing->datasetUrl();
        fetchRequest.formatHint = existing->format();
    }

    // Counted first, so that replacing a fetch in progress doesn't make the repository idle
    _setPending(1);
    GeoZoneFeedFetch *const fetch = new GeoZoneFeedFetch(_downloader, fetchRequest, _beginOperation(url), this);
    _fetches.insert(url, fetch);
    (void) connect(fetch, &GeoZoneFeedFetch::finished, this, [this, fetch, request](const GeoZoneFeedFetch::Result &result) {
        _fetchFinished(fetch, result, request);
    });
    fetch->start();
}

double GeoZoneSourceRepository::progress() const
{
    // Parsing has no progress to report
    return _downloader->progress();
}

QString GeoZoneSourceRepository::progressText() const
{
    if (_downloader->count() == 0) {
        return busy() ? tr("Loading geozones...") : QString();
    }
    return _downloader->progressText();
}

void GeoZoneSourceRepository::_fetchFinished(GeoZoneFeedFetch *fetch, const GeoZoneFeedFetch::Result &result, GeoZoneLoader::Request request)
{
    const QString url = fetch->request().url;
    (void) _fetches.remove(url);
    fetch->deleteLater();
    const auto done = qScopeGuard([this]() { _setPending(-1); });

    GeoZoneSource *const existing = source(url);
    if (!result.accessToken.isEmpty()) {
        _services.setToken(url, result.accessToken, result.tokenExpiresAt);
    }
    if (result.status == GeoZoneFeedFetch::Result::Status::Ready) {
        GeoZoneLoader::Job job = GeoZoneLoader::downloadJob(fetch->request(), result);
        job.request = request;
        job.cancelToken = fetch->cancelToken();
        _startLoad(job, LoadMode::Background);
        return;
    }

    _endOperation(url, fetch->cancelToken());
    if ((result.status == GeoZoneFeedFetch::Result::Status::NotModified) && existing) {
        qCDebug(GeoZoneSourceLog) << "Feed not modified" << url;
        const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
        existing->setCheckedAt(now);
        existing->setBusy(false);
        existing->setErrorString(QString());
        _scheduler->refreshSucceeded(url, result.maxAgeSecs);
        if (_index.confirm(url, existing->etag(), existing->lastModified(), now, result.maxAgeSecs)) {
            _saveIndex();
        }
        updateValidity();
        return;
    }

    const QString error = result.errorString.isEmpty() ? tr("The server responded with status %1.").arg(304) : result.errorString;
    qCWarning(GeoZoneSourceLog) << "Download failed" << url << error;
    if (existing) {
        existing->setBusy(false);
        existing->setErrorString(error);
        existing->setIntegrityFailed(result.integrity == GeoZoneProvenance::Integrity::Failed);
        _scheduler->refreshFailed(*existing);
    }
    if (!existing) {
        // The service of a feed which couldn't be added is forgotten with its secret
        _services.forget(url);
    }
    if (request != GeoZoneLoader::Request::Automatic) {
        emit loadFailed(url, tr("Geozone download from %1 failed. %2").arg(url, error));
    }
}

GeoZoneCancelToken GeoZoneSourceRepository::_beginOperation(const QString &key)
{
    _cancelOperation(key);
    const GeoZoneCancelToken token;
    _operations.insert(key, token);
    return token;
}

void GeoZoneSourceRepository::_endOperation(const QString &key, const GeoZoneCancelToken &token)
{
    if (const auto it = _operations.constFind(key); (it != _operations.cend()) && (it.value() == token)) {
        (void) _operations.erase(it);
    }
}

void GeoZoneSourceRepository::_cancelOperation(const QString &key)
{
    if (const auto it = _operations.constFind(key); it != _operations.cend()) {
        it->cancel();
        (void) _operations.erase(it);
    }
    if (GeoZoneFeedFetch *const fetch = _fetches.take(key)) {
        fetch->cancel();
        fetch->deleteLater();
        _setPending(-1);
    }
}

void GeoZoneSourceRepository::_prepareJob(GeoZoneLoader::Job &job) const
{
    if (_index.readOnly()) {
        job.storedFileName.clear();
    }
    // Read here, the default locale changes on this thread with the language setting
    job.language = QLocale().name().left(2).toLower();
}

void GeoZoneSourceRepository::_startLoad(GeoZoneLoader::Job job, LoadMode mode)
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

void GeoZoneSourceRepository::_applyLoad(const GeoZoneLoader::Job &job, const GeoZoneLoader::Result &loadResult)
{
    if (job.cancelToken.isCancelled() || loadResult.cancelled) {
        // Removed or loaded again in the meantime
        if (loadResult.stored) {
            _store.removeFile(job.storedFileName);
        }
        return;
    }
    _endOperation(job.key, job.cancelToken);

    GeoZoneSource *source = this->source(job.key);
    if (source) {
        source->setBusy(false);
    }

    if (!loadResult.errorString.isEmpty()) {
        qCWarning(GeoZoneSourceLog) << "Load failed" << job.key << loadResult.errorString;
        if (source) {
            source->setErrorString(loadResult.errorString);
            _scheduler->refreshFailed(*source);
        } else {
            _services.forget(job.key);
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
        qCDebug(GeoZoneSourceLog) << "Zones unchanged" << job.key;
    } else {
        if (!source) {
            source = _newSource(job.key, job.url);
            _removeLegacyFileSource(job);
        }
        if (_services.contains(job.key)) {
            source->setService(_services.service(job.key));
            source->setCredentialsMissing(_services.credentialsMissing(job.key));
        }
        emit sourceAboutToChange(source);
        source->setResult(loadResult.result, job.loadedAt, job.fileDate);
        emit sourceChanged(source);
        qCDebug(GeoZoneSourceLog) << "Loaded" << source->zoneCount() << source->formatName() << "zones from" << job.key;
    }
    if (job.checkedAt > job.loadedAt) {
        source->setCheckedAt(job.checkedAt);
    }
    if (!job.fromStore) {
        GeoZoneProvenance provenance = job.provenance;
        if (provenance.isEmpty()) {
            provenance.requestUrl = job.url.isEmpty() ? QUrl::fromLocalFile(job.key).toString() : job.url;
            provenance.fetchedAt = job.loadedAt;
        }
        provenance.contentHash = loadResult.contentHash;
        provenance.size = loadResult.contentSize;
        source->setProvenance(provenance);
        source->setIntegrityFailed(false);
    }
    source->setErrorString(QString());
    _scheduler->refreshSucceeded(job.key, job.maxAgeSecs);
    // Companions of the stored copy are reused for the next download
    if (!loadResult.result.companions.isEmpty()) {
        _companions.insert(job.key, loadResult.result.companions);
    }
    if (source->isFeed() && job.downloaded) {
        source->setValidators(job.etag, job.lastModified);
        source->setDatasetUrl(job.datasetUrl);
        source->setArea(job.area);
    }

    _updateStoredEntry(job, loadResult);
    // Adding a disabled source again shows it
    if (job.request == GeoZoneLoader::Request::Add) {
        source->setEnabled(true);
    }
    updateValidity();

    // The area of interest may have moved while the feed was downloaded
    if (source->service().areaOfInterest && job.downloaded) {
        _checkAreas();
    }

    if (unchanged) {
        return;
    }
    if (job.request != GeoZoneLoader::Request::Automatic) {
        emit userLoaded();
    }
    // Maps move to added sources, which refreshing the feeds must not do
    if (job.request == GeoZoneLoader::Request::Add) {
        emit sourceAdded(source);
    }
}

void GeoZoneSourceRepository::_updateStoredEntry(const GeoZoneLoader::Job &job, const GeoZoneLoader::Result &loadResult)
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

    GeoZoneSourceIndex::Entry entry = GeoZoneLoader::indexEntry(job, _index.entry(job.key));
    if (const GeoZoneSource *const source = this->source(job.key)) {
        entry.enabled = source->enabled();
        entry.provenance = source->provenance().toJson();
        entry.changes = source->changes().toJson();
    }
    entry.service = _services.contains(job.key) ? _services.service(job.key).toJson() : QJsonObject();
    _index.replace(entry);
    _saveIndex();
}

void GeoZoneSourceRepository::_reportStorageError(const QString &message)
{
    qCWarning(GeoZoneSourceLog) << message;
    if (!_storageErrorReported) {
        _storageErrorReported = true;
        emit storageFailed(message);
    }
}

void GeoZoneSourceRepository::_setPending(int delta)
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

void GeoZoneSourceRepository::_idle()
{
    if (_startupRefreshPending) {
        _startupRefreshPending = false;
        _scheduler->check();
        if (busy()) {
            return;
        }
    }

    if (_startupCheckPending) {
        _startupCheckPending = false;
        updateValidity();
        if (!_validityWarning.isEmpty()) {
            emit outdatedDataFound(tr("Some geozone data may be outdated. Load current files in Application Settings > Geozones.\n\n%1").arg(_validityWarning));
        }
    }
}

void GeoZoneSourceRepository::_removeLegacyFileSource(const GeoZoneLoader::Job &job)
{
    // Earlier versions keyed files by their name, so the file loaded again from its folder replaces that source
    if (!job.url.isEmpty()) {
        return;
    }
    const QString legacyKey = QFileInfo(job.key).fileName();
    for (int i = 0; i < _sources.count(); i++) {
        const GeoZoneSource *const legacy = _sources[i];
        if (!legacy->isFeed() && (legacy->key() == legacyKey) && (legacy->key() != job.key)) {
            removeSource(i);
            return;
        }
    }
}

void GeoZoneSourceRepository::removeSource(int index)
{
    if ((index < 0) || (index >= _sources.count())) {
        return;
    }

    GeoZoneSource *const source = _sources[index];
    // Drops results of loads and downloads in progress
    _cancelOperation(source->key());
    if (source->busy()) {
        source->setBusy(false);
    }
    _companions.remove(source->key());
    _services.forget(source->key());
    _scheduler->remove(source->key());
    emit sourceAboutToChange(source);
    _sources.removeAt(index);
    emit sourceRemoved(index);
    emit sourceChanged(source);
    source->deleteLater();

    if (!_index.readOnly()) {
        _index.remove(source->key());
        _saveIndex();
    }
    updateValidity();
}

void GeoZoneSourceRepository::clear()
{
    while (!_sources.isEmpty()) {
        removeSource(_sources.count() - 1);
    }
    // Drops results of new sources still loading
    for (const QString &key : _operations.keys() + _fetches.keys()) {
        _cancelOperation(key);
    }
}

void GeoZoneSourceRepository::loadStoredSources(LoadMode mode)
{
    const GeoZoneSourceIndex::Contents index = _index.read();
    switch (index.status) {
    case GeoZoneSourceIndex::Status::Ok:
    case GeoZoneSourceIndex::Status::Missing:
        break;
    case GeoZoneSourceIndex::Status::Unreadable: {
        qCWarning(GeoZoneSourceLog) << "Unreadable geozone source index" << index.errorString;
        const QString backupDir = _store.moveToBackup();
        _reportStorageError(backupDir.isEmpty() ?
            tr("The list of stored geozone sources could not be read (%1). Add the sources again in Application Settings > Geozones.").arg(index.errorString) :
            tr("The list of stored geozone sources could not be read (%1). The stored files were moved to %2. Add the sources again in Application Settings > Geozones.").arg(index.errorString, backupDir));
        return;
    }
    case GeoZoneSourceIndex::Status::Newer:
        _index.setReadOnly(true);
        _reportStorageError(tr("The geozone sources were saved by a newer version of QGroundControl and are not loaded. Sources added now will not be kept."));
        return;
    }

    QList<GeoZoneLoader::Job> jobs;
    for (const GeoZoneSourceIndex::Entry &entry : index.entries) {
        GeoZoneLoader::Job job = GeoZoneLoader::storedJob(entry, _store.path(entry.fileName));
        if (job.key.isEmpty() || entry.fileName.isEmpty() || !QFile::exists(job.sourcePath) || source(job.key)) {
            qCWarning(GeoZoneSourceLog) << "Skipping stored geozone source" << job.key << entry.fileName;
            continue;
        }
        job.cancelToken = _beginOperation(job.key);

        // Keep the order of the sources while they load
        GeoZoneSource *const placeholder = _newSource(job.key, job.url, entry.enabled);
        placeholder->setStoredDates(job.loadedAt, job.fileDate, job.checkedAt);
        placeholder->setValidators(job.etag, job.lastModified);
        placeholder->setDatasetUrl(job.datasetUrl);
        placeholder->setArea(entry.area);
        placeholder->setProvenance(GeoZoneProvenance::fromJson(entry.provenance));
        placeholder->setChanges(GeoZoneSourceChanges::fromJson(entry.changes));
        if (!entry.service.isEmpty()) {
            _services.restoreService(entry.key, GeoZoneService::fromJson(entry.service));
            placeholder->setService(_services.service(entry.key));
            placeholder->setCredentialsMissing(_services.credentialsMissing(entry.key));
        }
        _index.insert(entry);
        jobs.append(job);
    }

    _index.removeUnreferencedFiles();
    _saveIndex();

    for (const GeoZoneLoader::Job &job : std::as_const(jobs)) {
        _startLoad(job, mode);
    }
}

GeoZoneSource *GeoZoneSourceRepository::_newSource(const QString &key, const QString &url, bool enabled)
{
    GeoZoneSource *const source = new GeoZoneSource(key, url, this);
    source->setEnabled(enabled);
    (void) connect(source, &GeoZoneSource::enabledChanged, this, [this, source]() { _sourceEnabledChanged(source); });
    _sources.append(source);
    emit sourceAppended(source);
    return source;
}

void GeoZoneSourceRepository::_sourceEnabledChanged(GeoZoneSource *source)
{
    // The zones of the source join or leave the maps and checks
    emit sourceAboutToChange(source);
    emit sourceChanged(source);
    if (_index.setEnabled(source->key(), source->enabled())) {
        _saveIndex();
    }
    updateValidity();
}

void GeoZoneSourceRepository::_saveIndex()
{
    QStringList keys;
    for (const GeoZoneSource *const source : std::as_const(_sources)) {
        keys.append(source->key());
    }
    QString errorString;
    if (!_index.save(keys, errorString)) {
        _reportStorageError(tr("The list of geozone sources could not be saved, sources may be missing at the next start. %1").arg(errorString));
    }
}

QJsonObject GeoZoneSourceRepository::sourceReport(const GeoZoneSource &source)
{
    const auto time = [](const QDateTime &dateTime) { return dateTime.toUTC().toString(Qt::ISODate); };
    QJsonObject report{
        { QStringLiteral("key"), source.key() },
        { QStringLiteral("name"), source.name() },
        { QStringLiteral("format"), source.formatName() },
        { QStringLiteral("zoneCount"), source.zoneCount() },
        { QStringLiteral("enabled"), source.enabled() },
    };
    if (source.isFeed()) {
        report.insert(QStringLiteral("url"), source.url());
    }
    if (!source.provider().isEmpty()) {
        report.insert(QStringLiteral("provider"), source.provider());
    }
    const std::pair<const char*, QDateTime> dates[] = {
        { "dataDate", source.dataDate() }, { "validFrom", source.validFrom() }, { "validTo", source.validTo() },
        { "loadedAt", source.loadedAt() }, { "checkedAt", source.checkedAt() },
    };
    for (const auto &[name, date] : dates) {
        if (date.isValid()) {
            report.insert(QLatin1String(name), time(date));
        }
    }
    if (!source.service().isEmpty()) {
        // The user of a service, like an OAuth2 client id, is left out with the secrets and the headers
        QJsonObject service = source.service().shareableJson();
        service.remove(QStringLiteral("user"));
        report.insert(QStringLiteral("service"), service);
    }
    if (!source.provenance().isEmpty()) {
        report.insert(QStringLiteral("provenance"), source.provenance().toJson());
    }
    if (source.integrityFailed()) {
        report.insert(QStringLiteral("integrityFailed"), true);
    }
    if (!source.changes().isEmpty()) {
        report.insert(QStringLiteral("lastChanges"), source.changes().toJson());
    }
    if (const QDate airac = source.airacDate(); airac.isValid()) {
        report.insert(QStringLiteral("airac"), QJsonObject{
            { QStringLiteral("cycle"), GeoZoneAirac::cycleName(airac) },
            { QStringLiteral("effective"), GeoZoneAirac::cycleStart(airac).toString(Qt::ISODate) },
            { QStringLiteral("ends"), GeoZoneAirac::nextCycleStart(airac).toString(Qt::ISODate) },
        });
    }
    return report;
}

QJsonObject GeoZoneSourceRepository::provenanceReport() const
{
    QJsonArray sources;
    for (const GeoZoneSource *const source : _sources) {
        sources.append(sourceReport(*source));
    }
    return QJsonObject{
        { QStringLiteral("generatedAt"), GeoZoneEnvironment::currentDateTimeUtc().toString(Qt::ISODate) },
        { QStringLiteral("sources"), sources },
    };
}

bool GeoZoneSourceRepository::exportPack(const QStringList &keys, const QString &packPath, QString &errorString) const
{
    QList<GeoZonePackExchange::ExportSource> sources;
    for (const QString &key : keys) {
        const GeoZoneSource *const source = this->source(key);
        const GeoZoneSourceIndex::Entry entry = _index.entry(key);
        if (source && source->loaded() && !entry.fileName.isEmpty()) {
            sources.append(GeoZonePackExchange::ExportSource{ sourceReport(*source), entry, source->service(), _store.path(entry.fileName) });
        }
    }
    GeoZonePackExchange::Signer signer;
    if (!_packSigningKeyFile.isEmpty()) {
        QFile keyFile(_packSigningKeyFile);
        if (!keyFile.open(QIODevice::ReadOnly)) {
            errorString = tr("The pack signing key %1 could not be read: %2").arg(_packSigningKeyFile, keyFile.errorString());
            return false;
        }
        signer.privateKeyPem = keyFile.readAll();
        signer.publisher = _packPublisher;
    }
    return GeoZonePackExchange::write(sources, packPath, errorString, signer);
}

void GeoZoneSourceRepository::setPackSigner(const QString &privateKeyFile, const QString &publisher)
{
    _packSigningKeyFile = privateKeyFile;
    _packPublisher = publisher.trimmed();
}

void GeoZoneSourceRepository::setTrustedPublishers(const QList<GeoZonePackSignature::Publisher> &publishers)
{
    _trustedPublishers = publishers;
    // The signatures were checked when the packs were imported
    bool indexChanged = false;
    for (GeoZoneSource *const source : std::as_const(_sources)) {
        GeoZoneProvenance provenance = source->provenance();
        if (!provenance.imported || ((provenance.packSignature != GeoZoneProvenance::PackSignature::Untrusted) &&
                                     (provenance.packSignature != GeoZoneProvenance::PackSignature::Signed))) {
            continue;
        }
        const GeoZonePackSignature::Publisher *const publisher = GeoZonePackSignature::find(publishers, provenance.packPublicKey);
        const GeoZoneProvenance::PackSignature signature = publisher ? GeoZoneProvenance::PackSignature::Signed : GeoZoneProvenance::PackSignature::Untrusted;
        const QString signer = publisher ? publisher->name : provenance.packSigner;
        if ((signature != provenance.packSignature) || (signer != provenance.packSigner)) {
            provenance.packSignature = signature;
            provenance.packSigner = signer;
            source->setProvenance(provenance);
            indexChanged = _index.setProvenance(source->key(), provenance.toJson()) || indexChanged;
        }
    }
    if (indexChanged) {
        _saveIndex();
    }
}

int GeoZoneSourceRepository::importPack(const QString &packPath, QString &errorString, QString *signatureText)
{
    // Sources already here, or being added, keep their service
    const GeoZonePackExchange::Import import = GeoZonePackExchange::read(packPath, _store.downloadDir(), [this](const QString &key) {
        return source(key) || _services.contains(key);
    }, GeoZoneEnvironment::currentDateTimeUtc(), _trustedPublishers);
    if (!import.errorString.isEmpty()) {
        errorString = import.errorString;
        return -1;
    }

    for (const GeoZonePackExchange::ImportSource &imported : import.sources) {
        if (!imported.known) {
            // The secret of a new source isn't in the pack, the source shows that it is missing
            _setService(imported.job.key, imported.service);
        }
        GeoZoneLoader::Job job = imported.job;
        job.request = GeoZoneLoader::Request::User;
        job.cancelToken = _beginOperation(job.key);
        _startLoad(job, LoadMode::Background);
    }
    qCDebug(GeoZoneSourceLog) << "Importing" << import.sources.count() << "sources from" << packPath
                              << GeoZoneProvenance::packSignatureToString(import.signature) << import.signer;
    if (signatureText) {
        switch (import.signature) {
        case GeoZoneProvenance::PackSignature::None:
        case GeoZoneProvenance::PackSignature::Unsigned:
            *signatureText = tr("The pack is not signed.");
            break;
        case GeoZoneProvenance::PackSignature::Unverified:
            *signatureText = tr("The pack is signed, but this build can't check signatures.");
            break;
        case GeoZoneProvenance::PackSignature::Untrusted:
            *signatureText = tr("The pack is signed by \"%1\" with the key %2, which is not trusted.")
                                 .arg(import.signer, GeoZonePackSignature::keyId(import.publicKey));
            break;
        case GeoZoneProvenance::PackSignature::Signed:
            *signatureText = tr("The pack is signed by %1.").arg(import.signer);
            break;
        }
    }
    errorString = import.errors.join(QLatin1Char('\n'));
    return static_cast<int>(import.sources.count());
}

void GeoZoneSourceRepository::updateValidity()
{
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    QStringList lines;
    for (GeoZoneSource *const source : std::as_const(_sources)) {
        source->updateValidity(now);
        if (source->enabled() && !source->validityText().isEmpty()) {
            lines.append(QStringLiteral("%1: %2").arg(source->name(), source->validityText()));
        }
    }

    const QString warning = lines.join(QStringLiteral("\n"));
    if (warning != _validityWarning) {
        _validityWarning = warning;
        emit validityWarningChanged();
    }
}
