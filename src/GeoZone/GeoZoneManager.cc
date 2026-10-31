/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneManager.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "QGCApplication.h"
#include "QGCFileDownload.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/qapplicationstatic.h>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QFutureWatcher>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>

QGC_LOGGING_CATEGORY(GeoZoneManagerLog, "qgc.geozone.geozonemanager")

Q_APPLICATION_STATIC(GeoZoneManager, _geoZoneManager);

GeoZoneManager::GeoZoneManager(const QString &storageDir, QObject *parent)
    : QObject(parent)
    , _storageDir(storageDir)
    , _zones(new QmlObjectListModel(this))
    , _sources(new QmlObjectListModel(this))
    , _activeTimer(new QTimer(this))
    , _refreshTimer(new QTimer(this))
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

    _refreshTimer->setInterval(_refreshCheckIntervalMSecs);
    (void) connect(_refreshTimer, &QTimer::timeout, this, &GeoZoneManager::_refreshDueFeeds);
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
        preset(tr("Switzerland (FOCA)"),        QStringLiteral("https://data.geo.admin.ch/ch.bazl.einschraenkungen-drohnen/einschraenkungen-drohnen/einschraenkungen-drohnen_4326.json")),
    };
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

QString GeoZoneManager::_storedPath(const QString &storedFileName) const
{
    return QDir(_storageDir).filePath(storedFileName);
}

QString GeoZoneManager::_newStoredFileName()
{
    return QStringLiteral("%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void GeoZoneManager::addFile(const QString &fileName)
{
    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    const QFileInfo info(localFileName);

    LoadJob job;
    job.key = info.fileName();
    job.sourcePath = localFileName;
    job.storedFileName = _newStoredFileName();
    job.loadedAt = QDateTime::currentDateTimeUtc();
    job.fileDate = info.lastModified().toUTC();
    job.userInitiated = true;
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
    job.storedFileName = _newStoredFileName();
    job.loadedAt = QDateTime::currentDateTimeUtc();
    job.fileDate = info.lastModified().toUTC();
    job.generation = ++_generations[job.key];

    const LoadResult loadResult = _runLoad(job, _storageDir);
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
    _download(trimmedUrl, true /* userInitiated */);
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
        if (!source->busy() && source->refreshDue(now, refreshIntervalHours)) {
            _download(source->url(), false /* userInitiated */);
        }
    }
}

void GeoZoneManager::_download(const QString &url, bool userInitiated)
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
        job.storedFileName = _newStoredFileName();
        job.loadedAt = QDateTime::currentDateTimeUtc();
        job.fileDate = info.lastModified().toUTC();
        job.userInitiated = userInitiated;
        job.generation = generation;
        _startLoad(job, true /* background */);
        return;
    }

    if (!_networkManager) {
        _networkManager = new QNetworkAccessManager(this);
    }
    QNetworkRequest request(parsedUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(downloadTimeoutSecs * 1000);

    _setPending(1);
    QNetworkReply *const reply = _networkManager->get(request);
    QGCFileDownload::setIgnoreSSLErrorsIfNeeded(*reply);
    _downloads.insert(reply, Download{ url });
    emit progressChanged();
    (void) connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64 total) {
        const auto it = _downloads.find(reply);
        if (it != _downloads.end()) {
            it->received = received;
            it->total = total;
            emit progressChanged();
        }
    });
    (void) connect(reply, &QNetworkReply::finished, this, [this, reply, url, generation, userInitiated]() {
        reply->deleteLater();
        (void) _downloads.remove(reply);
        emit progressChanged();

        QString errorString;
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            errorString = reply->errorString();
        } else if ((statusCode < 200) || (statusCode >= 300)) {
            errorString = tr("The server responded with status %1.").arg(statusCode);
        }
        _downloadComplete(url, generation, (errorString.isEmpty() ? reply->readAll() : QByteArray()), errorString, userInitiated);
    });
}

