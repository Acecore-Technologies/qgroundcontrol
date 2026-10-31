/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneStore.h"
#include "GeoZoneManager.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QUuid>

namespace {

constexpr const char *keyKey = "key";
constexpr const char *urlKey = "url";
constexpr const char *fileKey = "file";
constexpr const char *loadedAtKey = "loadedAt";
constexpr const char *fileDateKey = "fileDate";
constexpr const char *etagKey = "etag";
constexpr const char *lastModifiedKey = "lastModified";
constexpr const char *sourcesKey = "sources";
constexpr const char *versionKey = "version";
constexpr int indexVersion = 1;

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

QList<GeoZoneStore::Entry> GeoZoneStore::readIndex() const
{
    QList<Entry> entries;

    QFile indexFile(path(indexFileName));
    if (indexFile.open(QIODevice::ReadOnly)) {
        const QJsonArray values = QJsonDocument::fromJson(indexFile.readAll()).object().value(sourcesKey).toArray();
        for (const QJsonValue &value : values) {
            const QJsonObject object = value.toObject();
            entries.append(Entry{
                object.value(keyKey).toString(),
                object.value(urlKey).toString(),
                object.value(fileKey).toString(),
                QDateTime::fromString(object.value(loadedAtKey).toString(), Qt::ISODate),
                QDateTime::fromString(object.value(fileDateKey).toString(), Qt::ISODate),
                object.value(etagKey).toString(),
                object.value(lastModifiedKey).toString(),
            });
        }
        return entries;
    }

    if (QFile::exists(path(legacyFileName))) {
        // Earlier versions kept a single file named in the settings
        QSettings settings;
        settings.beginGroup(legacySettingsGroup);
        const QString sourceName = settings.value(legacySourceNameKey).toString();
        settings.remove(legacySourceNameKey);
        settings.endGroup();

        const QDateTime fileDate = QFileInfo(path(legacyFileName)).lastModified().toUTC();
        Entry entry;
        entry.key = sourceName.isEmpty() ? QString(legacyFileName) : sourceName;
        entry.fileName = legacyFileName;
        entry.loadedAt = fileDate;
        entry.fileDate = fileDate;
        entries.append(entry);
    }

    return entries;
}

void GeoZoneStore::writeIndex(const QList<Entry> &entries) const
{
    if (entries.isEmpty()) {
        (void) QFile::remove(path(indexFileName));
        return;
    }

    QJsonArray values;
    for (const Entry &entry : entries) {
        QJsonObject object{
            { keyKey, entry.key },
            { fileKey, entry.fileName },
            { loadedAtKey, entry.loadedAt.toString(Qt::ISODate) },
            { fileDateKey, entry.fileDate.toString(Qt::ISODate) },
        };
        if (!entry.url.isEmpty()) {
            object.insert(urlKey, entry.url);
        }
        if (!entry.etag.isEmpty()) {
            object.insert(etagKey, entry.etag);
        }
        if (!entry.lastModified.isEmpty()) {
            object.insert(lastModifiedKey, entry.lastModified);
        }
        values.append(object);
    }

    const QByteArray bytes = QJsonDocument(QJsonObject{ { versionKey, indexVersion }, { sourcesKey, values } }).toJson();
    QString errorString;
    if (!saveFile(path(indexFileName), bytes, errorString)) {
        qCWarning(GeoZoneManagerLog) << "Unable to save geozone source index" << path(indexFileName) << errorString;
    }
}

bool GeoZoneStore::writeFile(const QString &fileName, const QByteArray &bytes, QString &errorString) const
{
    return saveFile(path(fileName), bytes, errorString);
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
        if ((fileName != QLatin1String(indexFileName)) && !referencedFileNames.contains(fileName)) {
            qCDebug(GeoZoneManagerLog) << "Removing unreferenced geozone file" << fileName;
            (void) QFile::remove(dir.filePath(fileName));
        }
    }
}
