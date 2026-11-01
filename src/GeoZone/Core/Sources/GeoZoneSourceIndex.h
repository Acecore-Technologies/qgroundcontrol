/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtPositioning/QGeoRectangle>

#include "GeoZoneStore.h"

/// The persisted index of the geozone sources: for each source kept in the GeoZoneStore, its origin, the name of its
/// stored copy, its dates, HTTP validators, how its service is requested and whether it is turned on. Secrets aren't
/// kept here, see GeoZoneCredentials. Reads the index of earlier versions, keeps an
/// index written by a newer version untouched, and writes the entries in the order of the sources.
class GeoZoneSourceIndex
{
public:
    struct Entry {
        QString key;            ///< File name or URL of the source
        QString url;            ///< Feed URL, empty for files
        QString fileName;       ///< Stored copy in the directory
        QDateTime loadedAt;
        QDateTime fileDate;
        QString etag;
        QString lastModified;
        bool enabled = true;    ///< See GeoZoneSource::enabled
        QString datasetUrl;     ///< Dataset the feed's page linked to, whose download the validators belong to
        qint64 maxAgeSecs = -1; ///< How long the server said the data stays current, negative when it didn't
        QDateTime checkedAt;    ///< When the server last confirmed the stored copy current, at least loadedAt
        QJsonObject service;    ///< See GeoZoneService::toJson, empty for plain feeds and files. Holds no secrets.
        QGeoRectangle area;     ///< Area of interest the feed was downloaded for, invalid when its service takes none
        QJsonObject provenance; ///< See GeoZoneProvenance::toJson
    };

    enum class Status : quint8 {
        Ok,
        Missing,        ///< No index, entries has the single file kept by earlier versions if there is one
        Unreadable,     ///< The index exists but isn't valid
        Newer,          ///< Written by a newer version which may store sources differently
    };

    struct Contents {
        Status status = Status::Missing;
        QList<Entry> entries;
        QString errorString;
    };

    explicit GeoZoneSourceIndex(const GeoZoneStore &store);

    /// @return The entry of a source in the index, also the fields restoring a source in a pack, see GeoZonePack
    static Entry entryFromJson(const QJsonObject &object);
    static QJsonObject entryToJson(const Entry &entry);

    /// @return Index entries in source order. Without an index, the single file kept by earlier versions is
    ///         returned and its name setting is removed.
    Contents read() const;
    /// Writes the index, removing it when there are no entries
    bool write(const QList<Entry> &entries, QString &errorString) const;

    /// An index written by a newer version is kept as is: sources are neither recorded nor removed
    bool readOnly() const { return _readOnly; }
    void setReadOnly(bool readOnly) { _readOnly = readOnly; }

    bool contains(const QString &key) const { return _entries.contains(key); }
    Entry entry(const QString &key) const { return _entries.value(key); }
    /// @return Name of the stored copy of a source, empty when it has none
    QString fileName(const QString &key) const { return _entries.value(key).fileName; }
    /// Records an entry read from the index, whose copy is kept
    void insert(const Entry &entry) { _entries.insert(entry.key, entry); }
    /// Records a new stored copy of a source, removing the copy it replaces
    void replace(const Entry &entry);
    /// Forgets a source and removes its stored copy
    void remove(const QString &key);
    /// @return true if the source has an entry, which was changed
    bool setEnabled(const QString &key, bool enabled);
    /// @return true if the source has an entry, which was changed
    bool setService(const QString &key, const QJsonObject &service);
    /// Records that the server confirmed the stored copy current, when the validators are those of the copy
    /// @return true if the entry was changed
    bool confirm(const QString &key, const QString &etag, const QString &lastModified, const QDateTime &checkedAt, qint64 maxAgeSecs);
    /// Removes the files of the store which no entry refers to
    void removeUnreferencedFiles() const;
    /// Writes the entries of the sources with the keys, in this order. Does nothing when read only.
    bool save(const QStringList &keys, QString &errorString) const;

    static constexpr const char *indexFileName = "sources.json";
    static constexpr const char *legacyFileName = "geozones.json";
    static constexpr const char *legacySettingsGroup = "GeoZoneManager";
    static constexpr const char *legacySourceNameKey = "sourceName";
    static constexpr int version = 1;

private:
    GeoZoneStore _store;
    QHash<QString, Entry> _entries;     ///< By source key
    bool _readOnly = false;
};