double GeoZoneManager::progress() const
{
    qint64 received = 0;
    qint64 total = 0;
    for (const Download &download : _downloads) {
        if (download.total <= 0) {
            return -1;
        }
        received += download.received;
        total += download.total;
    }
    // Parsing has no progress to report
    return (total > 0) ? (static_cast<double>(received) / total) : -1;
}

QString GeoZoneManager::progressText() const
{
    if (_downloads.isEmpty()) {
        return busy() ? tr("Loading geozones...") : QString();
    }

    qint64 received = 0;
    qint64 total = 0;
    bool totalKnown = true;
    for (const Download &download : _downloads) {
        received += download.received;
        total += download.total;
        totalKnown = totalKnown && (download.total > 0);
    }
    const QLocale locale;
    const QString size = totalKnown ?
        tr("%1 of %2").arg(locale.formattedDataSize(received), locale.formattedDataSize(total)) :
        locale.formattedDataSize(received);
    const QString name = (_downloads.count() == 1) ? QUrl(_downloads.cbegin()->url).host() : tr("%1 feeds").arg(_downloads.count());
    return tr("Downloading %1: %2").arg(name, size);
}

void GeoZoneManager::_downloadComplete(const QString &url, int generation, const QByteArray &data, const QString &errorString, bool userInitiated)
{
    if (_generations.value(url) != generation) {
        // Removed or downloaded again in the meantime
        _setPending(-1);
        return;
    }

    if (!errorString.isEmpty() || data.isEmpty()) {
        const QString error = errorString.isEmpty() ? tr("No data was received.") : errorString;
        qCWarning(GeoZoneManagerLog) << "Download failed" << url << error;
        if (GeoZoneSource *const existing = source(url)) {
            existing->setBusy(false);
            existing->setErrorString(error);
        }
        if (userInitiated) {
            qgcApp()->showAppMessage(tr("Geozone download from %1 failed. %2").arg(url, error));
        }
        _setPending(-1);
        return;
    }

    LoadJob job;
    job.key = url;
    job.url = url;
    job.data = data;
    job.storedFileName = _newStoredFileName();
    job.loadedAt = QDateTime::currentDateTimeUtc();
    job.fileDate = job.loadedAt;
    job.userInitiated = userInitiated;
    job.generation = generation;
    _startLoad(job, true /* background */);
    _setPending(-1);
}

GeoZoneManager::LoadResult GeoZoneManager::_runLoad(const LoadJob &job, const QString &storageDir)
{
    LoadResult loadResult;

    QByteArray bytes = job.data;
    if (bytes.isEmpty()) {
        QFile file(job.sourcePath);
        if (!file.open(QIODevice::ReadOnly)) {
            loadResult.errorString = tr("Unable to open file %1: %2").arg(job.sourcePath, file.errorString());
            return loadResult;
        }
        bytes = file.readAll();
    }

    QString parseError;
    if (!GeoZoneParser::parse(bytes, loadResult.result, parseError)) {
        loadResult.errorString = parseError;
        return loadResult;
    }

    if (!job.storedFileName.isEmpty()) {
        (void) QDir().mkpath(storageDir);
        QSaveFile storedFile(QDir(storageDir).filePath(job.storedFileName));
        if (!storedFile.open(QIODevice::WriteOnly) || (storedFile.write(bytes) != bytes.size()) || !storedFile.commit()) {
            qCWarning(GeoZoneManagerLog) << "Unable to store copy of geozone source" << job.key << storedFile.errorString();
        }
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
        _applyLoad(job, _runLoad(job, _storageDir));
        _setPending(-1);
        return;
    }

    QFutureWatcher<LoadResult> *const watcher = new QFutureWatcher<LoadResult>(this);
    (void) connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, job]() {
        _applyLoad(job, watcher->result());
        watcher->deleteLater();
        _setPending(-1);
    });
    watcher->setFuture(QtConcurrent::run(&GeoZoneManager::_runLoad, job, _storageDir));
}

