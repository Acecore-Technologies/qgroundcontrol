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
#include "GeoZoneProviders.h"
#include "GeoZoneStore.h"
#include "GeoZoneZip.h"

#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QScopeGuard>
#include <QtCore/QUrl>

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
    job.loadedAt = QDateTime::currentDateTimeUtc();
    job.fileDate = info.lastModified().toUTC();
    return job;
}

GeoZoneLoader::Job GeoZoneLoader::feedJob(const QString &url)
{
    Job job;
    job.key = url;
    job.url = url;
    job.storedFileName = GeoZoneStore::newFileName();
    job.loadedAt = QDateTime::currentDateTimeUtc();
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

    return GeoZoneZip::largestFile(data, { QStringLiteral(".xml"), QStringLiteral(".json"), QStringLiteral(".geojson") }, GeoZoneDownloader::defaultMaxBytes, errorString);
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

GeoZoneLoader::Result GeoZoneLoader::run(const Job &job, const GeoZoneStore &store)
{
    Result loadResult;
    if (!job.pagePaths.isEmpty()) {
        mergePages(job.sourcePath, job.pagePaths);
    }
    // Downloads which don't become the stored copy are removed
    const auto removeDownload = qScopeGuard([&job, &loadResult]() {
        if (job.downloaded && !loadResult.stored) {
            (void) QFile::remove(job.sourcePath);
        }
    });

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

    if (content.startsWith("PK\x03\x04")) {
        QString extractError;
        content = extractZipFile(job.sourcePath, extractError);
        if (content.isEmpty()) {
            loadResult.errorString = extractError;
            return loadResult;
        }
    }

    QString parseError;
    const bool parsed = GeoZoneParser::parse(content, loadResult.result, parseError, job.formatHint, job.language);
    content.clear();
    if (mapped) {
        (void) file.unmap(mapped);
    }
    file.close();
    if (!parsed) {
        loadResult.errorString = parseError;
        return loadResult;
    }

    if (!job.storedFileName.isEmpty()) {
        loadResult.stored = job.downloaded ? store.adoptFile(job.sourcePath, job.storedFileName, loadResult.storeError)
                                           : store.copyFile(job.sourcePath, job.storedFileName, loadResult.storeError);
    }

    return loadResult;
}
