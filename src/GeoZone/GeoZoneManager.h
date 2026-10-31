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
#include "GeoZoneDownloader.h"
#include "GeoZoneIndex.h"
#include "GeoZoneParser.h"
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

    Q_PROPERTY(QmlObjectListModel*  zones               READ zones              CONSTANT)                           ///< GeoZone objects of all sources
    Q_PROPERTY(QmlObjectListModel*  sources             READ sources            CONSTANT)                           ///< GeoZoneSource objects
    Q_PROPERTY(QGeoRectangle        boundingBox         READ boundingBox        NOTIFY zonesChanged)                ///< Bounding box of all zones
    Q_PROPERTY(bool                 visible             READ visible            WRITE setVisible NOTIFY visibleChanged)
    Q_PROPERTY(bool                 busy                READ busy               NOTIFY busyChanged)                 ///< Sources are being loaded or downloaded
    Q_PROPERTY(double               progress            READ progress           NOTIFY progressChanged)             ///< Fraction of the downloads received, negative when unknown
    Q_PROPERTY(QString              progressText        READ progressText       NOTIFY progressChanged)
    Q_PROPERTY(QString              validityWarning     READ validityWarning    NOTIFY validityWarningChanged)      ///< Sources with outdated data
    Q_PROPERTY(QStringList          fileDialogFilters   READ fileDialogFilters  CONSTANT)
    Q_PROPERTY(QVariantList         feedPresets         READ feedPresets        CONSTANT)                           ///< Maps with name and url of national feeds

public:
    static constexpr int maxLabelCount = 200;           ///< Limits the number of label items created on a map

    /// @param storageDir Directory in which copies of the sources are kept. Empty for the default application data location.
    explicit GeoZoneManager(const QString &storageDir = QString(), QObject *parent = nullptr);
    ~GeoZoneManager();

    static GeoZoneManager *instance();
    static void registerQmlTypes();

    /// Loads the stored sources in the background, then downloads outdated feeds, warns about outdated data
    /// and refreshes feeds periodically
    void init();

    /// Loads an ED-269 or ED-318 file in the background, replacing the source loaded from a file with the same name.
    /// Errors are shown to the user and sourceAdded is emitted on success.
    Q_INVOKABLE void addFile(const QString &fileName);

    /// Downloads an ED-269 or ED-318 feed from an http, https or file URL in the background, replacing the
    /// source with the same URL. Errors are shown to the user and sourceAdded is emitted on success.
    Q_INVOKABLE void addUrl(const QString &url);

    /// Downloads all feeds again. Failures keep the previous data.
    Q_INVOKABLE void refreshFeeds();

    /// Removes a source, its zones and its stored copy
    Q_INVOKABLE void removeSource(int index);

    /// Removes all sources
    Q_INVOKABLE void clear();

    /// Synchronously loads a file, replacing the source loaded from a file with the same name
    bool loadFile(const QString &fileName, QString &errorString);

    /// Loads the sources stored by a previous session
    /// @param background Parse the files in a background thread
    void loadStoredSources(bool background);

    /// @return Zones to label in the map region: those centered in it, limited to maxLabelCount
    ///         preferring the most restrictive
    Q_INVOKABLE QList<QObject*> labelZones(const QGeoRectangle &region) const;

    /// @return Zones matching one of the restrictions which intersect the area. An invalid area matches all zones.
    /// @param activeFrom When valid, only zones active at this time or, with a valid activeTo, at any time until activeTo
    QList<GeoZone*> zonesInArea(const QList<GeoZone::Restriction> &restrictions, const QGeoRectangle &area,
                                const QDateTime &activeFrom = QDateTime(), const QDateTime &activeTo = QDateTime()) const;

    /// @return Zones matching one of the restrictions which one of the paths enters, see GeoZone::intersectsPath
    QList<GeoZone*> zonesCrossingPaths(const QList<GeoZone::Restriction> &restrictions, const QList<QList<GeoZone::Position>> &paths) const;

    /// @return Zones matching one of the restrictions which overlap one of the areas, see GeoZone::intersectsArea
    QList<GeoZone*> zonesIntersectingAreas(const QList<GeoZone::Restriction> &restrictions, const QList<GeoZone::Area> &areas) const;

    QmlObjectListModel *zones() const { return _zones; }
    QmlObjectListModel *sources() const { return _sources; }
    GeoZoneSource *source(const QString &key) const;
    /// Polygons and circles of all zones for map display
    const QList<GeoZoneShape*> &shapes() const { return _shapes; }
    QGeoRectangle boundingBox() const { return _boundingBox; }
    bool visible() const;
    void setVisible(bool visible);
    bool busy() const { return _pendingCount > 0; }
    double progress() const;
    QString progressText() const;
    QString validityWarning() const { return _validityWarning; }
    QStringList fileDialogFilters() const;
    static QVariantList feedPresets();

    QString storageDir() const { return _store.dir(); }

signals:
    void zonesAboutToChange();  ///< Zones and shapes are about to be deleted
    void zonesChanged();
    void visibleChanged(bool visible);
    void busyChanged(bool busy);
    void progressChanged();
    void validityWarningChanged();
    void sourceAdded(GeoZoneSource *source);    ///< A source added by addFile or addUrl was loaded, not emitted when feeds are refreshed

private slots:
    void _updateActive();
    void _refreshDueFeeds();

private:
    /// Loads a file into a source on a worker thread
    struct LoadJob {
        QString key;
        QString url;
        QString sourcePath;         ///< File to parse when there is no data
        QByteArray data;            ///< Downloaded content to parse
        QString storedFileName;     ///< Name of the copy to write in the storage directory, empty to not copy
        QDateTime loadedAt;
        QDateTime fileDate;
        GeoZoneParser::Format formatHint = GeoZoneParser::Format::Unknown;
        QString etag;               ///< HTTP validators of a downloaded feed
        QString lastModified;
        bool userInitiated = false;
        bool added = false;         ///< Added by addFile or addUrl, not refreshed
        int generation = 0;
    };
    struct LoadResult {
        GeoZoneParser::Result result;
        QString errorString;
    };

    static LoadResult _runLoad(const LoadJob &job, const GeoZoneStore &store);
    void _startLoad(LoadJob job, bool background);
    void _applyLoad(const LoadJob &job, const LoadResult &loadResult);
    void _setStoredFileName(GeoZoneSource *source, const QString &storedFileName);
    void _download(const QString &url, bool userInitiated, bool added = false);
    void _downloadComplete(const GeoZoneDownloader::Result &download, const QString &url, int generation, bool userInitiated, bool added);
    void _idle();
    void _setPending(int delta);
    void _beginZonesChange();
    void _endZonesChange();
    void _saveIndex() const;
    void _updateValidity();

    GeoZoneStore _store;
    GeoZoneDownloader *_downloader = nullptr;
    QmlObjectListModel *_zones = nullptr;
    QmlObjectListModel *_sources = nullptr;
    QList<GeoZoneShape*> _shapes;
    QList<GeoZone*> _zoneList;          ///< Same as _zones, indexed by _index
    GeoZoneIndex _index;
    QTimer *_activeTimer = nullptr;
    QTimer *_refreshTimer = nullptr;
    QGeoRectangle _boundingBox;
    int _pendingCount = 0;
    bool _changingZones = false;
    QHash<QString, int> _generations;   ///< Latest load per source key, older results are dropped
    QString _validityWarning;
    bool _startupRefreshPending = false;
    bool _startupCheckPending = false;

    static constexpr int _activeUpdateIntervalMSecs = 60 * 1000;
    static constexpr int _refreshCheckIntervalMSecs = 5 * 60 * 1000;
};
