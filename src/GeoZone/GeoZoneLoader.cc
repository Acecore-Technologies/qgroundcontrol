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
#include "GeoZoneStore.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
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
    job.formatHint = formatHint(url);

    const QUrl parsedUrl(url);
    if (parsedUrl.isLocalFile()) {
        // Read in place, the file belongs to the user
        const QFileInfo info(parsedUrl.toLocalFile());
        job.sourcePath = info.filePath();
        job.fileDate = info.lastModified().toUTC();
    }
    return job;
}

GeoZoneParser::Format GeoZoneLoader::formatHint(const QString &url)
{
    return QUrl(url).host().endsWith(QStringLiteral("faa.gov")) ? GeoZoneParser::Format::FaaTfr : GeoZoneParser::Format::Unknown;
}

GeoZoneLoader::Result GeoZoneLoader::run(const Job &job, const GeoZoneStore &store)
{
    Result loadResult;

    QByteArray bytes = job.data;
    if (bytes.isEmpty()) {
        QFile file(job.sourcePath);
        if (!file.open(QIODevice::ReadOnly)) {
            loadResult.errorString = tr("Unable to open file %1: %2").arg(job.sourcePath, file.errorString());
            return loadResult;
        }
        if (file.size() > GeoZoneDownloader::defaultMaxBytes) {
            loadResult.errorString = tr("The file %1 is larger than %2.").arg(job.sourcePath, QLocale::c().formattedDataSize(GeoZoneDownloader::defaultMaxBytes));
            return loadResult;
        }
        bytes = file.readAll();
    }

    QString parseError;
    if (!GeoZoneParser::parse(bytes, loadResult.result, parseError, job.formatHint, job.language)) {
        loadResult.errorString = parseError;
        return loadResult;
    }

    if (!job.storedFileName.isEmpty()) {
        loadResult.stored = store.writeFile(job.storedFileName, bytes, loadResult.storeError);
    }

    return loadResult;
}
