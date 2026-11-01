/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneLoader.h"
#include "GeoZoneDownloader.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneParseCache.h"
#include "GeoZoneProviders.h"
#include "GeoZoneSource.h"
#include "GeoZoneStore.h"
#include "GeoZoneZip.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QScopeGuard>
#include <QtCore/QSet>
#include <QtCore/QUrl>

namespace {

const QStringList kZipSuffixes = { QStringLiteral(".xml"), QStringLiteral(".json"), QStringLiteral(".geojson") };

} // namespace

QString GeoZoneLoader::fileKey(const QString &fileName)
{
    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    const QFileInfo info(localFileName);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

GeoZoneLoader::Job GeoZoneLoader::fileJob(const QString &fileName)
{
    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    const QFileInfo info(localFileName);

    Job job;
    job.key = fileKey(localFileName);
    job.sourcePath = localFileName;
    job.storedFileName = GeoZoneStore::newFileName();
    job.loadedAt = GeoZoneEnvironment::currentDateTimeUtc();
    job.fileDate = info.lastModified().toUTC();
    return job;
}

GeoZoneLoader::Job GeoZoneLoader::feedJob(const QString &url)
{
    Job job;
    job.key = url;
    job.url = url;
    job.storedFileName = GeoZoneStore::newFileName();
    job.loadedAt = GeoZoneEnvironment::currentDateTimeUtc();
    job.fileDate = job.loadedAt;
    job.formatHint = GeoZoneProviders::formatHint(url);

    const QUrl parsedUrl(url);
    if (parsedUrl.isLocalFile()) {
        // Read in place, the file belongs to the user
        const QFileInfo info(parsedUrl.toLocalFile());
        job.sourcePath = info.filePath();
        job.fileDate = info.lastModified().toUTC();
    }
    return job;
}

QByteArray GeoZoneLoader::extractZip(const QByteArray &data, QString &errorString)
{
    if (!data.startsWith("PK\x03\x04")) {
        return data;
    }

    return GeoZoneZip::largestFile(data, kZipSuffixes, GeoZoneDownloader::defaultMaxBytes, errorString);
}

QByteArray GeoZoneLoader::extractZipFile(const QString &archivePath, QString &errorString)
{
    QFile archive(archivePath);
    if (archive.size() > GeoZoneDownloader::defaultMaxBytes) {
        errorString = tr("The zip archive is too large.");
        return QByteArray();
    }
    if (!archive.open(QIODevice::ReadOnly)) {
        errorString = tr("Unable to extract the zip archive: %1").arg(archive.errorString());
        return QByteArray();
    }
    return extractZip(archive.readAll(), errorString);
}

namespace {

/// Appends the features of the further pages of a WFS response to its first page, so that the stored copy holds all
/// of them, and removes the pages. Pages which can't be read are left out, the response then still misses zones.
void mergePages(const QString &filePath, const QStringList &pagePaths)
{
    const auto removePages = qScopeGuard([&pagePaths]() {
        for (const QString &pagePath : pagePaths) {
            (void) QFile::remove(pagePath);
        }
    });
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    if (!root.value(QStringLiteral("features")).isArray()) {
        return;
    }
    QJsonArray features = root.value(QStringLiteral("features")).toArray();
    for (const QString &pagePath : pagePaths) {
        QFile page(pagePath);
        if (!page.open(QIODevice::ReadOnly)) {
            continue;
        }
        for (const QJsonValue &feature : QJsonDocument::fromJson(page.readAll()).object().value(QStringLiteral("features")).toArray()) {
            features.append(feature);
        }
    }
    root.insert(QStringLiteral("features"), features);
    root.insert(QStringLiteral("numberReturned"), features.count());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        (void) file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }
}

} // namespace

bool GeoZoneLoader::parse(const QByteArray &content, GeoZoneParser::Result &result, QString &errorString, GeoZoneParser::Format formatHint,
                          const QString &language, qint64 maxBytes)
{
    GeoZoneParser::Options options;
    options.formatHint = formatHint;
    options.language = language;
    return parse(content, result, errorString, options, maxBytes);
}

