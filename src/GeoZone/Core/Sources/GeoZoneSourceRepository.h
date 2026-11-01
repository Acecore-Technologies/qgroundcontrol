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
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include "GeoZoneCancelToken.h"
#include "GeoZoneCredentials.h"
#include "GeoZoneDownloader.h"
#include "GeoZoneFeedFetch.h"
#include "GeoZoneLoader.h"
#include "GeoZoneService.h"
#include "GeoZoneServiceRegistry.h"
#include "GeoZoneSourceIndex.h"
#include "GeoZoneStore.h"

class GeoZoneRefreshScheduler;
class GeoZoneSource;

/// The geozone sources: adds, loads and removes them and reports outdated data. Coordinates the downloads of the
/// feeds (GeoZoneFeedFetch), their refreshes (GeoZoneRefreshScheduler) and the copies kept for the next start
/// (GeoZoneStore, GeoZoneSourceIndex). GeoZoneManager publishes the zones of the loaded sources.
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
    /// Adds a feed requested from a service taking credentials, a POST query or the area of interest, replacing the
    /// source with the same URL. The URL may be a template, see GeoZoneService. The secret kept for the source is
    /// forgotten when the service sends it elsewhere than before, see GeoZoneService::sameAuthTarget.
    /// @param secret Kept by credentials(), the one kept is used when empty
    /// @param allowOtherOrigins The user confirmed that the token or SHA-256 URLs may be on another server than the
    ///                          feed, see GeoZoneService::otherOrigins. The service is refused otherwise.
    void addService(const QString &url, const GeoZoneService &service, const QString &secret, bool allowOtherOrigins = false);
    /// Replaces the secret of the service of a source and downloads it again
    void setSecret(const QString &key, const QString &secret);
    /// Keeps the secrets of the services, in the application settings by default
    GeoZoneCredentials &credentials() { return _services.credentials(); }

    /// Sets the area of a planned mission, part of the area of interest of the services taking it, see
    /// GeoZoneService::areaOfInterest. Feeds whose downloaded area doesn't cover the area of interest are downloaded
    /// again for it, with areaMargin() around.
    /// @param owner Identifies the mission, like the mission check following it
    /// @param area Invalid when the mission has no flight path
    void setMissionArea(quintptr owner, const QGeoRectangle &area);
    /// Sets the position of the active vehicle, part of the area of interest, invalid without vehicle
    void setVehiclePosition(const QGeoCoordinate &position);
    /// @return Bounds of the planned missions and the vehicle, invalid without either
    QGeoRectangle areaOfInterest() const;
    /// Meters downloaded around the area of interest
    double areaMargin() const { return _areaMargin; }
    void setAreaMargin(double meters);
    /// When false, the feeds keep the area they were downloaded for
    bool followArea() const { return _followArea; }
    void setFollowArea(bool follow);
    /// @return The area with a margin in meters around it
    static QGeoRectangle expanded(const QGeoRectangle &area, double margin);

    static constexpr double defaultAreaMargin = 5000;

    /// @return Description of a source for briefings and offline packs: its origin, dates, service, provenance, the
    ///         changes of its last reload (lastChanges, see GeoZoneSourceChanges) and its AIRAC cycle. Holds no secrets.
    static QJsonObject sourceReport(const GeoZoneSource &source);
    /// @return The reports of the sources, see sourceReport, and when it was made
    QJsonObject provenanceReport() const;

    /// Writes an offline pack of the stored copies of the sources with the keys, see GeoZonePack. Secrets are left out.
    /// @return false when it can't be written, errorString tells why
    bool exportPack(const QStringList &keys, const QString &packPath, QString &errorString) const;
    /// Imports the sources of an offline pack, replacing those with the same keys. They are loaded in the background,
    /// keep their dates, validators and provenance, marked as imported, and are refreshed like other sources. Sources
    /// already here keep their service and secret. New sources take the service of the pack without its custom
    /// headers and without a secret; those whose service sends requests to another server than their feed are left
    /// out.
    /// @return Number of sources imported, negative when the pack can't be read. errorString tells why, or which
    ///         sources were left out.
    int importPack(const QString &packPath, QString &errorString);
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
    /// Decides when the feeds are refreshed
    GeoZoneRefreshScheduler *scheduler() const { return _scheduler; }

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
    /// Sets the service of a source, forgetting its secret and token when the service sends them elsewhere
    void _setService(const QString &key, const GeoZoneService &service);
    void _updateCredentialsMissing(const QString &key);
    /// @return Area to download a feed taking the area of interest for: the area it was downloaded for while it
    ///         covers the area of interest, otherwise the area of interest with the margin
    QGeoRectangle _requestArea(const QString &key) const;
    /// Downloads the feeds taking the area of interest again whose area doesn't cover it
    void _checkAreas();

    GeoZoneStore _store;
    GeoZoneSourceIndex _index;
    GeoZoneDownloader *_downloader = nullptr;
    QList<GeoZoneSource*> _sources;
    GeoZoneRefreshScheduler *_scheduler = nullptr;
    int _pendingCount = 0;
    QHash<QString, GeoZoneCancelToken> _operations; ///< Fetch or load in progress per source key, earlier ones were canceled
    QHash<QString, GeoZoneFeedFetch*> _fetches;     ///< Feeds being downloaded, by URL
    QHash<QString, GeoZoneParser::Companions> _companions;  ///< Companion documents of the feeds by URL, see GeoZoneFormat::companionRequests
    GeoZoneServiceRegistry _services;   ///< Of the feeds requested from a service, with their secrets and tokens
    QHash<quintptr, QGeoRectangle> _missionAreas;
    QGeoCoordinate _vehiclePosition;
    double _areaMargin = defaultAreaMargin;
    bool _followArea = true;
    bool _storageErrorReported = false;
    QString _validityWarning;
    bool _startupRefreshPending = false;
    bool _startupCheckPending = false;
};
