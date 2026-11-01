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
#include "GeoZoneSettings.h"
#include "GeoZoneSourceRepository.h"

Q_DECLARE_LOGGING_CATEGORY(GeoZoneManagerLog)

class GeoZoneDisplay;
class GeoZonePlanning;
class GeoZoneShape;
class GeoZoneSource;
class QmlObjectListModel;
class QTimer;
class Vehicle;

/// UAS geographical zones of the sources of GeoZoneSourceRepository, for display on the maps, mission checks, flight
/// alerts and conversion to GeoFence exclusion areas, with the queries they use.
class GeoZoneManager : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("QmlObjectListModel.h")
    Q_MOC_INCLUDE("GeoZoneSource.h")
    Q_MOC_INCLUDE("GeoZoneDisplay.h")
    Q_MOC_INCLUDE("GeoZonePlanning.h")

    Q_PROPERTY(int                  zoneCount           READ zoneCount          NOTIFY zoneCountChanged FINAL)            ///< Zones of all sources
    Q_PROPERTY(QmlObjectListModel*  sources             READ sources            CONSTANT FINAL)                           ///< GeoZoneSource objects
    Q_PROPERTY(GeoZoneDisplay*      display             READ display            CONSTANT FINAL)                           ///< Which zones the maps draw
    Q_PROPERTY(GeoZonePlanning*     planning            READ planning           CONSTANT FINAL)                           ///< Planned flight start and authorisations for mission checks
    Q_PROPERTY(GeoZoneSettings*     settings            READ settings           CONSTANT FINAL)
    Q_PROPERTY(QGeoRectangle        boundingBox         READ boundingBox        NOTIFY boundingBoxChanged FINAL)                ///< Bounding box of all zones
    Q_PROPERTY(int                  disabledSourceCount READ disabledSourceCount NOTIFY zoneCountChanged FINAL)           ///< Sources whose zones are left out
    Q_PROPERTY(bool                 busy                READ busy               NOTIFY busyChanged FINAL)                 ///< Sources are being loaded or downloaded
    Q_PROPERTY(double               progress            READ progress           NOTIFY progressChanged FINAL)             ///< Fraction of the downloads received, negative when unknown
    Q_PROPERTY(QString              progressText        READ progressText       NOTIFY progressChanged FINAL)
    Q_PROPERTY(QString              validityWarning     READ validityWarning    NOTIFY validityWarningChanged FINAL)      ///< Sources with outdated data
    Q_PROPERTY(QStringList          fileDialogFilters   READ fileDialogFilters  CONSTANT FINAL)
    Q_PROPERTY(QVariantList         feedPresets         READ feedPresets        CONSTANT FINAL)     ///< Maps with name and url of national feeds
    Q_PROPERTY(QVariantList         areaProviders       READ areaProviders      CONSTANT FINAL)     ///< See areaProviders()
    Q_PROPERTY(QVariantList         serviceTemplates    READ serviceTemplates   CONSTANT FINAL)     ///< See GeoZoneProviders::serviceTemplateList
    Q_PROPERTY(QString              credentialStoreName READ credentialStoreName CONSTANT FINAL)    ///< Where the secrets of the services are kept, see GeoZoneCredentials
    Q_PROPERTY(bool                 packSigningAvailable READ packSigningAvailable CONSTANT FINAL)  ///< The build can sign packs and check their signatures
    Q_PROPERTY(QString              packPublicKey       READ packPublicKey      NOTIFY packSigningChanged FINAL)   ///< Base64 public key of the pack signing key, empty without
    Q_PROPERTY(QString              packKeyId           READ packKeyId          NOTIFY packSigningChanged FINAL)   ///< See GeoZonePackSignature::keyId
    Q_PROPERTY(QVariantList         trustedPublishers   READ trustedPublishers  NOTIFY trustedPublishersChanged FINAL) ///< Maps with name, publicKey (base64) and keyId

