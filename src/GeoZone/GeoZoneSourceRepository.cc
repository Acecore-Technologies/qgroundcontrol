/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneSourceRepository.h"
#include "GeoZoneFormat.h"
#include "GeoZoneManager.h"
#include "GeoZoneProviders.h"
#include "GeoZoneSettings.h"
#include "GeoZoneSource.h"
#include "QmlObjectListModel.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QFutureWatcher>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

GeoZoneSourceRepository::GeoZoneSourceRepository(const QString &storageDir, QObject *parent)
    : QObject(parent)
    , _store(storageDir)
    , _downloader(new GeoZoneDownloader(this))
    , _sources(new QmlObjectListModel(this))
    , _refreshTimer(new QTimer(this))
{
    _downloader->setDirectory(_store.downloadDir());

    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    // The age at which data is outdated may have changed
    (void) connect(settings->staleDays(), &Fact::rawValueChanged, this, &GeoZoneSourceRepository::updateValidity);
    (void) connect(settings->liveStaleHours(), &Fact::rawValueChanged, this, &GeoZoneSourceRepository::updateValidity);

    _refreshTimer->setInterval(_refreshCheckIntervalMSecs);
    (void) connect(_refreshTimer, &QTimer::timeout, this, &GeoZoneSourceRepository::_refreshDueFeeds);

    (void) connect(_downloader, &GeoZoneDownloader::progressChanged, this, &GeoZoneSourceRepository::progressChanged);
}

GeoZoneSourceRepository::~GeoZoneSourceRepository()
{
}

void GeoZoneSourceRepository::init()
{
    _store.clearDownloads();
    _startupRefreshPending = true;
    _startupCheckPending = true;
    loadStoredSources(LoadMode::Background);
    _refreshTimer->start();
    if (!busy()) {
        _idle();
    }
}

QList<const GeoZoneSource*> GeoZoneSourceRepository::sourceList() const
{
    QList<const GeoZoneSource*> sources;
    for (int i = 0; i < _sources->count(); i++) {
        sources.append(_sources->value<GeoZoneSource*>(i));
    }
    return sources;
}

GeoZoneSource *GeoZoneSourceRepository::source(const QString &key) const
{
    for (int i = 0; i < _sources->count(); i++) {
        GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
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
    job.generation = ++_generations[job.key];
    _startLoad(job, LoadMode::Background);
}

bool GeoZoneSourceRepository::loadFile(const QString &fileName, QString &errorString)
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

void GeoZoneSourceRepository::refreshFeeds()
{
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        if (source->isFeed()) {
            _download(source->url(), GeoZoneLoader::Request::User);
        }
    }
}

void GeoZoneSourceRepository::_refreshDueFeeds()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        if (!source->busy() && source->refreshDue(now)) {
            _download(source->url(), GeoZoneLoader::Request::Automatic);
        }
    }
}

void GeoZoneSourceRepository::_download(const QString &url, GeoZoneLoader::Request request)
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

    // The validators of a feed linking to its dataset are the dataset's, its page is read every time since the
    // link depends on the date, like the current AIRAC cycle
    QString etag;
    QString lastModified;
    if (existing && existing->loaded() && existing->datasetUrl().isEmpty()) {
        etag = existing->etag();
        lastModified = existing->lastModified();
    }

    _setPending(1);
    _downloader->get(url, etag, lastModified, [this, url, generation, request](const GeoZoneDownloader::Result &download) {
        _downloadComplete(download, url, generation, request);
        _setPending(-1);
    }, url);
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