void GeoZoneManager::_applyLoad(const LoadJob &job, const LoadResult &loadResult)
{
    if (_generations.value(job.key) != job.generation) {
        // Removed or loaded again in the meantime
        if (!job.storedFileName.isEmpty()) {
            (void) QFile::remove(_storedPath(job.storedFileName));
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
        if (job.userInitiated) {
            qgcApp()->showAppMessage(tr("Geozone file load failed. %1").arg(loadResult.errorString));
        }
        return;
    }

    _beginZonesChange();
    if (!source) {
        source = new GeoZoneSource(job.key, job.url, this);
        _sources->append(source);
    }
    if (!job.storedFileName.isEmpty()) {
        const QString previousStoredFileName = source->storedFileName();
        source->setStoredFileName(job.storedFileName);
        if (!previousStoredFileName.isEmpty() && (previousStoredFileName != job.storedFileName)) {
            (void) QFile::remove(_storedPath(previousStoredFileName));
        }
    }
    source->setResult(loadResult.result, job.loadedAt, job.fileDate);
    source->setErrorString(QString());
    _endZonesChange();

    _saveIndex();
    _updateValidity();

    qCDebug(GeoZoneManagerLog) << "Loaded" << source->zoneCount() << source->formatName() << "zones from" << job.key;

    if (job.userInitiated) {
        setVisible(true);
        emit sourceAdded(source);
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
            qgcApp()->showAppMessage(tr("Some geozone data may be outdated. Load current files in Plan View > Fence > Geozones.\n\n%1").arg(_validityWarning), tr("Geozones"));
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

    if (!storedFileName.isEmpty()) {
        (void) QFile::remove(_storedPath(storedFileName));
    }
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
    _shapes.clear();
    _boundingBox = QGeoRectangle();
}

void GeoZoneManager::_endZonesChange()
{
    QList<QObject*> zones;
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        zones.append(QList<QObject*>(source->zones().cbegin(), source->zones().cend()));
        _shapes.append(source->shapes());
        if (source->boundingBox().isValid()) {
            _boundingBox = _boundingBox.isValid() ? _boundingBox.united(source->boundingBox()) : source->boundingBox();
        }
    }

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
    QJsonArray entries;
    QFile indexFile(_storedPath(_indexFileName));
    if (indexFile.open(QIODevice::ReadOnly)) {
        entries = QJsonDocument::fromJson(indexFile.readAll()).object().value(QStringLiteral("sources")).toArray();
        indexFile.close();
    } else if (QFile::exists(_storedPath(_legacyStoredFileName))) {
        // Earlier versions kept a single file
        QSettings settings;
        settings.beginGroup(_settingsGroup);
        const QString sourceName = settings.value(_legacySourceNameKey).toString();
        settings.remove(_legacySourceNameKey);
        settings.endGroup();

        const QString fileDate = QFileInfo(_storedPath(_legacyStoredFileName)).lastModified().toUTC().toString(Qt::ISODate);
        entries.append(QJsonObject{
            { QStringLiteral("key"), sourceName.isEmpty() ? QString(_legacyStoredFileName) : sourceName },
            { QStringLiteral("file"), QString(_legacyStoredFileName) },
            { QStringLiteral("loadedAt"), fileDate },
            { QStringLiteral("fileDate"), fileDate },
        });
    }

    QList<LoadJob> jobs;
    for (const QJsonValue &entryValue : std::as_const(entries)) {
        const QJsonObject entry = entryValue.toObject();
        LoadJob job;
        job.key = entry.value(QStringLiteral("key")).toString();
        job.url = entry.value(QStringLiteral("url")).toString();
        const QString storedFileName = entry.value(QStringLiteral("file")).toString();
        job.sourcePath = _storedPath(storedFileName);
        job.loadedAt = QDateTime::fromString(entry.value(QStringLiteral("loadedAt")).toString(), Qt::ISODate);
        job.fileDate = QDateTime::fromString(entry.value(QStringLiteral("fileDate")).toString(), Qt::ISODate);
        if (job.key.isEmpty() || storedFileName.isEmpty() || !QFile::exists(job.sourcePath) || source(job.key)) {
            qCWarning(GeoZoneManagerLog) << "Skipping stored geozone source" << job.key << storedFileName;
            continue;
        }
        job.generation = ++_generations[job.key];

        // Keep the order of the sources while they load
        GeoZoneSource *const placeholder = new GeoZoneSource(job.key, job.url, this);
        placeholder->setStoredFileName(storedFileName);
        placeholder->setStoredDates(job.loadedAt, job.fileDate);
        _sources->append(placeholder);
        jobs.append(job);
    }

    _removeOrphanFiles();
    _saveIndex();

    for (const LoadJob &job : std::as_const(jobs)) {
        _startLoad(job, background);
    }
}

void GeoZoneManager::_saveIndex() const
{
    QJsonArray entries;
    for (int i = 0; i < _sources->count(); i++) {
        const GeoZoneSource *const source = _sources->value<GeoZoneSource*>(i);
        if (source->storedFileName().isEmpty()) {
            continue;
        }
        QJsonObject entry{
            { QStringLiteral("key"), source->key() },
            { QStringLiteral("file"), source->storedFileName() },
            { QStringLiteral("loadedAt"), source->loadedAt().toString(Qt::ISODate) },
            { QStringLiteral("fileDate"), source->fileDate().toString(Qt::ISODate) },
        };
        if (source->isFeed()) {
            entry.insert(QStringLiteral("url"), source->url());
        }
        entries.append(entry);
    }

    if (entries.isEmpty()) {
        (void) QFile::remove(_storedPath(_indexFileName));
        return;
    }

    (void) QDir().mkpath(_storageDir);
    QSaveFile indexFile(_storedPath(_indexFileName));
    const QByteArray bytes = QJsonDocument(QJsonObject{ { QStringLiteral("version"), 1 }, { QStringLiteral("sources"), entries } }).toJson();
    if (!indexFile.open(QIODevice::WriteOnly) || (indexFile.write(bytes) != bytes.size()) || !indexFile.commit()) {
        qCWarning(GeoZoneManagerLog) << "Unable to save geozone source index" << indexFile.fileName() << indexFile.errorString();
    }
}

void GeoZoneManager::_removeOrphanFiles() const
{
    QSet<QString> referenced = { QString(_indexFileName) };
    for (int i = 0; i < _sources->count(); i++) {
        referenced.insert(_sources->value<GeoZoneSource*>(i)->storedFileName());
    }

    const QDir dir(_storageDir);
    for (const QString &fileName : dir.entryList({ QStringLiteral("*.json") }, QDir::Files)) {
        if (!referenced.contains(fileName)) {
            qCDebug(GeoZoneManagerLog) << "Removing unreferenced geozone file" << fileName;
            (void) QFile::remove(dir.filePath(fileName));
        }
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
    if (region.isValid()) {
        for (int i = 0; i < _zones->count(); i++) {
            GeoZone *const zone = _zones->value<GeoZone*>(i);
            if (region.contains(zone->center())) {
                candidates.append(zone);
            }
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
    QList<GeoZone*> zones;
    for (int i = 0; i < _zones->count(); i++) {
        GeoZone *const zone = _zones->value<GeoZone*>(i);
        if (!restrictions.contains(zone->restriction())) {
            continue;
        }
        if (area.isValid() && !area.intersects(zone->boundingBox())) {
            continue;
        }
        if (activeFrom.isValid() && !(activeTo.isValid() ? zone->isActiveDuring(activeFrom, activeTo) : zone->isActiveAt(activeFrom))) {
            continue;
        }
        zones.append(zone);
    }
    return zones;
}

QList<GeoZone*> GeoZoneManager::zonesCrossingPaths(const QList<GeoZone::Restriction> &restrictions, const QList<QList<QGeoCoordinate>> &paths) const
{
    QList<GeoZone*> zones;
    if (paths.isEmpty()) {
        return zones;
    }

    for (int i = 0; i < _zones->count(); i++) {
        GeoZone *const zone = _zones->value<GeoZone*>(i);
        if (!restrictions.contains(zone->restriction())) {
            continue;
        }
        if (std::any_of(paths.cbegin(), paths.cend(), [zone](const QList<QGeoCoordinate> &path) { return zone->intersectsPath(path); })) {
            zones.append(zone);
        }
    }
    return zones;
}
