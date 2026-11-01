/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtPositioning/QGeoRectangle>

#include "GeoZoneBox.h"
#include "GeoZoneParser.h"
#include "GeoZoneProvenance.h"
#include "GeoZoneProviders.h"
#include "GeoZoneService.h"

/// Loading, downloading and storing sources
Q_DECLARE_LOGGING_CATEGORY(GeoZoneSourceLog)

class GeoZone;
class GeoZoneShape;

/// Zones added, removed and changed by a reload of a source: zones are told apart by their identifier, or their name
/// without, and compared by their content hashes (GeoZone::Data::contentHash)
struct GeoZoneSourceChanges
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneSourceChanges)

public:
    QDateTime at;           ///< When the reload happened
    int addedCount = 0;
    int removedCount = 0;
    int changedCount = 0;
    QStringList added;      ///< Names of the zones, sorted, at most maxNames
    QStringList removed;
    QStringList changed;

    static constexpr qsizetype maxNames = 200;

    bool isEmpty() const { return (addedCount + removedCount + changedCount) == 0; }
    /// @return Like "Updated 12:03: 3 new, 1 changed, 2 removed", empty without changes
    QString summary() const;
    /// @return The names of the zones added, changed and removed, a line for each, empty without changes
    QString details() const;
    QJsonObject toJson() const;
    static GeoZoneSourceChanges fromJson(const QJsonObject &object);
    bool operator==(const GeoZoneSourceChanges &other) const = default;
};

/// Zones loaded from one geozone file or downloaded feed
class GeoZoneSource : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString      key             READ key            CONSTANT FINAL)                   ///< File name, or the URL of a feed
    Q_PROPERTY(QString      url             READ url            CONSTANT FINAL)                   ///< Empty for files
    Q_PROPERTY(QString      name            READ name           NOTIFY changed FINAL)
    Q_PROPERTY(QString      formatName      READ formatName     NOTIFY changed FINAL)
    Q_PROPERTY(QString      provider        READ provider       NOTIFY changed FINAL)
    Q_PROPERTY(int          zoneCount       READ zoneCount      NOTIFY changed FINAL)
    Q_PROPERTY(int          skippedCount    READ skippedCount   NOTIFY changed FINAL)
    Q_PROPERTY(QDateTime    loadedAt        READ loadedAt       NOTIFY changed FINAL)             ///< When the file was loaded or the feed downloaded
    Q_PROPERTY(QDateTime    dataDate        READ dataDate       NOTIFY changed FINAL)             ///< When the data was issued, or the file date
    Q_PROPERTY(QDateTime    checkedAt       READ checkedAt      NOTIFY changed FINAL)             ///< When the server last confirmed a feed's data current, or it was loaded
    Q_PROPERTY(QString      detailsText     READ detailsText    NOTIFY changed FINAL)
    Q_PROPERTY(QString      validityText    READ validityText   NOTIFY validityChanged FINAL)
    Q_PROPERTY(QString      warningText     READ warningText    NOTIFY changed FINAL)               ///< Why the data is incomplete or partly unreadable, empty when it is complete
    Q_PROPERTY(bool         busy            READ busy           NOTIFY busyChanged FINAL)
    Q_PROPERTY(QString      errorString     READ errorString    NOTIFY errorStringChanged FINAL)  ///< Why the last refresh failed
    Q_PROPERTY(QGeoRectangle boundingBox    READ boundingBox    NOTIFY changed FINAL)
    Q_PROPERTY(bool         enabled         READ enabled        WRITE setEnabled NOTIFY enabledChanged FINAL)   ///< Zones of disabled sources are left out of the maps, checks and alerts
    Q_PROPERTY(QString      changesText     READ changesText    NOTIFY changed FINAL)               ///< Counts of the zones added, changed and removed by the last reload which changed them, empty without
    Q_PROPERTY(QString      changesDetails  READ changesDetails NOTIFY changed FINAL)               ///< Names of these zones, see GeoZoneSourceChanges::details
    Q_PROPERTY(bool         needsCredentials    READ needsCredentials   NOTIFY serviceChanged FINAL)    ///< The service of the feed requires a secret, see GeoZoneCredentials
    Q_PROPERTY(bool         credentialsMissing  READ credentialsMissing NOTIFY serviceChanged FINAL)    ///< The secret the service requires isn't known, like after importing a pack
    Q_PROPERTY(bool         followsArea     READ followsArea    NOTIFY serviceChanged FINAL)        ///< The service downloads the zones around the mission and the vehicle
    Q_PROPERTY(QGeoRectangle area           READ area           NOTIFY changed FINAL)               ///< Area the zones were downloaded for, invalid unless followsArea
    Q_PROPERTY(QString      areaText        READ areaText       NOTIFY changed FINAL)               ///< Describes area, empty without
    Q_PROPERTY(QString      provenanceText  READ provenanceText NOTIFY changed FINAL)               ///< See GeoZoneProvenance::summary
    Q_PROPERTY(bool         verified        READ verified       NOTIFY changed FINAL)               ///< The data matches the SHA-256 its publisher gave
    Q_PROPERTY(bool         integrityFailed READ integrityFailed NOTIFY changed FINAL)              ///< The last download didn't match the SHA-256 its publisher gave, and wasn't loaded
    Q_PROPERTY(bool         imported        READ imported       NOTIFY changed FINAL)               ///< The data was imported from an offline pack

