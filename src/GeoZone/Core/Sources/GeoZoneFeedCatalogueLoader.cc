/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFeedCatalogueLoader.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneRefreshScheduler.h"
#include "GeoZoneSource.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtNetwork/QHostAddress>

#include <algorithm>

GeoZoneFeedCatalogueLoader::GeoZoneFeedCatalogueLoader(const QString &directory, GeoZoneDownloader *downloader, QObject *parent)
    : QObject(parent)
    , _directory(directory)
    , _downloader(downloader)
    , _tag(QStringLiteral("feed-catalogue-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

GeoZoneFeedCatalogueLoader::~GeoZoneFeedCatalogueLoader()
{
    if (_downloader) {
        _downloader->abort(_tag);
    }
    if (_applied) {
        GeoZoneProviders::resetCatalogue();
    }
}

QString GeoZoneFeedCatalogueLoader::filePath() const
{
    return QDir(_directory).filePath(QString::fromLatin1(GeoZoneFeedCatalogue::fileName));
}

void GeoZoneFeedCatalogueLoader::init()
{
    _initialized = true;
    _readCache();
    if (!_cache.data.isEmpty()) {
        QString errorString;
        if (!GeoZoneFeedCatalogue::parse(_cache.data, _cache.url, _urlCatalogue, errorString)) {
            _urlCatalogue = GeoZoneFeedCatalogue::Catalogue();
        }
    }
    const bool fileChanged = _readFile();
    if (fileChanged || !_urlCatalogue.feeds.isEmpty() || !_urlCatalogue.templates.isEmpty() || !_urlCatalogue.disabled.isEmpty()) {
        _apply();
    }
    check();
}

void GeoZoneFeedCatalogueLoader::setUrl(const QString &url)
{
    const QString trimmed = url.trimmed();
    if (trimmed == _url) {
        return;
    }
    _url = trimmed;
    // Before init, the copy of the catalogue isn't read yet
    if (!_initialized) {
        return;
    }
    if (_downloader) {
        _downloader->abort(_tag);
    }
    _downloading = false;
    _failures = 0;
    _retryAt = QDateTime();
    _downloadError.clear();
    _urlError.clear();
    _urlRefused.clear();
    // The catalogue of another URL is dropped
    if (_cache.url != _url) {
        const bool hadCatalogue = (_urlCatalogue.entryCount() > 0) || !_urlCatalogue.warnings.isEmpty();
        _cache = Cache();
        _urlCatalogue = GeoZoneFeedCatalogue::Catalogue();
        (void) QFile::remove(QDir(_directory).filePath(QString::fromLatin1(cacheFileName)));
        if (hadCatalogue) {
            _apply();
        }
    }
    emit changed();
    check(true);
}

void GeoZoneFeedCatalogueLoader::check(bool force)
{
    if (!_initialized) {
        return;
    }
    if (_readFile()) {
        _apply();
    }
    if (_url.isEmpty() || _downloading) {
        return;
    }
    if (const QString problem = _urlProblem(); !problem.isEmpty()) {
        _refuseUrl(problem);
        return;
    }
    if (QUrl(_url).isLocalFile()) {
        _readUrlFile();
        return;
    }
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    if (!force) {
        if (_retryAt.isValid() && (now < _retryAt)) {
            return;
        }
        const qint64 intervalSecs = qint64{ GeoZoneEnvironment::sourceSettings().feedRefreshHours } * 3600;
        if (!_retryAt.isValid() && _cache.checkedAt.isValid() && (_cache.checkedAt.secsTo(now) < intervalSecs)) {
            return;
        }
    }
    _download();
}

void GeoZoneFeedCatalogueLoader::retryUnreachable()
{
    if (_unreachable) {
        check(true);
    }
}

QString GeoZoneFeedCatalogueLoader::_urlProblem() const
{
    const QUrl url(_url);
    if (_url.isEmpty() || url.isLocalFile()) {
        return QString();
    }
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() || ((scheme != QStringLiteral("https")) && (scheme != QStringLiteral("http")))) {
        return tr("It is not an https address.");
    }
    // A catalogue changes the feeds offered: over http, anyone on the way could change it. A server on this computer,
    // like one of a test, is reached without a network.
    const bool loopback = (url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) || QHostAddress(url.host()).isLoopback();
    if ((scheme == QStringLiteral("http")) && !loopback) {
        return tr("Its address must start with https://, a catalogue read over http could be changed on its way.");
    }
    return QString();
}

void GeoZoneFeedCatalogueLoader::_refuseUrl(const QString &problem)
{
    const bool hadCatalogue = (_urlCatalogue.entryCount() > 0) || !_urlCatalogue.warnings.isEmpty();
    const bool differs = hadCatalogue || (problem != _urlRefused);
    // Also the copy init() left aside
    if (differs) {
        (void) QFile::remove(QDir(_directory).filePath(QString::fromLatin1(cacheFileName)));
    }
    _cache = Cache();
    _urlCatalogue = GeoZoneFeedCatalogue::Catalogue();
    _urlRefused = problem;
    _urlError.clear();
    _downloadError.clear();
    _unreachable = false;
    if (differs) {
        qCWarning(GeoZoneSourceLog) << "Feed catalogue not downloaded" << GeoZoneDownloader::redactedUrl(_url) << problem;
    }
    if (hadCatalogue) {
        _apply();
    } else if (differs) {
        emit changed();
    }
}

bool GeoZoneFeedCatalogueLoader::_readFile()
{
    const QFileInfo info(filePath());
    const QDateTime modified = info.exists() ? info.lastModified() : QDateTime();
    const qint64 size = info.exists() ? info.size() : -1;
    if ((modified == _fileModified) && (size == _fileSize)) {
        return false;
    }
    _fileModified = modified;
    _fileSize = size;
    _fileCatalogue = GeoZoneFeedCatalogue::Catalogue();
    _fileError.clear();
    if (!info.exists()) {
        return true;
    }

    QFile file(info.filePath());
    if (size > maxBytes) {
        _fileError = tr("The file is larger than %1 KB.").arg(maxBytes / 1024);
    } else if (!file.open(QIODevice::ReadOnly)) {
        _fileError = file.errorString();
    } else if (!GeoZoneFeedCatalogue::parse(file.readAll(), info.fileName(), _fileCatalogue, _fileError)) {
        _fileCatalogue = GeoZoneFeedCatalogue::Catalogue();
    }
    if (!_fileError.isEmpty()) {
        qCWarning(GeoZoneSourceLog) << "Feed catalogue ignored" << info.filePath() << _fileError;
    }
    return true;
}

void GeoZoneFeedCatalogueLoader::_readUrlFile()
{
    const QFileInfo info(QUrl(_url).toLocalFile());
    const QDateTime modified = info.exists() ? info.lastModified() : QDateTime();
    if (_cache.checkedAt.isValid() && (_cache.url == _url) && (_cache.lastModified == modified.toString(Qt::ISODate)) && (_cache.etag == QString::number(info.size()))) {
        return;
    }
    _cache = Cache();
    _cache.url = _url;
    _cache.checkedAt = GeoZoneEnvironment::currentDateTimeUtc();
    _cache.lastModified = modified.toString(Qt::ISODate);
    _cache.etag = QString::number(info.size());
    _urlCatalogue = GeoZoneFeedCatalogue::Catalogue();
    _urlError.clear();
    _downloadError.clear();
    QFile file(info.filePath());
    if (!info.exists() || (info.size() > maxBytes) || !file.open(QIODevice::ReadOnly)) {
        _downloadError = info.exists() ? tr("It can't be read.") : tr("The file doesn't exist.");
    } else if (!GeoZoneFeedCatalogue::parse(file.readAll(), _url, _urlCatalogue, _urlError)) {
        _urlCatalogue = GeoZoneFeedCatalogue::Catalogue();
    }
    _apply();
}

void GeoZoneFeedCatalogueLoader::_download()
{
    if (!_downloader) {
        return;
    }
    _downloading = true;
    const bool sameUrl = (_cache.url == _url) && !_cache.data.isEmpty();
    qCDebug(GeoZoneSourceLog) << "Downloading the feed catalogue" << GeoZoneDownloader::redactedUrl(_url);
    _downloader->get(_url, sameUrl ? _cache.etag : QString(), sameUrl ? _cache.lastModified : QString(),
                     [this](const GeoZoneDownloader::Result &result) { _downloaded(result); }, _tag);
}

void GeoZoneFeedCatalogueLoader::_downloaded(const GeoZoneDownloader::Result &result)
{
    _downloading = false;
    _unreachable = result.unreachable;
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    if (result.status == GeoZoneDownloader::Result::Status::Failed) {
        if (!result.filePath.isEmpty()) {
            (void) QFile::remove(result.filePath);
        }
        // Retried like a feed, later and later
        _failures++;
        const qint64 delay = (std::min)(qint64{ GeoZoneRefreshScheduler::maxRetrySecs }, qint64{ GeoZoneRefreshScheduler::minRetrySecs } << (std::min)(_failures - 1, 16));
        _retryAt = now.addSecs(delay);
        _downloadError = result.errorString;
        qCWarning(GeoZoneSourceLog) << "Feed catalogue download failed" << GeoZoneDownloader::redactedUrl(_url) << result.errorString;
        emit changed();
        return;
    }

    _failures = 0;
    _retryAt = QDateTime();
    _downloadError.clear();
    if (result.status == GeoZoneDownloader::Result::Status::NotModified) {
        _cache.checkedAt = now;
        _writeCache();
        emit changed();
        return;
    }

    const QByteArray data = GeoZoneDownloader::readFile(result.filePath, maxBytes);
    (void) QFile::remove(result.filePath);
    GeoZoneFeedCatalogue::Catalogue catalogue;
    QString errorString;
    if (data.isEmpty()) {
        errorString = tr("It is empty or larger than %1 KB.").arg(maxBytes / 1024);
    } else if (!GeoZoneFeedCatalogue::parse(data, _url, catalogue, errorString)) {
        qCWarning(GeoZoneSourceLog) << "Feed catalogue ignored" << GeoZoneDownloader::redactedUrl(_url) << errorString;
    }
    if (!errorString.isEmpty()) {
        // The last valid copy stays in use, the invalid one isn't asked for again before the next refresh
        _urlError = errorString;
        _cache.checkedAt = now;
        if (_cache.url != _url) {
            _cache.url = _url;
            _cache.data.clear();
            _cache.etag.clear();
            _cache.lastModified.clear();
        }
        _writeCache();
        emit changed();
        return;
    }

    _urlError.clear();
    _urlCatalogue = catalogue;
    _cache.url = _url;
    _cache.etag = result.etag;
    _cache.lastModified = result.lastModified;
    _cache.checkedAt = now;
    _cache.data = data;
    _writeCache();
    _apply();
}

void GeoZoneFeedCatalogueLoader::_readCache()
{
    QFile file(QDir(_directory).filePath(QString::fromLatin1(cacheFileName)));
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    _cache.url = object.value(QStringLiteral("url")).toString();
    // Only the catalogue of the URL of the settings is used, and no copy of a URL which isn't downloaded any more
    if ((_cache.url != _url) || !_urlProblem().isEmpty()) {
        _cache = Cache();
        return;
    }
    _cache.etag = object.value(QStringLiteral("etag")).toString();
    _cache.lastModified = object.value(QStringLiteral("lastModified")).toString();
    _cache.checkedAt = QDateTime::fromString(object.value(QStringLiteral("checkedAt")).toString(), Qt::ISODate);
    _cache.data = object.value(QStringLiteral("catalogue")).toString().toUtf8();
}

void GeoZoneFeedCatalogueLoader::_writeCache()
{
    (void) QDir().mkpath(_directory);
    QSaveFile file(QDir(_directory).filePath(QString::fromLatin1(cacheFileName)));
    const QJsonObject object{
        { QStringLiteral("url"), _cache.url }, { QStringLiteral("etag"), _cache.etag }, { QStringLiteral("lastModified"), _cache.lastModified },
        { QStringLiteral("checkedAt"), _cache.checkedAt.toUTC().toString(Qt::ISODate) }, { QStringLiteral("catalogue"), QString::fromUtf8(_cache.data) },
    };
    if (!file.open(QIODevice::WriteOnly) || (file.write(QJsonDocument(object).toJson()) < 0) || !file.commit()) {
        qCWarning(GeoZoneSourceLog) << "The copy of the feed catalogue can't be saved" << file.fileName() << file.errorString();
    }
}

void GeoZoneFeedCatalogueLoader::_apply()
{
    GeoZoneFeedCatalogue::Catalogue catalogue = GeoZoneFeedCatalogue::builtIn();
    // A downloaded catalogue may add and replace feeds, the built-in ones it turns off are told
    _disabledBuiltIns.clear();
    for (const QString &id : std::as_const(_urlCatalogue.disabled)) {
        for (const GeoZoneProviders::Service &service : std::as_const(catalogue.feeds)) {
            if (service.id == id) {
                _disabledBuiltIns.append(QCoreApplication::translate(GeoZoneFeedCatalogue::translationContext, service.name.toUtf8().constData()));
            }
        }
        for (const GeoZoneProviders::ServiceTemplate &serviceTemplate : std::as_const(catalogue.templates)) {
            if (serviceTemplate.service.id == id) {
                const QString name = serviceTemplate.service.name.isEmpty() ? serviceTemplate.name : serviceTemplate.service.name;
                _disabledBuiltIns.append(QCoreApplication::translate(GeoZoneFeedCatalogue::translationContext, name.toUtf8().constData()));
            }
        }
    }
    GeoZoneFeedCatalogue::apply(catalogue, _urlCatalogue);
    GeoZoneFeedCatalogue::apply(catalogue, _fileCatalogue);
    GeoZoneProviders::setCatalogue(catalogue.feeds, catalogue.templates);
    _applied = true;
    emit changed();
}

QString GeoZoneFeedCatalogueLoader::warning() const
{
    QStringList lines;
    const QString fileName = QString::fromLatin1(GeoZoneFeedCatalogue::fileName);
    if (!_fileError.isEmpty()) {
        lines.append(tr("The feed catalogue %1 is ignored: %2").arg(filePath(), _fileError));
    }
    for (const QString &warning : _fileCatalogue.warnings) {
        lines.append(QStringLiteral("%1: %2").arg(fileName, warning));
    }
    if (!_url.isEmpty()) {
        if (!_urlRefused.isEmpty()) {
            lines.append(tr("The feed catalogue %1 is not downloaded: %2").arg(_url, _urlRefused));
        }
        if (!_disabledBuiltIns.isEmpty()) {
            lines.append(tr("The feed catalogue of %1 turns off %n built-in feed(s): %2", nullptr, static_cast<int>(_disabledBuiltIns.count()))
                             .arg(_url, _disabledBuiltIns.join(QStringLiteral(", "))));
        }
        if (!_downloadError.isEmpty()) {
            lines.append((_urlCatalogue.entryCount() > 0) ? tr("The feed catalogue couldn't be downloaded from %1, its copy of %2 is used: %3")
                                                                 .arg(_url, QLocale().toString(_cache.checkedAt.toLocalTime(), QLocale::ShortFormat), _downloadError)
                                                           : tr("The feed catalogue couldn't be downloaded from %1: %2").arg(_url, _downloadError));
        }
        if (!_urlError.isEmpty()) {
            lines.append((_urlCatalogue.entryCount() > 0) ? tr("The feed catalogue downloaded from %1 is ignored, its previous copy is used: %2").arg(_url, _urlError)
                                                          : tr("The feed catalogue downloaded from %1 is ignored: %2").arg(_url, _urlError));
        }
        for (const QString &warning : _urlCatalogue.warnings) {
            lines.append(QStringLiteral("%1: %2").arg(_url, warning));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QString GeoZoneFeedCatalogueLoader::statusText() const
{
    QStringList parts = { tr("built-in feeds") };
    if (_urlCatalogue.entryCount() > 0) {
        parts.append(tr("%n change(s) from %1", nullptr, _urlCatalogue.entryCount()).arg(_url));
    }
    if (_fileCatalogue.entryCount() > 0) {
        parts.append(tr("%n change(s) from %1", nullptr, _fileCatalogue.entryCount()).arg(QString::fromLatin1(GeoZoneFeedCatalogue::fileName)));
    }
    return tr("Feed catalogue: %1").arg(parts.join(QStringLiteral(", ")));
}
