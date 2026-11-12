/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneStore.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneParseCache.h"
#include "GeoZoneSource.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QUuid>

namespace {

constexpr QLatin1StringView kPartialSuffix(".part");    ///< Of a copy being written, see GeoZoneStore::copyFile

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

QString GeoZoneStore::newFileName(const QString &sourcePath)
{
    return QStringLiteral("%1.%2").arg(QUuid::createUuid().toString(QUuid::WithoutBraces), fileSuffix(sourcePath));
}

QString GeoZoneStore::fileSuffix(const QString &filePath)
{
    // A file which can't be read is named like the copies of earlier versions: the content of a copy decides how it
    // is read, and the load of this one fails with the reason
    QFile file(filePath);
    if (filePath.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        return QStringLiteral("json");
    }
    QByteArray head = file.read(1024);
    if (head.startsWith("PK\x03\x04")) {
        return QStringLiteral("zip");
    }
    if (head.startsWith("\xEF\xBB\xBF")) {
        head.remove(0, 3);
    }
    return head.trimmed().startsWith('<') ? QStringLiteral("xml") : QStringLiteral("json");
}

QStringList GeoZoneStore::fileNameFilters()
{
    return { QStringLiteral("*.json"), QStringLiteral("*.xml"), QStringLiteral("*.zip") };
}

bool GeoZoneStore::isStoredFileName(const QString &fileName)
{
    // A UUID without braces and the suffix of the content, see newFileName
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.(json|xml|zip)\\z"));
    return (fileName == QLatin1String(legacyFileName)) || pattern.match(fileName).hasMatch();
}

QString GeoZoneStore::moveToBackup() const
{
    QDir dir(_dir);
    const QString backupName = QStringLiteral("backup-%1").arg(GeoZoneEnvironment::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    if (!dir.mkpath(backupName)) {
        return QString();
    }

    bool moved = false;
    for (const QString &fileName : dir.entryList(fileNameFilters(), QDir::Files)) {
        if (isStoredFileName(fileName) || (fileName == QLatin1String(indexFileName))) {
            moved = dir.rename(fileName, QStringLiteral("%1/%2").arg(backupName, fileName)) || moved;
            GeoZoneParseCache::remove(dir.filePath(fileName));
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
    const QString partial = target + kPartialSuffix;
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
    if (fileName.isEmpty()) {
        return;
    }
    // The index may name a file of the folder which the store didn't make: it isn't the store's to remove
    if (isStoredFileName(fileName)) {
        (void) QFile::remove(path(fileName));
    }
    GeoZoneParseCache::remove(path(fileName));
}

void GeoZoneStore::removeOrphanFiles(const QSet<QString> &referencedFileNames) const
{
    // Only what the store named itself: the directory also has files of others
    const QDir dir(_dir);
    for (const QString &fileName : dir.entryList(QDir::Files)) {
        // What an interrupted copy left, see copyFile, is never a complete copy
        if (fileName.endsWith(kPartialSuffix)) {
            if (isStoredFileName(fileName.chopped(kPartialSuffix.size()))) {
                qCDebug(GeoZoneSourceLog) << "Removing interrupted copy" << fileName;
                (void) QFile::remove(dir.filePath(fileName));
            }
            continue;
        }
        // The copies, and the caches of those removed or left by an interrupted removal
        const QString copyName = GeoZoneParseCache::isCacheFileName(fileName) ? QFileInfo(fileName).completeBaseName() : fileName;
        if (isStoredFileName(copyName) && !referencedFileNames.contains(copyName)) {
            qCDebug(GeoZoneSourceLog) << "Removing unreferenced geozone file" << fileName;
            (void) QFile::remove(dir.filePath(fileName));
        }
    }
}
