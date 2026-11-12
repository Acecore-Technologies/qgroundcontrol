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
#include "GeoZoneHash.h"
#include "GeoZoneParseCache.h"
#include "GeoZoneProviders.h"
#include "GeoZoneService.h"
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
#include <QtCore/QSaveFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QSet>
#include <QtCore/QUrl>

namespace {

const QStringList kZipSuffixes = { QStringLiteral(".xml"), QStringLiteral(".json"), QStringLiteral(".geojson") };

/// @return The format hint the stored copy of a source with the URL is restored with at startup, see
///         GeoZoneLoader::storedJob, and which the parse cache of the copy is therefore written for
GeoZoneParser::Format storedFormatHint(const QString &url)
{
    return GeoZoneProviders::formatHint(url);
}

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
    job.storedFileName = GeoZoneStore::newFileName(localFileName);
    job.loadedAt = GeoZoneEnvironment::currentDateTimeUtc();
    job.fileDate = info.lastModified().toUTC();
    return job;
}

GeoZoneLoader::Job GeoZoneLoader::feedJob(const QString &url)
{
    Job job;
    job.key = url;
    job.url = url;
    job.loadedAt = GeoZoneEnvironment::currentDateTimeUtc();
    job.fileDate = job.loadedAt;
    job.formatHint = GeoZoneProviders::formatHint(url);

    const QUrl parsedUrl(url);
    if (parsedUrl.isLocalFile()) {
        // Read in place, the file belongs to the user
        const QFileInfo info(parsedUrl.toLocalFile());
        job.sourcePath = info.filePath();
        job.fileDate = info.lastModified().toUTC();
        job.storedFileName = GeoZoneStore::newFileName(job.sourcePath);
    }
    return job;
}

GeoZoneLoader::Job GeoZoneLoader::downloadJob(const GeoZoneFeedFetch::Request &request, const GeoZoneFeedFetch::Result &result)
{
    Job job = feedJob(request.url);
    job.sourcePath = result.filePath;
    job.pagePaths = result.pagePaths;
    job.downloaded = true;
    job.storedFileName = GeoZoneStore::newFileName(result.filePath);
    job.responseMember = request.service.effectiveResponseMember();
    job.formatHint = request.formatHint;
    job.etag = result.etag;
    job.lastModified = result.lastModified;
    job.datasetUrl = result.datasetUrl;
    job.maxAgeSecs = result.maxAgeSecs;
    job.area = request.area;

    GeoZoneProvenance &provenance = job.provenance;
    provenance.providerId = request.service.id.isEmpty() ? GeoZoneProviders::serviceId(request.url) : request.service.id;
    provenance.requestUrl = result.requestUrl;
    provenance.finalUrl = result.finalUrl;
    provenance.redirects = result.redirects;
    provenance.etag = result.etag;
    provenance.lastModified = result.lastModified;
    provenance.fetchedAt = job.loadedAt;
    provenance.sha256 = result.sha256;
    provenance.area = job.area;
    provenance.integrity = result.integrity;
    provenance.integrityText = result.integrityText;
    return job;
}

void GeoZoneLoader::_setEntry(Job &job, const GeoZoneSourceIndex::Entry &entry)
{
    job.key = entry.key;
    job.url = entry.url;
    job.loadedAt = entry.loadedAt;
    job.fileDate = entry.fileDate;
    job.checkedAt = entry.checkedAt;
    job.formatHint = storedFormatHint(entry.url);
    job.etag = entry.etag;
    job.lastModified = entry.lastModified;
    job.datasetUrl = entry.datasetUrl;
    job.maxAgeSecs = entry.maxAgeSecs;
    job.area = entry.area;
    job.provenance = GeoZoneProvenance::fromJson(entry.provenance);
}

GeoZoneLoader::Job GeoZoneLoader::storedJob(const GeoZoneSourceIndex::Entry &entry, const QString &sourcePath)
{
    Job job;
    _setEntry(job, entry);
    job.sourcePath = sourcePath;
    job.fromStore = true;
    return job;
}

GeoZoneLoader::Job GeoZoneLoader::importedJob(const GeoZoneSourceIndex::Entry &entry, const QString &filePath, const QString &packName, const QDateTime &importedAt)
{
    Job job;
    _setEntry(job, entry);
    job.sourcePath = filePath;
    // Adopted as the stored copy like a download
    job.downloaded = true;
    job.storedFileName = GeoZoneStore::newFileName(filePath);
    if (!job.loadedAt.isValid()) {
        job.loadedAt = importedAt;
    }
    if (!job.fileDate.isValid()) {
        job.fileDate = job.loadedAt;
    }
    job.provenance.imported = true;
    job.provenance.packName = packName;
    job.provenance.importedAt = importedAt;
    return job;
}

GeoZoneSourceIndex::Entry GeoZoneLoader::indexEntry(const Job &job, const GeoZoneSourceIndex::Entry &previous)
{
    GeoZoneSourceIndex::Entry entry = previous;
    entry.key = job.key;
    entry.url = job.url;
    entry.fileName = job.storedFileName;
    entry.loadedAt = job.loadedAt;
    entry.fileDate = job.fileDate;
    entry.checkedAt = (job.checkedAt > job.loadedAt) ? job.checkedAt : job.loadedAt;
    if (job.downloaded) {
        entry.etag = job.etag;
        entry.lastModified = job.lastModified;
        entry.datasetUrl = job.datasetUrl;
        entry.maxAgeSecs = job.maxAgeSecs;
        entry.area = job.area;
    }
    return entry;
}

