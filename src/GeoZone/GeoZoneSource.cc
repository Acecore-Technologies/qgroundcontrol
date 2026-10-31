/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneSource.h"
#include "GeoZone.h"
#include "GeoZoneSettings.h"
#include "GeoZoneShape.h"
#include "SettingsManager.h"

#include <QtCore/QUrl>
#include <QtPositioning/QGeoCircle>

namespace {

QString _formatDate(const QDateTime &dateTime)
{
    return dateTime.toUTC().toString(QStringLiteral("yyyy-MM-dd"));
}

} // namespace

GeoZoneSource::GeoZoneSource(const QString &key, const QString &url, QObject *parent)
    : QObject(parent)
    , _key(key)
    , _url(url)
{
}

GeoZoneSource::~GeoZoneSource()
{
    _clearZones();
}

QString GeoZoneSource::name() const
{
    if (!_title.isEmpty()) {
        return _title;
    }
    return isFeed() ? QUrl(_url).host() : _key;
}

QString GeoZoneSource::formatName() const
{
    return (_format == GeoZoneParser::Format::Unknown) ? QString() : GeoZoneParser::formatToString(_format);
}

QString GeoZoneSource::detailsText() const
{
    if (!loaded()) {
        return QString();
    }

    QStringList parts = { formatName(), tr("zones: %1").arg(zoneCount()) };
    if (!_dateText().isEmpty()) {
        parts.append(_dateText());
    }
    if (!_provider.isEmpty()) {
        parts.append(_provider);
    }
    if (_skippedCount > 0) {
        parts.append(tr("unsupported features skipped: %1").arg(_skippedCount));
    }
    return parts.join(QStringLiteral(", "));
}

QString GeoZoneSource::_dateText() const
{
    if (_issued.isValid()) {
        return tr("issued %1").arg(_formatDate(_issued));
    }
    if (isFeed() && _loadedAt.isValid()) {
        return tr("downloaded %1").arg(_formatDate(_loadedAt));
    }
    if (_fileDate.isValid()) {
        return tr("file from %1").arg(_formatDate(_fileDate));
    }
    return QString();
}

void GeoZoneSource::_clearZones()
{
    qDeleteAll(_shapes);
    _shapes.clear();
    // QML may still hold references until the next event loop iteration
    for (GeoZone *zone : std::as_const(_zones)) {
        zone->deleteLater();
    }
    _zones.clear();
    _boundingBox = QGeoRectangle();
}

void GeoZoneSource::markCurrent(const QDateTime &loadedAt, const QDateTime &issued)
{
    _loadedAt = loadedAt;
    _fileDate = loadedAt;
    if (issued.isValid()) {
        _issued = issued;
    }
    updateValidity(QDateTime::currentDateTimeUtc());
    emit changed();
}

void GeoZoneSource::setStoredDates(const QDateTime &loadedAt, const QDateTime &fileDate)
{
    _loadedAt = loadedAt;
    _fileDate = fileDate;
    updateValidity(QDateTime::currentDateTimeUtc());
    emit changed();
}

void GeoZoneSource::setResult(const GeoZoneParser::Result &result, const QDateTime &loadedAt, const QDateTime &fileDate)
{
    _clearZones();

    _title = result.title;
    _format = result.format;
    _provider = result.provider;
    _skippedCount = result.skippedFeatures;
    _issued = result.issued;
    _validFrom = result.validFrom;
    _validTo = result.validTo;
    _loadedAt = loadedAt;
    _fileDate = fileDate;
    _contentHash = result.contentHash;

    const QString sourceText = _dateText().isEmpty() ? name() : QStringLiteral("%1 (%2)").arg(name(), _dateText());
    for (const GeoZone::Data &zoneData : result.zones) {
        GeoZone *const zone = new GeoZone(zoneData, this);
        zone->setSourceText(sourceText);
        _zones.append(zone);
        if (zone->boundingBox().isValid()) {
            _boundingBox = _boundingBox.isValid() ? _boundingBox.united(zone->boundingBox()) : zone->boundingBox();
        }

        // Volumes stacked at different altitudes often share the same footprint, only draw it once
        QList<QList<QList<QGeoCoordinate>>> drawnPolygons;
        QList<QGeoCircle> drawnCircles;
        for (const GeoZone::Volume &volume : zoneData.volumes) {
            if (volume.isCircle) {
                const QGeoCircle circle(volume.center, volume.radius);
                if (!drawnCircles.contains(circle)) {
                    drawnCircles.append(circle);
                    _shapes.append(new GeoZoneShape(zone, volume.center, volume.radius));
                }
            } else if (!drawnPolygons.contains(volume.rings)) {
                drawnPolygons.append(volume.rings);
                _shapes.append(new GeoZoneShape(zone, volume.rings));
            }
        }
    }

    updateValidity(QDateTime::currentDateTimeUtc());
    emit changed();
}

void GeoZoneSource::setBusy(bool busy)
{
    if (busy != _busy) {
        _busy = busy;
        emit busyChanged(_busy);
    }
}

void GeoZoneSource::setErrorString(const QString &errorString)
{
    if (errorString != _errorString) {
        _errorString = errorString;
        emit errorStringChanged();
    }
}

void GeoZoneSource::updateValidity(const QDateTime &now)
{
    GeoZoneSettings *const settings = SettingsManager::instance()->geoZoneSettings();
    QString text;
    if (loaded()) {
        const QDateTime date = dataDate();
        if (_validTo.isValid() && (_validTo < now)) {
            text = tr("Expired on %1").arg(_formatDate(_validTo));
        } else if (_validFrom.isValid() && (_validFrom > now)) {
            text = tr("Not valid until %1").arg(_formatDate(_validFrom));
        } else if (isLive() && date.isValid() && (date.secsTo(now) > (settings->liveStaleHours()->rawValue().toLongLong() * 3600))) {
            text = tr("Data is %1 hours old").arg(date.secsTo(now) / 3600);
        } else if (!isLive() && date.isValid() && (date.daysTo(now) > settings->staleDays()->rawValue().toLongLong())) {
            text = tr("Data is %1 days old").arg(date.daysTo(now));
        }
    }

    if (text != _validityText) {
        _validityText = text;
        emit validityChanged();
    }
}

int GeoZoneSource::refreshIntervalSecs() const
{
    GeoZoneSettings *const settings = SettingsManager::instance()->geoZoneSettings();
    return isLive() ? (settings->liveFeedRefreshMinutes()->rawValue().toInt() * 60) : (settings->feedRefreshHours()->rawValue().toInt() * 3600);
}

bool GeoZoneSource::refreshDue(const QDateTime &now) const
{
    return isFeed() && (!_loadedAt.isValid() || (_loadedAt.secsTo(now) >= refreshIntervalSecs()));
}