bool GeoZoneLoader::parse(const QByteArray &content, GeoZoneParser::Result &result, QString &errorString, const GeoZoneParser::Options &options,
                          qint64 maxBytes)
{
    if (!content.startsWith("PK\x03\x04")) {
        return GeoZoneParser::parse(content, result, errorString, options);
    }

    // Packages split into several files are loaded together. The largest file which parses sets the format, files of
    // other formats, like other renditions of the data or metadata, are left out before their features are read, and
    // zones published twice are kept once. Files are inflated one at a time.
    result = GeoZoneParser::Result();
    errorString.clear();
    bool parsed = false;
    int fileCount = 0;
    QString failedFile;
    QString parseError;
    QCryptographicHash contentHash(QCryptographicHash::Sha256);
    QSet<QByteArray> zoneHashes;
    const bool read = GeoZoneZip::forEachFile(content, kZipSuffixes, maxBytes, errorString, [&](const GeoZoneZip::File &file) {
        if (options.cancelToken.isCancelled()) {
            return false;
        }
        fileCount++;
        GeoZoneParser::Options fileOptions = options;
        if (parsed) {
            fileOptions.onlyFormat = result.format;
        }
        GeoZoneParser::Result fileResult;
        QString fileError;
        if (!GeoZoneParser::parse(file.data, fileResult, fileError, fileOptions)) {
            if (failedFile.isEmpty()) {
                failedFile = file.name;
                parseError = fileError;
            }
            return true;
        }
        if (!parsed) {
            QList<GeoZone::Data> zones = std::move(fileResult.zones);
            result = fileResult;
            result.zones.clear();
            fileResult.zones = std::move(zones);
        } else {
            result.skippedFeatures += fileResult.skippedFeatures;
            result.missingFeatures += fileResult.missingFeatures;
            for (auto it = fileResult.issues.cbegin(); it != fileResult.issues.cend(); ++it) {
                result.issues[it.key()] += it.value();
            }
            result.companions.insert(fileResult.companions);
        }
        parsed = true;
        contentHash.addData(fileResult.contentHash);
        for (GeoZone::Data &zone : fileResult.zones) {
            if (zone.contentHash.isEmpty() || !zoneHashes.contains(zone.contentHash)) {
                zoneHashes.insert(zone.contentHash);
                result.zones.append(std::move(zone));
            }
        }
        return true;
    });
    if (!read) {
        result = GeoZoneParser::Result();
        return false;
    }
    if (options.cancelToken.isCancelled()) {
        result = GeoZoneParser::Result();
        errorString = GeoZoneParser::tr("The parse was canceled.");
        return false;
    }
    if (!parsed) {
        errorString = (fileCount > 1) ? tr("%1: %2").arg(failedFile, parseError) : parseError;
        return false;
    }
    result.contentHash = contentHash.result();
    return true;
}

GeoZoneLoader::Result GeoZoneLoader::run(const Job &job, const GeoZoneStore &store)
{
    Result loadResult;
    // Downloads which don't become the stored copy are removed
    const auto removeDownload = qScopeGuard([&job, &loadResult]() {
        if (job.downloaded && !loadResult.stored) {
            for (const QString &path : job.pagePaths + QStringList{ job.sourcePath }) {
                (void) QFile::remove(path);
            }
        }
    });
    if (job.cancelToken.isCancelled()) {
        loadResult.cancelled = true;
        return loadResult;
    }
    QElapsedTimer timer;
    timer.start();
    const GeoZoneParseCache::Key cacheKey{ job.language, job.formatHint };
    if (job.fromStore && GeoZoneParseCache::read(job.sourcePath, cacheKey, loadResult.result)) {
        loadResult.fromCache = true;
        qCDebug(GeoZoneSourceLog) << "Read" << loadResult.result.zones.count() << "zones of" << job.key << "from the parse cache in" << timer.elapsed() << "ms";
        return loadResult;
    }
    if (!job.pagePaths.isEmpty()) {
        mergePages(job.sourcePath, job.pagePaths);
    }

    QFile file(job.sourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        loadResult.errorString = tr("Unable to open file %1: %2").arg(job.sourcePath, file.errorString());
        return loadResult;
    }
    if (file.size() > GeoZoneDownloader::defaultMaxBytes) {
        loadResult.errorString = tr("The file %1 is larger than %2.").arg(job.sourcePath, QLocale::c().formattedDataSize(GeoZoneDownloader::defaultMaxBytes));
        return loadResult;
    }

    // Large national files are parsed from the mapped file rather than a copy in memory
    QByteArray content;
    uchar *const mapped = (file.size() > 0) ? file.map(0, file.size()) : nullptr;
    if (mapped) {
        content = QByteArray::fromRawData(reinterpret_cast<const char*>(mapped), file.size());
    } else {
        content = file.readAll();
    }

    // Zip archives are read from the mapped file
    GeoZoneParser::Options options;
    options.formatHint = job.formatHint;
    options.language = job.language;
    options.cancelToken = job.cancelToken;
    QString parseError;
    const bool parsed = parse(content, loadResult.result, parseError, options);
    // The parse cache identifies the stored copy by its size and hash
    const bool writeCache = parsed && (job.fromStore || !job.storedFileName.isEmpty());
    const qint64 contentSize = content.size();
    const QByteArray contentHash = writeCache ? GeoZoneParseCache::hash(content) : QByteArray();
    content.clear();
    if (mapped) {
        (void) file.unmap(mapped);
    }
    file.close();
    if (job.cancelToken.isCancelled()) {
        loadResult.result = GeoZoneParser::Result();
        loadResult.cancelled = true;
        return loadResult;
    }
    if (!parsed) {
        loadResult.errorString = parseError;
        return loadResult;
    }
    qCDebug(GeoZoneSourceLog) << "Parsed" << loadResult.result.zones.count() << "zones of" << job.key << "in" << timer.elapsed() << "ms";
    if (!job.storedFileName.isEmpty()) {
        loadResult.stored = job.downloaded ? store.adoptFile(job.sourcePath, job.storedFileName, loadResult.storeError)
                                           : store.copyFile(job.sourcePath, job.storedFileName, loadResult.storeError);
    }
    if (writeCache && (loadResult.stored || job.fromStore) && !job.cancelToken.isCancelled()) {
        const QString storedPath = job.fromStore ? job.sourcePath : store.path(job.storedFileName);
        (void) GeoZoneParseCache::write(storedPath, contentSize, contentHash, cacheKey, loadResult.result);
    }

    return loadResult;
}