public:
    GeoZoneSource(const QString &key, const QString &url, QObject *parent = nullptr);
    ~GeoZoneSource();

    QString key() const { return _key; }
    /// @return true once zones were loaded
    bool loaded() const { return _format != GeoZoneParser::Format::Unknown; }
    QString url() const { return _url; }
    bool isFeed() const { return !_url.isEmpty(); }
    QString name() const;
    QString formatName() const;
    QString provider() const { return _provider; }
    int zoneCount() const { return static_cast<int>(_zones.count()); }
    int skippedCount() const { return _skippedCount; }
    QDateTime loadedAt() const { return _loadedAt; }
    QDateTime fileDate() const { return _fileDate; }
    QDateTime dataDate() const { return _issued.isValid() ? _issued : _fileDate; }
    /// @return When the data was last confirmed current: when the server answered a refresh with the same data or
    ///         304 Not Modified, otherwise when it was loaded
    QDateTime checkedAt() const { return _checkedAt; }
    QDateTime validFrom() const { return _validFrom; }
    QDateTime validTo() const { return _validTo; }
    QString detailsText() const;
    QString validityText() const { return _validityText; }
    QString warningText() const;
    bool busy() const { return _busy; }
    QString errorString() const { return _errorString; }
    bool enabled() const { return _enabled; }
    QString changesText() const { return _changes.summary(); }
    QString changesDetails() const { return _changes.details(); }
    /// @return The changes of the last reload which changed zones, kept until the next one
    const GeoZoneSourceChanges &changes() const { return _changes; }
    /// Restores the changes recorded by a previous session
    void setChanges(const GeoZoneSourceChanges &changes);
    /// @return GeoZone::key of the zones whose published data changed in the last reload
    const QSet<QString> &changedZoneKeys() const { return _changedZoneKeys; }
    void setEnabled(bool enabled);

    const QList<GeoZone*> &zones() const { return _zones; }
    /// The zones, to keep them alive while another thread uses them
    const QList<QSharedPointer<GeoZone>> &sharedZones() const { return _sharedZones; }
    /// Volumes sharing a footprint have one shape
    const QList<GeoZoneShape*> &shapes() const { return _shapes; }
    /// Bounds of the zones, which may cross the antimeridian
    const GeoZoneBox &box() const { return _box; }
    QGeoRectangle boundingBox() const { return _box.toRectangle(); }

    /// Sets the dates of the stored copy before it is loaded
    /// @param checkedAt When the server last confirmed the copy current, loadedAt when invalid or earlier
    void setStoredDates(const QDateTime &loadedAt, const QDateTime &fileDate, const QDateTime &checkedAt = QDateTime());
    /// Records that the server confirmed the data current, keeping its dates
    void setCheckedAt(const QDateTime &checkedAt);

    GeoZoneParser::Format format() const { return _format; }
    QByteArray contentHash() const { return _contentHash; }

    /// Records that a download had the same zones as loaded, updating the dataset metadata
    void markCurrent(const GeoZoneParser::Result &result, const QDateTime &loadedAt, const QDateTime &fileDate);

    /// HTTP validators of a feed for downloading it only when it changed
    QString etag() const { return _etag; }
    QString lastModified() const { return _lastModified; }
    void setValidators(const QString &etag, const QString &lastModified) { _etag = etag; _lastModified = lastModified; }
    /// Dataset a feed's page linked to, like the current AIRAC cycle, empty when the feed URL is the dataset. The
    /// validators are those of the dataset.
    QString datasetUrl() const { return _datasetUrl; }
    void setDatasetUrl(const QString &datasetUrl);
    /// @return Effective date of the AIRAC cycle of the data, invalid unless the feed's dataset is published by AIRAC
    ///         cycle, see GeoZoneProviders::airacDatasetDate. The data is outdated once the next cycle takes effect.
    QDate airacDate() const { return GeoZoneProviders::airacDatasetDate(_datasetUrl); }

    /// Replaces the zones. The previous zones are deleted.
    /// @param fileDate Date of the data when the file has no issue date: file modification or download time
    void setResult(const GeoZoneParser::Result &result, const QDateTime &loadedAt, const QDateTime &fileDate);
    void setBusy(bool busy);
    void setErrorString(const QString &errorString);

    /// Re-evaluates expiry and staleness against the specified UTC time, using the outdated data ages of
    /// GeoZoneEnvironment::sourceSettings. Data without an issue date downloaded from a server is as old as the
    /// server last confirmed it.
    void updateValidity(const QDateTime &now);

    /// @return Temporary flight restrictions which change during the day, downloaded and outdated sooner
    bool isLive() const { return GeoZoneProviders::isLive(_format); }

    /// How the feed is requested from its service, empty for plain feeds and files
    const GeoZoneService &service() const { return _service; }
    void setService(const GeoZoneService &service);
    bool needsCredentials() const { return _service.needsSecret(); }
    bool credentialsMissing() const { return _service.needsSecret() && _credentialsMissing; }
    void setCredentialsMissing(bool missing);
    bool followsArea() const { return _service.areaOfInterest; }
    /// Area of interest the feed was downloaded for, invalid when its service takes none
    QGeoRectangle area() const { return _area; }
    void setArea(const QGeoRectangle &area);
    QString areaText() const;

    /// Where and how the data was obtained
    const GeoZoneProvenance &provenance() const { return _provenance; }
    void setProvenance(const GeoZoneProvenance &provenance);
    QString provenanceText() const { return _provenance.summary(); }
    bool verified() const { return _provenance.integrity == GeoZoneProvenance::Integrity::Verified; }
    bool imported() const { return _provenance.imported; }
    bool integrityFailed() const { return _integrityFailed; }
    void setIntegrityFailed(bool failed);

