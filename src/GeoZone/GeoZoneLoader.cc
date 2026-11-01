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
#include "QGCZip.h"

#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
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

    const QTemporaryDir dir;
    QFile archive(dir.filePath(QStringLiteral("archive.zip")));
    if (!dir.isValid() || !archive.open(QIODevice::WriteOnly) || (archive.write(data) != data.size())) {
        errorString = tr("Unable to extract the zip archive: %1").arg(archive.errorString());
        return QByteArray();
    }
    archive.close();
    return extractZipFile(archive.fileName(), errorString);
}

QByteArray GeoZoneLoader::extractZipFile(const QString &archivePath, QString &errorString)
{
    const QTemporaryDir dir;
    const QString extracted = dir.filePath(QStringLiteral("extracted"));
    if (!dir.isValid() || !QGCZip::unzipFile(archivePath, extracted)) {
        errorString = tr("Unable to extract the zip archive.");
        return QByteArray();
    }

    QDirIterator files(extracted, { QStringLiteral("*.xml"), QStringLiteral("*.json"), QStringLiteral("*.geojson") }, QDir::Files, QDirIterator::Subdirectories);
    QString largest;
    qint64 largestSize = -1;
    while (files.hasNext()) {
        const QFileInfo info(files.next());
        // Metadata files of the archive are small, the data is the largest file
        if (info.size() > largestSize) {
            largest = info.filePath();
            largestSize = info.size();
        }
    }
    QFile file(largest);
    if (largest.isEmpty() || (largestSize > GeoZoneDownloader::defaultMaxBytes) || !file.open(QIODevice::ReadOnly)) {
        errorString = tr("The zip archive has no geozone file.");
        return QByteArray();
    }
    return file.readAll();
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