void GeoZoneSourceRepository::_downloadComplete(const GeoZoneDownloader::Result &download, const QString &url, int generation, GeoZoneLoader::Request request,
                                                  const QString &datasetUrl)
{
    if (_generations.value(url) != generation) {
        // Removed or downloaded again in the meantime
        if (!download.filePath.isEmpty()) {
            (void) QFile::remove(download.filePath);
        }
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
        updateValidity();
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

    // Pages linking to the current dataset, like the AIRAC cycles of NATS, are followed. Pages are small, datasets aren't read here.
    const QByteArray page = datasetUrl.isEmpty() ? GeoZoneDownloader::readFile(download.filePath, _maxPageBytes) : QByteArray();
    if (const QString linkedUrl = GeoZoneProviders::datasetLink(url, page, QDateTime::currentDateTimeUtc().date()); !linkedUrl.isEmpty()) {
        qCDebug(GeoZoneManagerLog) << "Downloading dataset" << linkedUrl << "linked from" << url;
        (void) QFile::remove(download.filePath);
        // The dataset already downloaded is confirmed by its validators
        QString etag;
        QString lastModified;
        if (existing && existing->loaded() && (existing->datasetUrl() == linkedUrl)) {
            etag = existing->etag();
            lastModified = existing->lastModified();
        }
        _setPending(1);
        _downloader->get(linkedUrl, etag, lastModified, [this, url, generation, request, linkedUrl](const GeoZoneDownloader::Result &dataset) {
            _downloadComplete(dataset, url, generation, request, linkedUrl);
            _setPending(-1);
        }, url);
        return;
    }

    if (_fetchCompanions(download, url, generation, request, datasetUrl)) {
        return;
    }
    _loadDownload(download, url, generation, request, datasetUrl);
}

void GeoZoneSourceRepository::_loadDownload(const GeoZoneDownloader::Result &download, const QString &url, int generation, GeoZoneLoader::Request request,
                                             const QString &datasetUrl)
{
    const GeoZoneSource *const existing = source(url);
    GeoZoneLoader::Job job = GeoZoneLoader::feedJob(url);
    job.sourcePath = download.filePath;
    job.downloaded = true;
    if (existing && (existing->format() == GeoZoneParser::Format::FaaTfr)) {
        job.formatHint = GeoZoneParser::Format::FaaTfr;
    }
    job.etag = download.etag;
    job.lastModified = download.lastModified;
    job.datasetUrl = datasetUrl;
    job.request = request;
    job.generation = generation;
    _startLoad(job, LoadMode::Background);
}

bool GeoZoneSourceRepository::_fetchCompanions(const GeoZoneDownloader::Result &download, const QString &url, int generation, GeoZoneLoader::Request request,
                                                const QString &datasetUrl)
{
    // Feeds with companions, like the FAA TFR list, are small enough to read here
    const QJsonDocument document = QJsonDocument::fromJson(GeoZoneDownloader::readFile(download.filePath, _maxPageBytes));
    if (!document.isObject()) {
        return false;
    }
    const QJsonObject root = document.object();
    const QJsonArray features = root.value(QStringLiteral("features")).toArray();
    const GeoZoneSource *const existing = source(url);
    const GeoZoneParser::Format hint = (existing && existing->loaded()) ? existing->format() : GeoZoneProviders::formatHint(url);
    const GeoZoneFormat *format = nullptr;
    for (const GeoZoneFormat *candidate : GeoZoneFormat::formats()) {
        if (!candidate->isXml() && candidate->detect(root, features, hint)) {
            format = candidate;
            break;
        }
    }
    if (!format) {
        return false;
    }
    const QList<GeoZoneFormat::CompanionRequest> requests = format->companionRequests(QUrl(url), root, features);
    if (requests.isEmpty()) {
        return false;
    }

    struct Fetch {
        QJsonObject root;
        GeoZoneParser::Companions companions;
        int remaining = 0;
    };
    const auto fetch = QSharedPointer<Fetch>::create();
    fetch->root = root;
    const GeoZoneParser::Companions known = _companions.value(url);
    QList<GeoZoneFormat::CompanionRequest> missing;
    for (const GeoZoneFormat::CompanionRequest &companion : requests) {
        const auto it = known.constFind(companion.key);
        if ((it != known.cend()) && (it->version == companion.version)) {
            fetch->companions.insert(companion.key, it.value());
        } else {
            missing.append(companion);
        }
    }

    // The document is stored with its companions, so they are available offline
    const auto finish = [this, fetch, download, url, generation, request, datasetUrl]() {
        if (_generations.value(url) != generation) {
            (void) QFile::remove(download.filePath);
            return;
        }
        QFile file(download.filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            (void) file.write(GeoZoneParser::composite(fetch->root, fetch->companions));
            file.close();
        }
        _loadDownload(download, url, generation, request, datasetUrl);
    };
    if (missing.isEmpty()) {
        finish();
        return true;
    }

    qCDebug(GeoZoneManagerLog) << "Downloading" << missing.count() << "companion documents of" << url;
    fetch->remaining = static_cast<int>(missing.count());
    _setPending(1);
    for (const GeoZoneFormat::CompanionRequest &companion : std::as_const(missing)) {
        _downloader->get(companion.url, QString(), QString(), [this, fetch, companion, finish](const GeoZoneDownloader::Result &result) {
            // A missing companion leaves its zones as conservative as without it
            if (result.status == GeoZoneDownloader::Result::Status::Ok) {
                const QByteArray data = GeoZoneDownloader::readFile(result.filePath, _maxCompanionBytes);
                if (!data.isEmpty()) {
                    fetch->companions.insert(companion.key, GeoZoneParser::Companion{ companion.version, data });
                }
            } else {
                qCWarning(GeoZoneManagerLog) << "Companion download failed" << companion.url << result.errorString;
            }
            if (!result.filePath.isEmpty()) {
                (void) QFile::remove(result.filePath);
            }
            if (--fetch->remaining == 0) {
                finish();
                _setPending(-1);
            }
        }, url);
    }
    return true;
}

void GeoZoneSourceRepository::_prepareJob(GeoZoneLoader::Job &job) const
{
    if (_storeReadOnly) {
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
            source = _newSource(job.key, job.url);
            _removeLegacyFileSource(job);
        }
        emit sourceAboutToChange(source);
        source->setResult(loadResult.result, job.loadedAt, job.fileDate);
        emit sourceChanged(source);
        qCDebug(GeoZoneManagerLog) << "Loaded" << source->zoneCount() << source->formatName() << "zones from" << job.key;
    }
    source->setErrorString(QString());
    // Companions of the stored copy are reused for the next download
    if (!loadResult.result.companions.isEmpty()) {
        _companions.insert(job.key, loadResult.result.companions);
    }
    if (source->isFeed() && job.downloaded) {
        source->setValidators(job.etag, job.lastModified);
        source->setDatasetUrl(job.datasetUrl);
    }

    _updateStoredEntry(job, loadResult);
    // Adding a disabled source again shows it
    if (job.request == GeoZoneLoader::Request::Add) {
        source->setEnabled(true);
    }
    updateValidity();

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

    GeoZoneStore::Entry &entry = _storedEntries[job.key];
    const QString previousFileName = entry.fileName;
    entry.key = job.key;
    entry.url = job.url;
    entry.fileName = job.storedFileName;
    entry.loadedAt = job.loadedAt;
    entry.fileDate = job.fileDate;
    if (const GeoZoneSource *const source = this->source(job.key)) {
        entry.enabled = source->enabled();
    }
    if (job.downloaded) {
        entry.etag = job.etag;
        entry.lastModified = job.lastModified;
        entry.datasetUrl = job.datasetUrl;
    }
    if (previousFileName != entry.fileName) {
        _store.removeFile(previousFileName);
    }
    _saveIndex();
}