namespace {

/// Appends the features of the further pages of a paged response to its first page, so that the stored copy holds all
/// of them, and removes the pages. A page which can't be read fails the merge: a response missing zones isn't stored.
/// The first page is replaced as a whole, it is left as it was when the merged document can't be written.
/// @param member Member of the pages holding the zones, see GeoZoneService::responseMember, empty for GeoJSON pages
bool mergePages(const QString &filePath, const QStringList &pagePaths, const QString &member, QString &errorString)
{
    const auto removePages = qScopeGuard([&pagePaths]() {
        for (const QString &pagePath : pagePaths) {
            (void) QFile::remove(pagePath);
        }
    });
    const QString featuresKey = QStringLiteral("features");
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        errorString = GeoZoneLoader::tr("Unable to open file %1: %2").arg(filePath, file.errorString());
        return false;
    }
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    if (!root.value(featuresKey).isArray()) {
        errorString = GeoZoneLoader::tr("The pages of the response could not be put together: its first page has no features.");
        return false;
    }
    QJsonArray features = root.value(featuresKey).toArray();
    for (const QString &pagePath : pagePaths) {
        QFile page(pagePath);
        if (!page.open(QIODevice::ReadOnly)) {
            errorString = GeoZoneLoader::tr("Unable to open file %1: %2").arg(pagePath, page.errorString());
            return false;
        }
        QByteArray document = page.readAll();
        if (!member.isEmpty()) {
            document = GeoZoneService::extractMember(document, member, errorString);
            if (document.isEmpty()) {
                return false;
            }
        }
        const QJsonValue pageFeatures = QJsonDocument::fromJson(document).object().value(featuresKey);
        if (!pageFeatures.isArray()) {
            errorString = GeoZoneLoader::tr("The pages of the response could not be put together: page %1 of %2 has no features.")
                              .arg(pagePaths.indexOf(pagePath) + 2).arg(pagePaths.count() + 1);
            return false;
        }
        for (const QJsonValue &feature : pageFeatures.toArray()) {
            features.append(feature);
        }
    }
    root.insert(featuresKey, features);
    root.insert(QStringLiteral("numberReturned"), features.count());
    const QByteArray merged = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QSaveFile mergedFile(filePath);
    if (!mergedFile.open(QIODevice::WriteOnly) || (mergedFile.write(merged) != merged.size()) || !mergedFile.commit()) {
        errorString = GeoZoneLoader::tr("Unable to write file %1: %2").arg(filePath, mergedFile.errorString());
        return false;
    }
    return true;
}

/// Replaces a service's response with the member holding the zones, so that the stored copy is the zones' document
bool extractMember(const QString &filePath, const QString &member, QString &errorString)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        errorString = GeoZoneLoader::tr("Unable to open file %1: %2").arg(filePath, file.errorString());
        return false;
    }
    const QByteArray document = GeoZoneService::extractMember(file.readAll(), member, errorString);
    file.close();
    if (document.isEmpty()) {
        return false;
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || (file.write(document) != document.size())) {
        errorString = GeoZoneLoader::tr("Unable to write file %1: %2").arg(filePath, file.errorString());
        return false;
    }
    return true;
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
        qCDebug(GeoZoneSourceLog) << "Read" << loadResult.result.zones.count() << "zones of" << GeoZoneDownloader::redactedUrl(job.key) << "from the parse cache in" << timer.elapsed() << "ms";
        return loadResult;
    }
    if (job.downloaded && !job.responseMember.isEmpty() && !extractMember(job.sourcePath, job.responseMember, loadResult.errorString)) {
        return loadResult;
    }
    if (!job.pagePaths.isEmpty() && !mergePages(job.sourcePath, job.pagePaths, job.responseMember, loadResult.errorString)) {
        return loadResult;
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
    // The hash also identifies the data in its provenance
    const QByteArray contentHash = parsed ? GeoZoneHash::contentHash(content) : QByteArray();
    loadResult.contentHash = contentHash;
    loadResult.contentSize = parsed ? contentSize : -1;
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
    qCDebug(GeoZoneSourceLog) << "Parsed" << loadResult.result.zones.count() << "zones of" << GeoZoneDownloader::redactedUrl(job.key) << "in" << timer.elapsed() << "ms";
    if (!job.storedFileName.isEmpty()) {
        loadResult.stored = job.downloaded ? store.adoptFile(job.sourcePath, job.storedFileName, loadResult.storeError)
                                           : store.copyFile(job.sourcePath, job.storedFileName, loadResult.storeError);
    }
    if (writeCache && (loadResult.stored || job.fromStore) && !job.cancelToken.isCancelled()) {
        const QString storedPath = job.fromStore ? job.sourcePath : store.path(job.storedFileName);
        // The cache is read by the job restoring the copy at the next start, whose hint is the one of the address.
        // This load may have had another, like the format of the zones of the feed it refreshes: keyed with it, the
        // cache would never be used and every start would parse the copy again.
        const GeoZoneParseCache::Key storedKey{ job.language, job.fromStore ? job.formatHint : storedFormatHint(job.url) };
        (void) GeoZoneParseCache::write(storedPath, contentSize, contentHash, storedKey, loadResult.result);
    }

    return loadResult;
}
