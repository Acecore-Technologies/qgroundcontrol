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
#include <QtCore/QObject>
#include <QtPositioning/QGeoRectangle>

#include "GeoZoneParser.h"

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
    Q_PROPERTY(QString      detailsText     READ detailsText    NOTIFY changed FINAL)
    Q_PROPERTY(QString      validityText    READ validityText   NOTIFY validityChanged FINAL)
    Q_PROPERTY(QString      warningText     READ warningText    NOTIFY changed FINAL)               ///< Why the data is incomplete, empty when it is complete     ///< Warning about outdated data, empty when current
    Q_PROPERTY(bool         busy            READ busy           NOTIFY busyChanged FINAL)
    Q_PROPERTY(QString      errorString     READ errorString    NOTIFY errorStringChanged FINAL)  ///< Why the last refresh failed
    Q_PROPERTY(QGeoRectangle boundingBox    READ boundingBox    NOTIFY changed FINAL)

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
    QDateTime validFrom() const { return _validFrom; }
    QDateTime validTo() const { return _validTo; }
    QString detailsText() const;
    QString validityText() const { return _validityText; }
    QString warningText() const;
    bool busy() const { return _busy; }
    QString errorString() const { return _errorString; }

    const QList<GeoZone*> &zones() const { return _zones; }
    /// Volumes sharing a footprint have one shape
    const QList<GeoZoneShape*> &shapes() const { return _shapes; }
    QGeoRectangle boundingBox() const { return _boundingBox; }

    /// Sets the dates of the stored copy before it is loaded, or when a feed is confirmed unchanged
    void setStoredDates(const QDateTime &loadedAt, const QDateTime &fileDate);

    GeoZoneParser::Format format() const { return _format; }
    QByteArray contentHash() const { return _contentHash; }

    /// Records that a download had the same zones as loaded, updating the dataset metadata
    void markCurrent(const GeoZoneParser::Result &result, const QDateTime &loadedAt, const QDateTime &fileDate);

    /// HTTP validators of a feed for downloading it only when it changed
    QString etag() const { return _etag; }
    QString lastModified() const { return _lastModified; }
    void setValidators(const QString &etag, const QString &lastModified) { _etag = etag; _lastModified = lastModified; }

    /// Replaces the zones. The previous zones are deleted.
    /// @param fileDate Date of the data when the file has no issue date: file modification or download time
    void setResult(const GeoZoneParser::Result &result, const QDateTime &loadedAt, const QDateTime &fileDate);
    void setBusy(bool busy);
    void setErrorString(const QString &errorString);

    /// Re-evaluates expiry and staleness against the specified UTC time, using the outdated data ages of the settings
    void updateValidity(const QDateTime &now);

    /// @return Temporary flight restrictions which change during the day, downloaded and outdated sooner
    bool isLive() const { return _format == GeoZoneParser::Format::FaaTfr; }

    /// @return Seconds after which a feed is downloaded again, from the refresh interval settings
    int refreshIntervalSecs() const;

    /// @return true if a feed should be downloaded again
    bool refreshDue(const QDateTime &now) const;

signals:
    void changed();
    void validityChanged();
    void busyChanged(bool busy);
    void errorStringChanged();

private:
    void _clearZones();
    void _setMetadata(const GeoZoneParser::Result &result, const QDateTime &loadedAt, const QDateTime &fileDate);
    QString _dateText() const;
    QString _sourceText() const;

    const QString _key;
    const QString _url;
    QString _title;
    GeoZoneParser::Format _format = GeoZoneParser::Format::Unknown;
    QString _provider;
    int _skippedCount = 0;
    int _missingCount = 0;
    QDateTime _loadedAt;
    QDateTime _fileDate;
    QDateTime _issued;
    QDateTime _validFrom;
    QDateTime _validTo;
    QString _validityText;
    bool _busy = false;
    QString _errorString;
    QByteArray _contentHash;
    QString _etag;
    QString _lastModified;

    QList<GeoZone*> _zones;
    QList<GeoZoneShape*> _shapes;
    QGeoRectangle _boundingBox;
};