public:
    static constexpr int maxLabelCount = 200;           ///< Limits the number of label items created on a map

    using LoadMode = GeoZoneSourceRepository::LoadMode;

    /// @param storageDir Directory in which copies of the sources are kept. Empty for the default application data location.
    explicit GeoZoneManager(const QString &storageDir = QString(), QObject *parent = nullptr);
    ~GeoZoneManager();

    static GeoZoneManager *instance();

    /// Loads the stored sources in the background, then downloads outdated feeds, warns about outdated data
    /// and refreshes feeds periodically
    void init();

    /// Loads an ED-269 or ED-318 file in the background, replacing the source loaded from the same file.
    /// loadFailed is emitted on errors and sourceAdded on success.
    Q_INVOKABLE void addFile(const QString &fileName) { _repository->addFile(fileName); }

    /// Downloads an ED-269 or ED-318 feed from an http, https or file URL in the background, replacing the
    /// source with the same URL. loadFailed is emitted on errors and sourceAdded on success.
    Q_INVOKABLE void addUrl(const QString &url) { _repository->addUrl(url); }

    /// Downloads a feed from a service taking credentials, an ED-318 query or the area of interest, see
    /// GeoZoneSourceRepository::addService
    /// @param service Fields of GeoZoneService::toVariantMap
    /// @param secret API key, token, password or client secret, kept in the settings, the one kept when empty
    /// @param allowOtherOrigins The user confirmed the other servers of serviceOtherOrigins
    Q_INVOKABLE void addService(const QString &url, const QVariantMap &service, const QString &secret, bool allowOtherOrigins = false)
    {
        _repository->addService(url, GeoZoneService::fromVariantMap(service), secret, allowOtherOrigins);
    }

    /// @return Origins of the token and SHA-256 URLs of the service which aren't the feed's, which the user confirms
    ///         before addService, see GeoZoneService::otherOrigins
    Q_INVOKABLE QStringList serviceOtherOrigins(const QString &url, const QVariantMap &service) const
    {
        return GeoZoneService::fromVariantMap(service).otherOrigins(url.trimmed());
    }

    /// Replaces the secret of the service of a source and downloads it again
    Q_INVOKABLE void setCredentials(const QString &key, const QString &secret) { _repository->setSecret(key, secret); }

    /// Downloads all feeds again. Failures keep the previous data.
    Q_INVOKABLE void refreshFeeds() { _repository->refreshFeeds(); }

    /// @return Where and how the data of the sources was obtained, see GeoZoneSourceRepository::provenanceReport
    QJsonObject provenanceReport() const { return _repository->provenanceReport(); }
    /// Writes provenanceReport as a JSON file, for briefings. fileOperationFinished tells the outcome.
    Q_INVOKABLE bool exportProvenance(const QString &fileName);

    /// Writes an offline pack of the sources, see GeoZonePack. fileOperationFinished tells the outcome.
    /// @param keys Of the sources, those turned on when empty
    Q_INVOKABLE bool exportPack(const QString &fileName, const QStringList &keys = QStringList());
    /// Imports the sources of an offline pack, loaded in the background. fileOperationFinished tells the outcome and
    /// how the pack is signed.
    Q_INVOKABLE bool importPack(const QString &fileName);

    /// Trusts the signed packs of a publisher, see GeoZoneSettings::trustedPackPublishers
    /// @param publicKey Ed25519 public key, see GeoZonePackSignature::parsePublicKey
    /// @return false when the key isn't valid, fileOperationFinished then tells why
    Q_INVOKABLE bool trustPublisher(const QString &name, const QString &publicKey);
    /// No longer trusts the publisher with the base64 public key
    Q_INVOKABLE void distrustPublisher(const QString &publicKey);
    /// Trusts the publisher of the pack a source was imported from, with the name the pack gives
    Q_INVOKABLE bool trustSourcePublisher(const QString &sourceKey);
    /// Writes a new Ed25519 private key to sign packs with, and selects it. fileOperationFinished tells the outcome.
    Q_INVOKABLE bool createSigningKey(const QString &fileName);

    /// Removes a source, its zones and its stored copy
    Q_INVOKABLE void removeSource(int index) { _repository->removeSource(index); }

    /// Removes all sources
    Q_INVOKABLE void clear() { _repository->clear(); }

    /// Synchronously loads a file, replacing the source loaded from the same file
    bool loadFile(const QString &fileName, QString &errorString) { return _repository->loadFile(fileName, errorString); }

    /// Loads the sources stored by a previous session. An unreadable index is moved to a backup folder with the stored
    /// copies, a newer one leaves the store untouched for the session; storageFailed is emitted in both cases.
    void loadStoredSources(LoadMode mode) { _repository->loadStoredSources(mode); }

    /// @return Name of the stored copy of a source in storageDir, empty when it has none
    QString storedFileName(const QString &key) const { return _repository->storedFileName(key); }

    /// @return Zones to label in the map region: those centered in it, limited to maxLabelCount
    ///         preferring the most restrictive
    Q_INVOKABLE QVariantList labelZones(const QGeoRectangle &region) const;

    /// @return Zones shown on maps whose horizontal projection contains the coordinate at any altitude, the most
    ///         restrictive and active first
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
    QmlObjectListModel *sources() const { return _sources; }
    GeoZoneSource *source(const QString &key) const { return _repository->source(key); }
    GeoZoneSourceRepository *repository() const { return _repository; }
    GeoZoneDisplay *display() const { return _display; }
    GeoZonePlanning *planning() const { return _planning; }
    static GeoZoneSettings *settings() { return GeoZoneSettings::instance(); }
    /// Polygons and circles of all zones for map display
    const QList<GeoZoneShape*> &shapes() const { return _catalog.shapes(); }
    /// @return Shapes of the zones whose bounding box intersects the bounds in degrees
    QList<const GeoZoneShape*> shapesInArea(double south, double west, double north, double east) const { return _catalog.shapesInArea(south, west, north, east); }
    QGeoRectangle boundingBox() const { return _catalog.boundingBox(); }
    int disabledSourceCount() const;
    bool busy() const { return _repository->busy(); }
    double progress() const { return _repository->progress(); }
    QString progressText() const { return _repository->progressText(); }
    QString validityWarning() const { return _repository->validityWarning(); }
    QStringList fileDialogFilters() const;
    static QVariantList feedPresets() { return GeoZoneProviders::feedPresets(); }
    /// See GeoZoneProviders::areaProviders
    static QVariantList areaProviders() { return GeoZoneProviders::areaProviders(); }
    static QVariantList serviceTemplates() { return GeoZoneProviders::serviceTemplateList(); }
    static QString credentialStoreName() { return GeoZoneCredentials::storeName(); }
    static bool packSigningAvailable() { return GeoZonePackSignature::available(); }
    QString packPublicKey() const;
    QString packKeyId() const;
    QVariantList trustedPublishers() const;
    /// See GeoZoneProviders::areaFeedUrl
    Q_INVOKABLE QString areaFeedUrl(const QString &provider, const QGeoRectangle &area) const { return GeoZoneProviders::areaFeedUrl(provider, area); }
    /// Adds a feed downloading the zones of the provider in the area, like the area of a map, which is then kept for
    /// offline use and refreshed like other feeds. loadFailed is emitted when the area is too large.
    Q_INVOKABLE bool addArea(const QString &provider, const QGeoRectangle &area);

    QString storageDir() const { return _repository->storageDir(); }

    /// Sets the area of a mission, which with the active vehicle makes the area of interest of the services taking it,
    /// see GeoZoneSourceRepository::setMissionArea
    /// @param mission Identifies the mission, like the mission check following it
    void setMissionArea(const QObject *mission, const QGeoRectangle &area) { _repository->setMissionArea(reinterpret_cast<quintptr>(mission), area); }

