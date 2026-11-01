/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoRectangle>

#include "GeoZone.h"
#include "GeoZoneCatalog.h"
#include "GeoZoneDownloader.h"
#include "GeoZoneLoader.h"
#include "GeoZoneProviders.h"
#include "GeoZoneStore.h"

Q_DECLARE_LOGGING_CATEGORY(GeoZoneManagerLog)

class GeoZoneShape;
class GeoZoneSource;
class QmlObjectListModel;
class QTimer;

/// Loads UAS geographical zones from EUROCAE ED-269 / ED-318 files and national feeds for display on the
/// maps, mission checks, flight alerts and conversion to GeoFence exclusion areas. A copy of each source is
/// kept in the application data folder and reloaded on startup. Feeds are downloaded again periodically.
class GeoZoneManager : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("QmlObjectListModel.h")
    Q_MOC_INCLUDE("GeoZoneSource.h")

    Q_PROPERTY(QmlObjectListModel*  zones               READ zones              CONSTANT FINAL)                           ///< GeoZone objects of all sources
    Q_PROPERTY(QmlObjectListModel*  sources             READ sources            CONSTANT FINAL)                           ///< GeoZoneSource objects
    Q_PROPERTY(QGeoRectangle        boundingBox         READ boundingBox        NOTIFY boundingBoxChanged FINAL)                ///< Bounding box of all zones
    Q_PROPERTY(bool                 visible             READ visible            WRITE setVisible NOTIFY visibleChanged FINAL)
    Q_PROPERTY(bool                 busy                READ busy               NOTIFY busyChanged FINAL)                 ///< Sources are being loaded or downloaded
    Q_PROPERTY(double               progress            READ progress           NOTIFY progressChanged FINAL)             ///< Fraction of the downloads received, negative when unknown
    Q_PROPERTY(QString              progressText        READ progressText       NOTIFY progressChanged FINAL)
    Q_PROPERTY(QString              validityWarning     READ validityWarning    NOTIFY validityWarningChanged FINAL)      ///< Sources with outdated data
    Q_PROPERTY(QStringList          fileDialogFilters   READ fileDialogFilters  CONSTANT FINAL)
    Q_PROPERTY(QVariantList         feedPresets         READ feedPresets        CONSTANT FINAL)     ///< Maps with name and url of national feeds
    Q_PROPERTY(QVariantList         areaProviders       READ areaProviders      CONSTANT FINAL)     ///< See areaProviders()
    Q_PROPERTY(QDateTime            plannedStart        READ plannedStart       WRITE setPlannedStart NOTIFY plannedStartChanged FINAL)    ///< Planned flight start for mission checks, invalid to check zones whatever their schedule                           ///< Maps with name and url of national feeds

public:
    static constexpr int maxLabelCount = 200;           ///< Limits the number of label items created on a map

    enum class LoadMode : quint8 {
        Synchronous,
        Background,     ///< Parse files in a worker thread
    };

    /// @param storageDir Directory in which copies of the sources are kept. Empty for the default application data location.
    explicit GeoZoneManager(const QString &storageDir = QString(), QObject *parent = nullptr);
    ~GeoZoneManager();

    static GeoZoneManager *instance();
    static void registerQmlTypes();

    /// Loads the stored sources in the background, then downloads outdated feeds, warns about outdated data
    /// and refreshes feeds periodically
    void init();

    /// Loads an ED-269 or ED-318 file in the background, replacing the source loaded from a file with the same name.
    /// loadFailed is emitted on errors and sourceAdded on success.
    Q_INVOKABLE void addFile(const QString &fileName);

    /// Downloads an ED-269 or ED-318 feed from an http, https or file URL in the background, replacing the
    /// source with the same URL. loadFailed is emitted on errors and sourceAdded on success.
    Q_INVOKABLE void addUrl(const QString &url);

    /// Downloads all feeds again. Failures keep the previous data.
    Q_INVOKABLE void refreshFeeds();

    /// Removes a source, its zones and its stored copy
    Q_INVOKABLE void removeSource(int index);

    /// Removes all sources
    Q_INVOKABLE void clear();

    /// Synchronously loads a file, replacing the source loaded from a file with the same name
    bool loadFile(const QString &fileName, QString &errorString);

    /// Loads the sources stored by a previous session. An unreadable index is moved to a backup folder with the stored
    /// copies, a newer one leaves the store untouched for the session; storageFailed is emitted in both cases.
    void loadStoredSources(LoadMode mode);

    /// @return Name of the stored copy of a source in storageDir, empty when it has none
    QString storedFileName(const QString &key) const { return _storedEntries.value(key).fileName; }

    /// @return Zones to label in the map region: those centered in it, limited to maxLabelCount
    ///         preferring the most restrictive
    Q_INVOKABLE QList<QObject*> labelZones(const QGeoRectangle &region) const;

    /// @return Zones whose horizontal projection contains the coordinate at any altitude, the most restrictive and
    ///         active first
    Q_INVOKABLE QList<QObject*> zonesAt(const QGeoCoordinate &coordinate) const;

    /// @return Zones whose name or identifier contains the text, ignoring case, limited to maxResults. Names
    ///         starting with the text come first, then the most restrictive.
    Q_INVOKABLE QList<QObject*> searchZones(const QString &text, int maxResults = 50) const;

    /// Asks the visible map to show the zone and its details
    Q_INVOKABLE void showZone(QObject *zone) { if (qobject_cast<GeoZone*>(zone)) { emit showZoneRequested(zone); } }

    /// @return Zones matching one of the restrictions which intersect the area. An invalid area matches all zones.
    /// @param activeFrom When valid, only zones active at this time or, with a valid activeTo, at any time until activeTo
    QList<GeoZone*> zonesInArea(const QList<GeoZone::Restriction> &restrictions, const QGeoRectangle &area,
                                const QDateTime &activeFrom = QDateTime(), const QDateTime &activeTo = QDateTime()) const
    {
        return _catalog.zonesInArea(restrictions, area, activeFrom, activeTo);
    }

    /// @return Zones matching one of the restrictions which one of the paths enters, see GeoZone::intersectsPath
    QList<GeoZone*> zonesCrossingPaths(const QList<GeoZone::Restriction> &restrictions, const QList<QList<GeoZone::Position>> &paths) const
    {
        return _catalog.zonesCrossingPaths(restrictions, paths);
    }

    /// @return Zones matching one of the restrictions which overlap one of the areas, see GeoZone::intersectsArea
    QList<GeoZone*> zonesIntersectingAreas(const QList<GeoZone::Restriction> &restrictions, const QList<GeoZone::Area> &areas) const
    {
        return _catalog.zonesIntersectingAreas(restrictions, areas);
    }

    QmlObjectListModel *zones() const { return _zones; }
    QmlObjectListModel *sources() const { return _sources; }
    GeoZoneSource *source(const QString &key) const;
    /// Polygons and circles of all zones for map display
    const QList<GeoZoneShape*> &shapes() const { return _catalog.shapes(); }
    /// @return Shapes of the zones whose bounding box intersects the bounds in degrees
    QList<const GeoZoneShape*> shapesInArea(double south, double west, double north, double east) const { return _catalog.shapesInArea(south, west, north, east); }
    QGeoRectangle boundingBox() const { return _catalog.boundingBox(); }
    bool visible() const;
    void setVisible(bool visible);
    bool busy() const { return _pendingCount > 0; }
    double progress() const;
    QString progressText() const;
    QString validityWarning() const { return _validityWarning; }
    QStringList fileDialogFilters() const;
    static QVariantList feedPresets() { return GeoZoneProviders::feedPresets(); }
    /// See GeoZoneProviders::areaProviders
    static QVariantList areaProviders() { return GeoZoneProviders::areaProviders(); }
    /// See GeoZoneProviders::areaFeedUrl
    Q_INVOKABLE QString areaFeedUrl(const QString &provider, const QGeoRectangle &area) const { return GeoZoneProviders::areaFeedUrl(provider, area); }
    /// Adds a feed downloading the zones of the provider in the area, like the area of a map, which is then kept for
    /// offline use and refreshed like other feeds. loadFailed is emitted when the area is too large.
    Q_INVOKABLE bool addArea(const QString &provider, const QGeoRectangle &area);

    QString storageDir() const { return _store.dir(); }

    QDateTime plannedStart() const { return _plannedStart; }
    void setPlannedStart(const QDateTime &plannedStart);
    /// Checks missions against zones whatever their schedule
    Q_INVOKABLE void clearPlannedStart() { setPlannedStart(QDateTime()); }