signals:
    void changed();
    void validityChanged();
    void busyChanged(bool busy);
    void errorStringChanged();
    void enabledChanged();
    void serviceChanged();

private:
    void _clearZones();
    /// Compares the zones with those of the previous load
    void _summarizeChanges(const QList<GeoZone::Data> &previous, const QList<GeoZone*> &zones);
    void _setMetadata(const GeoZoneParser::Result &result, const QDateTime &loadedAt, const QDateTime &fileDate);
    QString _dateText() const;
    /// @return true for feeds downloaded from a server, which confirms their data
    bool _isDownloaded() const;
    QString _sourceText() const;

    const QString _key;
    const QString _url;
    QString _title;
    GeoZoneParser::Format _format = GeoZoneParser::Format::Unknown;
    QString _provider;
    int _skippedCount = 0;
    int _missingCount = 0;
    QMap<QString, int> _issues;     ///< See GeoZoneParser::Result::issues
    QDateTime _loadedAt;
    QDateTime _fileDate;
    QDateTime _checkedAt;
    QDateTime _issued;
    QDateTime _validFrom;
    QDateTime _validTo;
    QString _validityText;
    bool _busy = false;
    GeoZoneSourceChanges _changes;
    QSet<QString> _changedZoneKeys;
    bool _enabled = true;
    QString _errorString;
    QByteArray _contentHash;
    QString _etag;
    QString _lastModified;
    QString _datasetUrl;
    GeoZoneService _service;
    bool _credentialsMissing = false;
    QGeoRectangle _area;
    GeoZoneProvenance _provenance;
    bool _integrityFailed = false;

    QList<GeoZone*> _zones;
    QList<QSharedPointer<GeoZone>> _sharedZones;    ///< Owns _zones
    QList<GeoZoneShape*> _shapes;
    GeoZoneBox _box;
};
