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
#include <QtCore/QFutureWatcher>
#include <QtCore/QLocale>
#include <QtCore/QScopeGuard>
#include <QtCore/QUrl>

#include <cmath>

GeoZoneSourceRepository::GeoZoneSourceRepository(const QString &storageDir, QObject *parent)
    : QObject(parent)
    , _store(storageDir)
    , _index(_store)
    , _downloader(new GeoZoneDownloader(this))
    , _scheduler(new GeoZoneRefreshScheduler([this]() { return _sources; }, this))
    , _feedCatalogue(new GeoZoneFeedCatalogueLoader(storageDir, _downloader, this))
{
    _downloader->setDirectory(_store.downloadDir());
    // The catalogue file and the catalogue URL are checked with the feeds, the next checks shown follow the settings
    (void) connect(_scheduler, &GeoZoneRefreshScheduler::checked, this, [this]() {
        _feedCatalogue->check();
        _updateNextChecks();
    });
    (void) connect(_scheduler, &GeoZoneRefreshScheduler::networkChanged, this, &GeoZoneSourceRepository::_updateNextChecks);
    (void) connect(_scheduler, &GeoZoneRefreshScheduler::networkRestored, _feedCatalogue, &GeoZoneFeedCatalogueLoader::retryUnreachable);

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
    _feedCatalogue->init();
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

void GeoZoneSourceRepository::addFiles(const QStringList &fileNames)
{
    if (fileNames.count() == 1) {
        addFile(fileNames.constFirst());
        return;
    }
    if (fileNames.isEmpty()) {
        return;
    }
    const quint32 batchId = ++_lastBatchId;
    _fileBatches.insert(batchId, FileBatch{ static_cast<int>(fileNames.count()), 0, QStringList() });
    for (const QString &fileName : fileNames) {
        GeoZoneLoader::Job job = GeoZoneLoader::fileJob(fileName);
        job.request = GeoZoneLoader::Request::Add;
        job.batchId = batchId;
        job.cancelToken = _beginOperation(job.key);
        _startLoad(job, LoadMode::Background);
    }
}

void GeoZoneSourceRepository::restoreFile(const QString &fileName)
{
    GeoZoneLoader::Job job = GeoZoneLoader::fileJob(fileName);
    job.cancelToken = _beginOperation(job.key);
    _startLoad(job, LoadMode::Background);
}

void GeoZoneSourceRepository::_batchLoaded(const GeoZoneLoader::Job &job, const GeoZoneLoader::Result &loadResult)
{
    const auto it = _fileBatches.find(job.batchId);
    if (it == _fileBatches.end()) {
        return;
    }
    // Files removed or listed again while loading count neither as added nor as failed
    if (!job.cancelToken.isCancelled() && !loadResult.cancelled) {
        if (loadResult.errorString.isEmpty()) {
            it->added++;
        } else {
            it->failures.append(tr("%1: %2").arg(QFileInfo(job.sourcePath).fileName(), loadResult.errorString));
        }
    }
    if (--it->pending > 0) {
        return;
    }
    const FileBatch batch = *it;
    _fileBatches.erase(it);
    emit filesAdded(batch.added, batch.failures);
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
    // All other servers of the service, for callers which don't name those the user confirmed
    addService(url, service, secret, allowOtherOrigins ? service.otherOrigins(url.trimmed()) : QStringList());
}

void GeoZoneSourceRepository::addService(const QString &url, const GeoZoneService &service, const QString &secret, const QStringList &confirmedOrigins)
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
    // The client secret and the area of interest only go to the other servers the user confirmed, by name: a
    // confirmation given for other addresses than the service has now doesn't count
    QStringList unconfirmedOrigins = feedService.otherOrigins(trimmedUrl);
    (void) unconfirmedOrigins.removeIf([&confirmedOrigins](const QString &origin) { return confirmedOrigins.contains(origin); });
    if (!unconfirmedOrigins.isEmpty()) {
        emit loadFailed(trimmedUrl, tr("Geozone download from %1 failed. The service sends requests to %2, another server than the feed's, which was not confirmed.")
                                        .arg(trimmedUrl, unconfirmedOrigins.join(QStringLiteral(", "))));
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
    // Nothing is kept for a key without a service sending a secret, whose source then misses none
    if (!_services.setSecret(key, secret)) {
        return;
    }
    _updateCredentialsMissing(key);
    if (const GeoZoneSource *const existing = source(key); existing && existing->isFeed() && !secret.isEmpty()) {
        _download(key, GeoZoneLoader::Request::User);
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

void GeoZoneSourceRepository::setMissionArea(const void *owner, const QGeoRectangle &area)
{
    // Without the mission, a service answering small areas may answer the rest of the area of interest again
    const bool removed = !area.isValid() && _areaOfInterest.hasMissionArea(owner);
    if (_areaOfInterest.setMissionArea(owner, area) || removed) {
        _checkAreas();
    }
}

void GeoZoneSourceRepository::setVehiclePosition(const QGeoCoordinate &position, double groundSpeed, double course)
{
    const bool had = _areaOfInterest.hasVehicle();
    if (_areaOfInterest.setVehiclePosition(position, groundSpeed, course) || had) {
        _checkAreas();
    }
}

void GeoZoneSourceRepository::setViewArea(const QGeoRectangle &area)
{
    if (_areaOfInterest.setViewArea(area) && _areaOfInterest.followView()) {
        _checkAreas();
    }
}

void GeoZoneSourceRepository::setFollowArea(bool follow)
{
    if (follow != _areaOfInterest.follow()) {
        _areaOfInterest.setFollow(follow);
        _checkAreas();
    }
}

void GeoZoneSourceRepository::setFollowView(bool follow)
{
    if (_areaOfInterest.setFollowView(follow)) {
        _checkAreas();
    }
}

void GeoZoneSourceRepository::_checkAreas()
{
    const bool follow = _areaOfInterest.follow() && areaOfInterest().isValid();
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    for (GeoZoneSource *const source : std::as_const(_sources)) {
        if (!source->isFeed() || !source->service().areaOfInterest) {
            continue;
        }
        // The map view is left out for services answering smaller areas, then the missions, see GeoZoneAreaOfInterest
        const double maxAreaKm2 = source->service().maxAreaKm2;
        QString shortfall;
        if (follow && _areaOfInterest.missionsCut(maxAreaKm2)) {
            const QString areaKm2 = QString::number(std::ceil(GeoZoneService::areaKm2(_areaOfInterest.missionsAndVehicle())), 'f', 0);
            const QString maxKm2 = QString::number(maxAreaKm2, 'f', 0);
            shortfall = _areaOfInterest.hasVehicle() ?
                tr("The area of the mission and the vehicle (%1 km²) is larger than the %2 km² the service answers: only the zones around the vehicle are downloaded.").arg(areaKm2, maxKm2) :
                tr("The area of the mission (%1 km²) is larger than the %2 km² the service answers: only the zones around its center are downloaded.").arg(areaKm2, maxKm2);
        }
        source->setAreaShortfall(shortfall);
        if (!follow || !source->enabled() || source->busy() || source->credentialsMissing()) {
            continue;
        }
        if (const QGeoRectangle interest = _areaOfInterest.interest(maxAreaKm2);
            !interest.isValid() || (source->area().isValid() && source->area().contains(interest))) {
            continue;
        }
        // Services which failed aren't asked again before their retry
        if (const QDateTime retryAt = _scheduler->retryAt(source->key()); retryAt.isValid() && (retryAt > now)) {
            continue;
        }
        qCDebug(GeoZoneSourceLog) << "The area of interest left the area of" << GeoZoneDownloader::redactedUrl(source->key());
        _download(source->key(), GeoZoneLoader::Request::Automatic);
    }
}

bool GeoZoneSourceRepository::_vehicleUncovered(const GeoZoneSource &source) const
{
    if (!source.isFeed() || !source.service().areaOfInterest || !_areaOfInterest.follow() || !_areaOfInterest.hasVehicle()) {
        return false;
    }
    const QGeoRectangle interest = _areaOfInterest.interest(source.service().maxAreaKm2);
    return interest.isValid() && !(source.area().isValid() && source.area().contains(interest));
}

void GeoZoneSourceRepository::setNetwork(GeoZoneRefreshScheduler::Network network)
{
    _scheduler->setNetwork(network);
}

void GeoZoneSourceRepository::_updateNextCheck(GeoZoneSource *source)
{
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    source->setNextCheck(_scheduler->nextRefresh(*source), _scheduler->waitingForNetwork(*source, now));
}

void GeoZoneSourceRepository::_updateNextChecks()
{
    for (GeoZoneSource *const source : std::as_const(_sources)) {
        _updateNextCheck(source);
    }
}

void GeoZoneSourceRepository::refreshFeeds()
{
    _feedCatalogue->check(true);
    for (int i = 0; i < _sources.count(); i++) {
        const GeoZoneSource *const source = _sources[i];
        if (source->isFeed()) {
            _download(source->url(), GeoZoneLoader::Request::User);
        }
    }
}

void GeoZoneSourceRepository::refreshSource(const QString &key)
{
    const GeoZoneSource *const feed = source(key);
    if (feed && feed->isFeed()) {
        _download(feed->url(), GeoZoneLoader::Request::User);
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
        fetchRequest.area = _areaOfInterest.requestArea(existing ? existing->area() : QGeoRectangle(), fetchRequest.service.maxAreaKm2);
    }
    if (existing && existing->loaded()) {
        // The validators are those of the area downloaded. A copy missing zones is downloaded again, not confirmed.
        if ((!fetchRequest.service.areaOfInterest || (existing->area() == fetchRequest.area)) && (existing->missingCount() == 0)) {
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
        qCDebug(GeoZoneSourceLog) << "Feed not modified" << GeoZoneDownloader::redactedUrl(url);
        const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
        existing->setCheckedAt(now);
        existing->setBusy(false);
        existing->setErrorString(QString());
        _scheduler->refreshSucceeded(url, result.maxAgeSecs);
        _updateNextCheck(existing);
        if (_index.confirm(url, existing->etag(), existing->lastModified(), now, result.maxAgeSecs)) {
            _saveIndex();
        }
        updateValidity();
        return;
    }

    const QString error = result.errorString.isEmpty() ? tr("The server responded with status %1.").arg(304) : result.errorString;
    qCWarning(GeoZoneSourceLog) << "Download failed" << GeoZoneDownloader::redactedUrl(url) << error;
    if (existing) {
        existing->setBusy(false);
        existing->setErrorString(error);
        existing->setIntegrityFailed(result.integrity == GeoZoneProvenance::Integrity::Failed);
        _scheduler->refreshFailed(*existing, result.unreachable);
        // The vehicle flies on: the zones around it are asked for again sooner than a feed is retried
        if (_vehicleUncovered(*existing)) {
            _scheduler->retryWithin(existing->key(), areaRetrySecs);
        }
        _updateNextCheck(existing);
    }
    if (!existing) {
        // The service of a feed which couldn't be added is forgotten with its secret
        _services.forget(url);
    }
    if (request != GeoZoneLoader::Request::Automatic) {
        emit loadFailed(url, tr("Geozone download from %1 failed. %2").arg(GeoZoneDownloader::redactedUrl(url), error));
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
        const GeoZoneLoader::Result loadResult = watcher->result();
        _applyLoad(job, loadResult);
        watcher->deleteLater();
        _setPending(-1);
        if (job.batchId != 0) {
            _batchLoaded(job, loadResult);
        }
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
        qCWarning(GeoZoneSourceLog) << "Load failed" << GeoZoneDownloader::redactedUrl(job.key) << loadResult.errorString;
        if (source) {
            source->setErrorString(loadResult.errorString);
            _scheduler->refreshFailed(*source);
            _updateNextCheck(source);
        } else {
            _services.forget(job.key);
        }
        if ((job.request != GeoZoneLoader::Request::Automatic) && (job.batchId == 0)) {
            emit loadFailed(job.key, tr("Geozone file load failed. %1").arg(loadResult.errorString));
        }
        return;
    }

    // Live feeds are downloaded often and mostly unchanged, keep their zones so maps and alerts aren't reset
    const bool unchanged = source && source->loaded() && !loadResult.result.contentHash.isEmpty() &&
                           (source->contentHash() == loadResult.result.contentHash) && (source->format() == loadResult.result.format);
    if (unchanged) {
        source->markCurrent(loadResult.result, job.loadedAt, job.fileDate);
        qCDebug(GeoZoneSourceLog) << "Zones unchanged" << GeoZoneDownloader::redactedUrl(job.key);
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
        qCDebug(GeoZoneSourceLog) << "Loaded" << source->zoneCount() << source->formatName() << "zones from" << GeoZoneDownloader::redactedUrl(job.key);
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
    // A download missing zones, like a WFS response which lost pages, counts as a failed refresh and is retried as one
    const bool incomplete = job.downloaded && source->isFeed() && (source->missingCount() > 0);
    if (incomplete) {
        qCWarning(GeoZoneSourceLog) << "Incomplete download" << GeoZoneDownloader::redactedUrl(job.key) << source->incompleteText();
        _scheduler->refreshFailed(*source);
    } else {
        _scheduler->refreshSucceeded(job.key, job.maxAgeSecs);
    }
    _updateNextCheck(source);
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

    if (incomplete && (job.request != GeoZoneLoader::Request::Automatic)) {
        emit loadFailed(job.key, tr("Geozone download from %1. %2").arg(GeoZoneDownloader::redactedUrl(job.key), source->incompleteText()));
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
    // Drops results of new sources still loading. Their services were set when they were added: forgotten with
    // their secrets and tokens, which would otherwise be used again for a source added at the same address.
    for (const QString &key : _operations.keys() + _fetches.keys()) {
        _cancelOperation(key);
        _companions.remove(key);
        _services.forget(key);
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
        // Before its file is read, and later removed with the source
        if (!GeoZoneSourceIndex::entryValid(entry)) {
            qCWarning(GeoZoneSourceLog) << "Skipping stored geozone source with an invalid entry" << entry.key << entry.url << entry.fileName;
            continue;
        }
        GeoZoneLoader::Job job = GeoZoneLoader::storedJob(entry, _store.path(entry.fileName));
        if (!QFile::exists(job.sourcePath) || source(job.key)) {
            qCWarning(GeoZoneSourceLog) << "Skipping stored geozone source" << GeoZoneDownloader::redactedUrl(job.key) << entry.fileName;
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
    // Builds which can't sign export unsigned packs, also when the settings they share with others name a key
    GeoZonePackExchange::Signer signer;
    if (!_packSigningKeyFile.isEmpty() && GeoZonePackSignature::available()) {
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
            // What the pack said about the integrity of its data goes with the trust in its publisher
            provenance.distrustPackIntegrity();
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
    const GeoZonePackExchange::Import import = GeoZonePackExchange::read(packPath, _store.downloadDir(), [this](const QString &key) {
        // Sources already here, or being added, keep their service
        if (source(key) || _services.contains(key)) {
            return key;
        }
        // Packs leave out a secret typed into the address of a feed: the feed here which has it takes the data
        for (const GeoZoneSource *const candidate : std::as_const(_sources)) {
            if (candidate->isFeed() && (GeoZonePackExchange::shareableUrl(candidate->key()) == key)) {
                return candidate->key();
            }
        }
        return QString();
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
            *signatureText = tr("The pack is signed, but its signature can't be checked in this build, so it is taken as unsigned.");
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
    bool invalid = false;
    QDateTime lastPulledAt;
    for (GeoZoneSource *const source : std::as_const(_sources)) {
        source->updateValidity(now);
        if (!source->enabled()) {
            continue;
        }
        if (!source->validityText().isEmpty()) {
            lines.append(QStringLiteral("%1: %2").arg(source->name(), source->validityText()));
            invalid = invalid || source->invalid();
        }
        // The server confirming the data current counts as a pull
        if (source->loaded() && source->checkedAt().isValid() && (!lastPulledAt.isValid() || (source->checkedAt() > lastPulledAt))) {
            lastPulledAt = source->checkedAt();
        }
    }

    const QString warning = lines.join(QStringLiteral("\n"));
    const QString lastPulledText = lastPulledAt.isValid() ? tr("Data pulled %1").arg(GeoZoneSource::timeText(lastPulledAt, now)) : QString();
    if ((warning != _validityWarning) || (invalid != _dataInvalid) || (lastPulledAt != _lastPulledAt) || (lastPulledText != _lastPulledText)) {
        _validityWarning = warning;
        _dataInvalid = invalid;
        _lastPulledAt = lastPulledAt;
        _lastPulledText = lastPulledText;
        emit validityWarningChanged();
    }
}
