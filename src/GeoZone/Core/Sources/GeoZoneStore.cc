/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneStore.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneSource.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QUuid>

namespace {

bool saveFile(const QString &path, const QByteArray &bytes, QString &errorString)
{
    (void) QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || (file.write(bytes) != bytes.size()) || !file.commit()) {
        errorString = file.errorString();
        return false;
    }
    return true;
}

} // namespace

GeoZoneStore::GeoZoneStore(const QString &dir)
    : _dir(dir)
{
}

QString GeoZoneStore::path(const QString &fileName) const
{
    return QDir(_dir).filePath(fileName);
}

QString GeoZoneStore::newFileName()
{
    return QStringLiteral("%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QString GeoZoneStore::moveToBackup() const
{
    QDir dir(_dir);
    const QString backupName = QStringLiteral("backup-%1").arg(GeoZoneEnvironment::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    if (!dir.mkpath(backupName)) {
        return QString();
    }

    bool moved = false;
    for (const QString &fileName : dir.entryList({ QStringLiteral("*.json") }, QDir::Files)) {
        if (fileName != QLatin1String(GeoZoneAuthorisations::fileName)) {
            moved = dir.rename(fileName, QStringLiteral("%1/%2").arg(backupName, fileName)) || moved;
        }
    }
    return moved ? dir.filePath(backupName) : QString();
}

bool GeoZoneStore::writeFile(const QString &fileName, const QByteArray &bytes, QString &errorString) const
{
    return saveFile(path(fileName), bytes, errorString);
}

bool GeoZoneStore::copyFile(const QString &sourcePath, const QString &fileName, QString &errorString) const
{
    if (!QDir().mkpath(_dir)) {
        errorString = QStringLiteral("Unable to create %1").arg(_dir);
        return false;
    }
    // Copied under a temporary name first, so an interrupted copy is never taken for a complete one
    const QString target = path(fileName);
    const QString partial = target + QStringLiteral(".part");
    (void) QFile::remove(partial);
    QFile source(sourcePath);
    if (!source.copy(partial)) {
        errorString = source.errorString();
        (void) QFile::remove(partial);
        return false;
    }
    if (!QFile::rename(partial, target)) {
        errorString = QStringLiteral("Unable to rename %1").arg(partial);
        (void) QFile::remove(partial);
        return false;
    }
    return true;
}

bool GeoZoneStore::adoptFile(const QString &sourcePath, const QString &fileName, QString &errorString) const
{
    if (!QDir().mkpath(_dir)) {
        errorString = QStringLiteral("Unable to create %1").arg(_dir);
        return false;
    }
    if (QFile::rename(sourcePath, path(fileName))) {
        return true;
    }
    // Other file systems can't rename across
    if (!copyFile(sourcePath, fileName, errorString)) {
        return false;
    }
    (void) QFile::remove(sourcePath);
    return true;
}

QString GeoZoneStore::downloadDir() const
{
    return QDir(_dir).filePath(QStringLiteral("downloads"));
}

void GeoZoneStore::clearDownloads() const
{
    QDir dir(downloadDir());
    if (dir.exists()) {
        (void) dir.removeRecursively();
    }
}

void GeoZoneStore::removeFile(const QString &fileName) const
{
    if (!fileName.isEmpty()) {
        (void) QFile::remove(path(fileName));
    }
}

void GeoZoneStore::removeOrphanFiles(const QSet<QString> &referencedFileNames) const
{
    const QDir dir(_dir);
    for (const QString &fileName : dir.entryList({ QStringLiteral("*.json") }, QDir::Files)) {
        if ((fileName != QLatin1String(GeoZoneAuthorisations::fileName)) && !referencedFileNames.contains(fileName)) {
            qCDebug(GeoZoneSourceLog) << "Removing unreferenced geozone file" << fileName;
            (void) QFile::remove(dir.filePath(fileName));
        }
    }
}