signals:
    /// The zones and shapes of the source are about to be deleted, because it is reloaded or removed
    void zonesAboutToChange(GeoZoneSource *source);
    /// The zones of the source changed, zones() and the queries include them unless it was removed. The source is
    /// null when the vertical limits of all zones changed, after a new QNH converts their flight levels differently.
    void zonesChanged(GeoZoneSource *source);
    void boundingBoxChanged();
    void showZoneRequested(const QVariant &zone);   ///< GeoZoneInfo
    void zoneCountChanged();
    /// Zones became active or inactive with the time, see GeoZone::active
    void activeZonesChanged();
    void busyChanged(bool busy);
    void progressChanged();
    void validityWarningChanged();
    void sourceAdded(GeoZoneSource *source);    ///< A source added by addFile or addUrl was loaded, not emitted when feeds are refreshed
    void loadFailed(const QString &key, const QString &message);   ///< Loading or downloading a source requested by the user failed
    void storageFailed(const QString &message); ///< Stored copies can't be read or written, emitted once per session
    void outdatedDataFound(const QString &message); ///< Some sources have outdated data after the startup refresh
    void fileOperationFinished(bool success, const QString &message);  ///< An export or import the user asked for finished
    void packSigningChanged();
    void trustedPublishersChanged();

private slots:
    void _updateActive();

private:
    void _endZonesChange(GeoZoneSource *source);
    /// Applies the pack signing settings to the repository
    void _updatePackSigning();
    void _updateTrustedPublishers();
    /// @return The public key of the pack signing key, empty without
    QByteArray _packPublicKey() const;
    void _activeVehicleChanged(Vehicle *vehicle);

    QmlObjectListModel *_sources = nullptr;     ///< The sources of the repository for QML, deleted before them
    GeoZoneSourceRepository *_repository = nullptr;
    GeoZoneDisplay *_display = nullptr;
    GeoZonePlanning *_planning = nullptr;
    GeoZoneCatalog _catalog;
    QTimer *_activeTimer = nullptr;
    QMetaObject::Connection _vehicleConnection;

    static constexpr int _activeUpdateIntervalMSecs = 60 * 1000;
};
