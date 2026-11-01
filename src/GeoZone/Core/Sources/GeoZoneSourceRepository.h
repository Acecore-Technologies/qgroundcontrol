/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>

#include "GeoZoneCancelToken.h"
#include "GeoZoneDownloader.h"
#include "GeoZoneFeedFetch.h"
#include "GeoZoneLoader.h"
#include "GeoZoneSourceIndex.h"
#include "GeoZoneStore.h"

class GeoZoneSource;
class QTimer;

/// The geozone sources: adds, downloads, loads and removes them, keeps a copy of each for the next start,
/// refreshes the feeds and reports outdated data. GeoZoneManager publishes the zones of the loaded sources.
class GeoZoneSourceRepository : public QObject
{
    Q_OBJECT

public:
    enum class LoadMode : quint8 {
        Synchronous,
        Background,     ///< Parse files in a worker thread
    };

    /// @param storageDir Directory in which copies of the sources are kept
    explicit GeoZoneSourceRepository(const QString &storageDir, QObject *parent = nullptr);
    ~GeoZoneSourceRepository() override;

    /// Loads the stored sources in the background, then downloads outdated feeds, warns about outdated data
    /// and refreshes feeds periodically
    void init();

    /// See GeoZoneManager::addFile
    void addFile(const QString &fileName);
    /// See GeoZoneManager::addUrl
    void addUrl(const QString &url);
    /// Downloads all feeds again. Failures keep the previous data.
    void refreshFeeds();
    /// Removes a source, its stored copy, and cancels its downloads
    void removeSource(int index);
    void clear();
    /// Synchronously loads a file, replacing the source loaded from the same file
    bool loadFile(const QString &fileName, QString &errorString);
    /// See GeoZoneManager::loadStoredSources
    void loadStoredSources(LoadMode mode);

    /// The sources, in the order they were added
    const QList<GeoZoneSource*> &sources() const { return _sources; }
    QList<const GeoZoneSource*> sourceList() const;
    GeoZoneSource *source(const QString &key) const;
    /// @return Name of the stored copy of a source in storageDir, empty when it has none
    QString storedFileName(const QString &key) const { return _index.fileName(key); }
    QString storageDir() const { return _store.dir(); }

    bool busy() const { return _pendingCount > 0; }
    double progress() const;
    QString progressText() const;
    /// Sources with outdated data, one per line
    QString validityWarning() const { return _validityWarning; }
    /// Re-evaluates the age of the sources' data, also when GeoZoneEnvironment::sourceSettings changed
    void updateValidity();

signals:
    /// The zones of the source are about to be replaced, or the source removed
    void sourceAboutToChange(GeoZoneSource *source);
    /// The zones of the source were replaced, or the source was removed from sources()
    void sourceChanged(GeoZoneSource *source);
    void sourceAdded(GeoZoneSource *source);    ///< A source added by addFile or addUrl was loaded, not emitted when feeds are refreshed
    void sourceAppended(GeoZoneSource *source); ///< The source was appended to sources(), before it is loaded
    void sourceRemoved(int index);              ///< The source at the index was removed from sources(), it is deleted later
    /// A source the user added or refreshed was loaded, its zones should be shown
    void userLoaded();
    void loadFailed(const QString &key, const QString &message);
    void storageFailed(const QString &message);
    void outdatedDataFound(const QString &message);
    void busyChanged(bool busy);
    void progressChanged();
    void validityWarningChanged();

private:
    void _refreshDueFeeds();
    void _prepareJob(GeoZoneLoader::Job &job) const;
    void _startLoad(GeoZoneLoader::Job job, LoadMode mode);
    void _applyLoad(const GeoZoneLoader::Job &job, const GeoZoneLoader::Result &loadResult);
    void _updateStoredEntry(const GeoZoneLoader::Job &job, const GeoZoneLoader::Result &loadResult);
    void _download(const QString &url, GeoZoneLoader::Request request);
    void _fetchFinished(GeoZoneFeedFetch *fetch, const GeoZoneFeedFetch::Result &result, GeoZoneLoader::Request request);
    /// Cancels the fetch or load of the source in progress
    /// @return Token of the fetch or load replacing it
    GeoZoneCancelToken _beginOperation(const QString &key);
    /// Forgets the token of a fetch or load which finished, if it is the source's current one
    void _endOperation(const QString &key, const GeoZoneCancelToken &token);
    void _cancelOperation(const QString &key);
    void _idle();
    void _setPending(int delta);
    void _saveIndex();
    /// Appends a source to the list, enabled as stored
    GeoZoneSource *_newSource(const QString &key, const QString &url, bool enabled = true);
    void _removeLegacyFileSource(const GeoZoneLoader::Job &job);
    void _sourceEnabledChanged(GeoZoneSource *source);
    void _reportStorageError(const QString &message);

    GeoZoneStore _store;
    GeoZoneSourceIndex _index;
    GeoZoneDownloader *_downloader = nullptr;
    QList<GeoZoneSource*> _sources;
    QTimer *_refreshTimer = nullptr;
    int _pendingCount = 0;
    QHash<QString, GeoZoneCancelToken> _operations; ///< Fetch or load in progress per source key, earlier ones were canceled
    QHash<QString, GeoZoneFeedFetch*> _fetches;     ///< Feeds being downloaded, by URL
    QHash<QString, GeoZoneParser::Companions> _companions;  ///< Companion documents of the feeds by URL, see GeoZoneFormat::companionRequests
    bool _storageErrorReported = false;
    QString _validityWarning;
    bool _startupRefreshPending = false;
    bool _startupCheckPending = false;

    static constexpr int _refreshCheckIntervalMSecs = 5 * 60 * 1000;
};
