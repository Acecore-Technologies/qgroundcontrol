/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneSourceIndex.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtCore/QSettings>

namespace {

constexpr const char *keyKey = "key";
constexpr const char *urlKey = "url";
constexpr const char *fileKey = "file";
constexpr const char *loadedAtKey = "loadedAt";
constexpr const char *fileDateKey = "fileDate";
constexpr const char *etagKey = "etag";
constexpr const char *lastModifiedKey = "lastModified";
constexpr const char *datasetUrlKey = "datasetUrl";
constexpr const char *enabledKey = "enabled";
constexpr const char *maxAgeKey = "maxAge";
constexpr const char *checkedAtKey = "checkedAt";
constexpr const char *serviceKey = "service";
constexpr const char *areaKey = "area";
constexpr const char *provenanceKey = "provenance";
constexpr const char *changesKey = "changes";
constexpr const char *sourcesKey = "sources";
constexpr const char *versionKey = "version";

} // namespace

GeoZoneSourceIndex::GeoZoneSourceIndex(const GeoZoneStore &store)
    : _store(store)
{
}

GeoZoneSourceIndex::Contents GeoZoneSourceIndex::read() const
{
    Contents contents;

    QFile indexFile(_store.path(indexFileName));
    if (indexFile.exists()) {
        contents.status = Status::Unreadable;
        if (!indexFile.open(QIODevice::ReadOnly)) {
            contents.errorString = indexFile.errorString();
            return contents;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(indexFile.readAll(), &parseError);
        const QJsonObject root = document.object();
        if (parseError.error != QJsonParseError::NoError) {
            contents.errorString = parseError.errorString();
            return contents;
        }
        if (!document.isObject() || !root.value(sourcesKey).isArray() || !root.value(versionKey).isDouble()) {
            contents.errorString = QStringLiteral("Unexpected content");
            return contents;
        }
        if (root.value(versionKey).toInt() > version) {
            contents.status = Status::Newer;
            return contents;
        }

        for (const QJsonValue &value : root.value(sourcesKey).toArray()) {
            contents.entries.append(entryFromJson(value.toObject()));
        }
        contents.status = Status::Ok;
        return contents;
    }

    if (QFile::exists(_store.path(legacyFileName))) {
        // Earlier versions kept a single file named in the settings
        QSettings settings;
        settings.beginGroup(legacySettingsGroup);
        const QString sourceName = settings.value(legacySourceNameKey).toString();
        settings.remove(legacySourceNameKey);
        settings.endGroup();

        const QDateTime fileDate = QFileInfo(_store.path(legacyFileName)).lastModified().toUTC();
        Entry entry;
        entry.key = sourceName.isEmpty() ? QString(legacyFileName) : sourceName;
        entry.fileName = legacyFileName;
        entry.loadedAt = fileDate;
        entry.fileDate = fileDate;
        entry.checkedAt = fileDate;
        contents.entries.append(entry);
    }

    return contents;
}

bool GeoZoneSourceIndex::write(const QList<Entry> &entries, QString &errorString) const
{
    errorString.clear();

    if (entries.isEmpty()) {
        QFile indexFile(_store.path(indexFileName));
        if (indexFile.exists() && !indexFile.remove()) {
            errorString = indexFile.errorString();
            return false;
        }
        return true;
    }

    QJsonArray values;
    for (const Entry &entry : entries) {
        values.append(entryToJson(entry));
    }

    const QByteArray bytes = QJsonDocument(QJsonObject{ { versionKey, version }, { sourcesKey, values } }).toJson();
    return _store.writeFile(indexFileName, bytes, errorString);
}

GeoZoneSourceIndex::Entry GeoZoneSourceIndex::entryFromJson(const QJsonObject &object)
{
    Entry entry;
    entry.key = object.value(keyKey).toString();
    entry.url = object.value(urlKey).toString();
    entry.fileName = object.value(fileKey).toString();
    entry.loadedAt = QDateTime::fromString(object.value(loadedAtKey).toString(), Qt::ISODate);
    entry.fileDate = QDateTime::fromString(object.value(fileDateKey).toString(), Qt::ISODate);
    entry.etag = object.value(etagKey).toString();
    entry.lastModified = object.value(lastModifiedKey).toString();
    entry.enabled = object.value(enabledKey).toBool(true);
    entry.datasetUrl = object.value(datasetUrlKey).toString();
    entry.maxAgeSecs = object.value(maxAgeKey).toInteger(-1);
    // Only written once the server confirmed the copy after downloading it
    entry.checkedAt = QDateTime::fromString(object.value(checkedAtKey).toString(), Qt::ISODate);
    if (!entry.checkedAt.isValid() || (entry.checkedAt < entry.loadedAt)) {
        entry.checkedAt = entry.loadedAt;
    }
    entry.service = object.value(serviceKey).toObject();
    entry.provenance = object.value(provenanceKey).toObject();
    entry.changes = object.value(changesKey).toObject();
    const QJsonArray area = object.value(areaKey).toArray();
    if (area.count() == 4) {
        entry.area = QGeoRectangle(QGeoCoordinate(area[3].toDouble(), area[0].toDouble()), QGeoCoordinate(area[1].toDouble(), area[2].toDouble()));
    }
    return entry;
}

QJsonObject GeoZoneSourceIndex::entryToJson(const Entry &entry)
{
    QJsonObject object{
        { keyKey, entry.key },
        { fileKey, entry.fileName },
        { loadedAtKey, entry.loadedAt.toString(Qt::ISODate) },
        { fileDateKey, entry.fileDate.toString(Qt::ISODate) },
    };
    const std::pair<const char*, QString> texts[] = {
        { urlKey, entry.url }, { etagKey, entry.etag }, { lastModifiedKey, entry.lastModified }, { datasetUrlKey, entry.datasetUrl },
    };
    for (const auto &[name, text] : texts) {
        if (!text.isEmpty()) {
            object.insert(QLatin1String(name), text);
        }
    }
    if (!entry.enabled) {
        object.insert(enabledKey, false);
    }
    if (entry.maxAgeSecs > 0) {
        object.insert(maxAgeKey, entry.maxAgeSecs);
    }
    if (entry.checkedAt.isValid() && (entry.checkedAt > entry.loadedAt)) {
        object.insert(checkedAtKey, entry.checkedAt.toString(Qt::ISODate));
    }
    if (!entry.service.isEmpty()) {
        object.insert(serviceKey, entry.service);
    }
    if (!entry.provenance.isEmpty()) {
        object.insert(provenanceKey, entry.provenance);
    }
    if (!entry.changes.isEmpty()) {
        object.insert(changesKey, entry.changes);
    }
    if (entry.area.isValid()) {
        object.insert(areaKey, QJsonArray{ entry.area.topLeft().longitude(), entry.area.bottomRight().latitude(),
                                           entry.area.bottomRight().longitude(), entry.area.topLeft().latitude() });
    }
    return object;
}

void GeoZoneSourceIndex::replace(const Entry &entry)
{
    const QString previousFileName = _entries.value(entry.key).fileName;
    _entries.insert(entry.key, entry);
    if (previousFileName != entry.fileName) {
        _store.removeFile(previousFileName);
    }
}

void GeoZoneSourceIndex::remove(const QString &key)
{
    if (!_readOnly) {
        _store.removeFile(_entries.take(key).fileName);
    }
}

bool GeoZoneSourceIndex::setEnabled(const QString &key, bool enabled)
{
    const auto it = _entries.find(key);
    if ((it == _entries.end()) || _readOnly) {
        return false;
    }
    it->enabled = enabled;
    return true;
}

bool GeoZoneSourceIndex::setService(const QString &key, const QJsonObject &service)
{
    const auto it = _entries.find(key);
    if ((it == _entries.end()) || _readOnly || (it->service == service)) {
        return false;
    }
    it->service = service;
    return true;
}

bool GeoZoneSourceIndex::confirm(const QString &key, const QString &etag, const QString &lastModified, const QDateTime &checkedAt, qint64 maxAgeSecs)
{
    // The stored copy is only confirmed current when it is the version the validators belong to
    const auto it = _entries.find(key);
    if ((it == _entries.end()) || (it->etag != etag) || (it->lastModified != lastModified)) {
        return false;
    }
    it->checkedAt = checkedAt;
    it->maxAgeSecs = maxAgeSecs;
    return true;
}

void GeoZoneSourceIndex::removeUnreferencedFiles() const
{
    QSet<QString> referenced = { QString(indexFileName) };
    for (const Entry &entry : _entries) {
        referenced.insert(entry.fileName);
    }
    _store.removeOrphanFiles(referenced);
}

bool GeoZoneSourceIndex::save(const QStringList &keys, QString &errorString) const
{
    errorString.clear();
    if (_readOnly) {
        return true;
    }

    QList<Entry> entries;
    for (const QString &key : keys) {
        const auto it = _entries.constFind(key);
        if (it != _entries.cend()) {
            entries.append(it.value());
        }
    }
    return write(entries, errorString);
}