signals:
    /// The zones and shapes of the source are about to be deleted, because it is reloaded or removed
    void zonesAboutToChange(GeoZoneSource *source);
    /// The zones of the source changed, zones() and the queries include them unless it was removed
    void zonesChanged(GeoZoneSource *source);
    void boundingBoxChanged();
    void showZoneRequested(QObject *zone);
    void plannedStartChanged();
    void visibleChanged(bool visible);
    void busyChanged(bool busy);
    void progressChanged();
    void validityWarningChanged();
    void sourceAdded(GeoZoneSource *source);    ///< A source added by addFile or addUrl was loaded, not emitted when feeds are refreshed
    void loadFailed(const QString &key, const QString &message);   ///< Loading or downloading a source requested by the user failed
    void storageFailed(const QString &message); ///< Stored copies can't be read or written, emitted once per session
    void outdatedDataFound(const QString &message); ///< Some sources have outdated data after the startup refresh

private slots:
    void _updateActive();
    void _refreshDueFeeds();

private:
    void _prepareJob(GeoZoneLoader::Job &job) const;
    void _startLoad(GeoZoneLoader::Job job, LoadMode mode);
    void _applyLoad(const GeoZoneLoader::Job &job, const GeoZoneLoader::Result &loadResult);
    void _updateStoredEntry(const GeoZoneLoader::Job &job, const GeoZoneLoader::Result &loadResult);
    void _download(const QString &url, GeoZoneLoader::Request request);
    void _downloadComplete(const GeoZoneDownloader::Result &download, const QString &url, int generation, GeoZoneLoader::Request request);
    void _idle();
    void _setPending(int delta);
    void _beginZonesChange(GeoZoneSource *source);
    void _endZonesChange(GeoZoneSource *source);
    void _saveIndex();
    void _reportStorageError(const QString &message);
    void _updateValidity();

    GeoZoneStore _store;
    GeoZoneDownloader *_downloader = nullptr;
    QmlObjectListModel *_zones = nullptr;
    QmlObjectListModel *_sources = nullptr;
    GeoZoneCatalog _catalog;            ///< Same zones as _zones, for queries
    QTimer *_activeTimer = nullptr;
    QTimer *_refreshTimer = nullptr;
    int _pendingCount = 0;
    QHash<QString, int> _generations;   ///< Latest load per source key, older results are dropped
    QHash<QString, GeoZoneStore::Entry> _storedEntries; ///< Stored copy of each source as written, by source key
    bool _storeReadOnly = false;        ///< The index is from a newer version, keep it as is
    bool _storageErrorReported = false;
    QDateTime _plannedStart;
    QString _validityWarning;
    bool _startupRefreshPending = false;
    bool _startupCheckPending = false;

    static constexpr int _activeUpdateIntervalMSecs = 60 * 1000;
    static constexpr int _refreshCheckIntervalMSecs = 5 * 60 * 1000;
};
