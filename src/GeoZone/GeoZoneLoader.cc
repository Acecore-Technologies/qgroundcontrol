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
#include "QGCZip.h"

#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
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
    const QString host = QUrl(url).host();
    if (host.endsWith(QStringLiteral("faa.gov"))) {
        return GeoZoneParser::Format::FaaTfr;
    }
    if (host == QStringLiteral("uas-betrieb.de")) {
        return GeoZoneParser::Format::Dipul;
    }
    if (host == QStringLiteral("data.geopf.fr")) {
        return GeoZoneParser::Format::FranceUas;
    }
    if (host.endsWith(QStringLiteral("ead-it.com"))) {
        return GeoZoneParser::Format::Aixm;
    }
    return GeoZoneParser::Format::Unknown;
}

QString GeoZoneLoader::datasetLink(const QString &url, const QByteArray &data, const QDate &today)
{
    const QByteArray start = data.left(1024).trimmed().toLower();
    if (!start.startsWith("<!doctype html") && !start.startsWith("<html")) {
        return QString();
    }

    // NATS lists the full UAS flight restriction datasets of the current and next AIRAC cycles
    static const QRegularExpression linkRegExp(QStringLiteral("href=\"([^\"]*UAS_AREA_1/[^\"]*_FULL_(\\d{8})_XML\\.zip)\""));
    QString link;
    QDate linkDate;
    QString earliestLink;
    QDate earliestDate;
    QRegularExpressionMatchIterator it = linkRegExp.globalMatch(QString::fromUtf8(data));
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QDate date = QDate::fromString(match.captured(2), QStringLiteral("yyyyMMdd"));
        if (!date.isValid()) {
            continue;
        }
        if ((date <= today) && (!linkDate.isValid() || (date > linkDate))) {
            link = match.captured(1);
            linkDate = date;
        }
        if (!earliestDate.isValid() || (date < earliestDate)) {
            earliestLink = match.captured(1);
            earliestDate = date;
        }
    }
    if (link.isEmpty()) {
        link = earliestLink;
    }
    return link.isEmpty() ? QString() : QUrl(url).resolved(QUrl(link)).toString();
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
    const QString extracted = dir.filePath(QStringLiteral("extracted"));
    if (!QGCZip::unzipFile(archive.fileName(), extracted)) {
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

    QString extractError;
    const QByteArray content = extractZip(bytes, extractError);
    if (content.isEmpty()) {
        loadResult.errorString = extractError;
        return loadResult;
    }

    QString parseError;
    if (!GeoZoneParser::parse(content, loadResult.result, parseError, job.formatHint, job.language)) {
        loadResult.errorString = parseError;
        return loadResult;
    }

    if (!job.storedFileName.isEmpty()) {
        loadResult.stored = store.writeFile(job.storedFileName, bytes, loadResult.storeError);
    }

    return loadResult;
}