void GeoZoneSourceRepository::_reportStorageError(const QString &message)
{
    qCWarning(GeoZoneManagerLog) << message;
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
        _refreshDueFeeds();
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
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const legacy = _sources->value<GeoZoneSource*>(i);
        if (!legacy->isFeed() && (legacy->key() == legacyKey) && (legacy->key() != job.key)) {
            removeSource(i);
            return;
        }
    }
}

void GeoZoneSourceRepository::removeSource(int index)
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

    _downloader->abort(source->key());
    _companions.remove(source->key());
    emit sourceAboutToChange(source);
    (void) _sources->removeAt(index);
    emit sourceChanged(source);
    source->deleteLater();

    if (!_storeReadOnly) {
        _store.removeFile(_storedEntries.take(source->key()).fileName);
        _saveIndex();
    }
    updateValidity();
}

void GeoZoneSourceRepository::clear()
{
    while (_sources->count() > 0) {
        removeSource(_sources->count() - 1);
    }
    // Drops results of new sources still loading
    for (auto it = _generations.begin(); it != _generations.end(); ++it) {
        it.value()++;
        _downloader->abort(it.key());
    }
}

void GeoZoneSourceRepository::loadStoredSources(LoadMode mode)
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
        job.formatHint = GeoZoneProviders::formatHint(job.url);
        job.etag = entry.etag;
        job.lastModified = entry.lastModified;
        job.datasetUrl = entry.datasetUrl;
        if (job.key.isEmpty() || entry.fileName.isEmpty() || !QFile::exists(job.sourcePath) || source(job.key)) {
            qCWarning(GeoZoneManagerLog) << "Skipping stored geozone source" << job.key << entry.fileName;
            continue;
        }
        job.generation = ++_generations[job.key];

        // Keep the order of the sources while they load
        GeoZoneSource *const placeholder = _newSource(job.key, job.url, entry.enabled);
        placeholder->setStoredDates(job.loadedAt, job.fileDate);
        placeholder->setValidators(job.etag, job.lastModified);
        placeholder->setDatasetUrl(job.datasetUrl);
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

GeoZoneSource *GeoZoneSourceRepository::_newSource(const QString &key, const QString &url, bool enabled)
{
    GeoZoneSource *const source = new GeoZoneSource(key, url, this);
    source->setEnabled(enabled);
    (void) connect(source, &GeoZoneSource::enabledChanged, this, [this, source]() { _sourceEnabledChanged(source); });
    _sources->append(source);
    return source;
}

void GeoZoneSourceRepository::_sourceEnabledChanged(GeoZoneSource *source)
{
    // The zones of the source join or leave the maps and checks
    emit sourceAboutToChange(source);
    emit sourceChanged(source);
    if (const auto it = _storedEntries.find(source->key()); (it != _storedEntries.end()) && !_storeReadOnly) {
        it->enabled = source->enabled();
        _saveIndex();
    }
    updateValidity();
}

void GeoZoneSourceRepository::_saveIndex()
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

void GeoZoneSourceRepository::updateValidity()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QStringList lines;
    for (int i = 0; i < _sources->count(); i++) {
        GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
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
