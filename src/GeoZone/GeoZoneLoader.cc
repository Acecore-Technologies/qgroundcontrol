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
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QScopeGuard>
#include <QtCore/QUrl>

GeoZoneLoader::Job GeoZoneLoader::fileJob(const QString &fileName)
{
    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    const QFileInfo info(localFileName);

    Job job;
    job.key = info.fileName();
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

GeoZoneLoader::Result GeoZoneLoader::run(const Job &job, const GeoZoneStore &store)
{
    Result loadResult;
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
