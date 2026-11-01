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
#include "GeoZoneProviders.h"
#include "GeoZoneSourceRepository.h"

Q_DECLARE_LOGGING_CATEGORY(GeoZoneManagerLog)

class GeoZoneShape;
class GeoZoneSource;
class QmlObjectListModel;
class QTimer;

/// UAS geographical zones of the sources of GeoZoneSourceRepository, for display on the maps, mission checks, flight
/// alerts and conversion to GeoFence exclusion areas, with the queries they use.
class GeoZoneManager : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("QmlObjectListModel.h")
    Q_MOC_INCLUDE("GeoZoneSource.h")

    Q_PROPERTY(int                  zoneCount           READ zoneCount          NOTIFY zoneCountChanged FINAL)            ///< Zones of all sources
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

    using LoadMode = GeoZoneSourceRepository::LoadMode;

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
    Q_INVOKABLE void addFile(const QString &fileName) { _repository->addFile(fileName); }

    /// Downloads an ED-269 or ED-318 feed from an http, https or file URL in the background, replacing the
    /// source with the same URL. loadFailed is emitted on errors and sourceAdded on success.
    Q_INVOKABLE void addUrl(const QString &url) { _repository->addUrl(url); }

    /// Downloads all feeds again. Failures keep the previous data.
    Q_INVOKABLE void refreshFeeds() { _repository->refreshFeeds(); }

    /// Removes a source, its zones and its stored copy
    Q_INVOKABLE void removeSource(int index) { _repository->removeSource(index); }

    /// Removes all sources
    Q_INVOKABLE void clear() { _repository->clear(); }

    /// Synchronously loads a file, replacing the source loaded from a file with the same name
    bool loadFile(const QString &fileName, QString &errorString) { return _repository->loadFile(fileName, errorString); }

    /// Loads the sources stored by a previous session. An unreadable index is moved to a backup folder with the stored
    /// copies, a newer one leaves the store untouched for the session; storageFailed is emitted in both cases.
    void loadStoredSources(LoadMode mode) { _repository->loadStoredSources(mode); }

    /// @return Name of the stored copy of a source in storageDir, empty when it has none
    QString storedFileName(const QString &key) const { return _repository->storedFileName(key); }

    /// @return Zones to label in the map region: those centered in it, limited to maxLabelCount
    ///         preferring the most restrictive
    Q_INVOKABLE QVariantList labelZones(const QGeoRectangle &region) const;

    /// @return Zones whose horizontal projection contains the coordinate at any altitude, the most restrictive and
    ///         active first
    Q_INVOKABLE QVariantList zonesAt(const QGeoCoordinate &coordinate) const;

    /// @return Zones whose name or identifier contains the text, ignoring case, limited to maxResults. Names
    ///         starting with the text come first, then the most restrictive.
    Q_INVOKABLE QVariantList searchZones(const QString &text, int maxResults = 50) const;

    /// Asks the visible map to show the zone and its details
    Q_INVOKABLE void showZone(const QVariant &zone);

    int zoneCount() const { return static_cast<int>(_catalog.zones().count()); }
    /// Zones of all sources, in the order of the sources
    const QList<GeoZone*> &zoneList() const { return _catalog.zones(); }
    /// Zones of all sources with their spatial index
    const GeoZoneCatalog &catalog() const { return _catalog; }
    QmlObjectListModel *sources() const { return _repository->sources(); }
    GeoZoneSource *source(const QString &key) const { return _repository->source(key); }
    GeoZoneSourceRepository *repository() const { return _repository; }
    /// Polygons and circles of all zones for map display
    const QList<GeoZoneShape*> &shapes() const { return _catalog.shapes(); }
    /// @return Shapes of the zones whose bounding box intersects the bounds in degrees
    QList<const GeoZoneShape*> shapesInArea(double south, double west, double north, double east) const { return _catalog.shapesInArea(south, west, north, east); }
    QGeoRectangle boundingBox() const { return _catalog.boundingBox(); }
    bool visible() const;
    void setVisible(bool visible);
    bool busy() const { return _repository->busy(); }
    double progress() const { return _repository->progress(); }
    QString progressText() const { return _repository->progressText(); }
    QString validityWarning() const { return _repository->validityWarning(); }
    QStringList fileDialogFilters() const;
    static QVariantList feedPresets() { return GeoZoneProviders::feedPresets(); }
    /// See GeoZoneProviders::areaProviders
    static QVariantList areaProviders() { return GeoZoneProviders::areaProviders(); }
    /// See GeoZoneProviders::areaFeedUrl
    Q_INVOKABLE QString areaFeedUrl(const QString &provider, const QGeoRectangle &area) const { return GeoZoneProviders::areaFeedUrl(provider, area); }
    /// Adds a feed downloading the zones of the provider in the area, like the area of a map, which is then kept for
    /// offline use and refreshed like other feeds. loadFailed is emitted when the area is too large.
    Q_INVOKABLE bool addArea(const QString &provider, const QGeoRectangle &area);

    QString storageDir() const { return _repository->storageDir(); }

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
    void showZoneRequested(const QVariant &zone);   ///< GeoZoneInfo
    void zoneCountChanged();
    /// Zones became active or inactive with the time, see GeoZone::active
    void activeZonesChanged();
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

private:
    void _endZonesChange(GeoZoneSource *source);

    GeoZoneSourceRepository *_repository = nullptr;
    GeoZoneCatalog _catalog;
    QTimer *_activeTimer = nullptr;
    QDateTime _plannedStart;

    static constexpr int _activeUpdateIntervalMSecs = 60 * 1000;
};
