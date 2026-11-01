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
#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtPositioning/QGeoRectangle>

#include "GeoZoneBox.h"
#include "GeoZoneParser.h"
#include "GeoZoneProviders.h"

/// Loading, downloading and storing sources
Q_DECLARE_LOGGING_CATEGORY(GeoZoneSourceLog)

class GeoZone;
class GeoZoneShape;

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
    Q_PROPERTY(QString      changesText     READ changesText    NOTIFY changed FINAL)               ///< Zones added, removed and changed by the last reload which changed them, empty without

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
    QString changesText() const { return _changesText; }
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
    void setDatasetUrl(const QString &datasetUrl) { _datasetUrl = datasetUrl; }

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

signals:
    void changed();
    void validityChanged();
    void busyChanged(bool busy);
    void errorStringChanged();
    void enabledChanged();

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
    QString _changesText;
    QSet<QString> _changedZoneKeys;
    bool _enabled = true;
    QString _errorString;
    QByteArray _contentHash;
    QString _etag;
    QString _lastModified;
    QString _datasetUrl;

    QList<GeoZone*> _zones;
    QList<QSharedPointer<GeoZone>> _sharedZones;    ///< Owns _zones
    QList<GeoZoneShape*> _shapes;
    GeoZoneBox _box;
};
