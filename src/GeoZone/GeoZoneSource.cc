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

#include <QtCore/QHash>
#include <QtCore/QUrl>

namespace {

QString _formatDate(const QDateTime &dateTime)
{
    return dateTime.toUTC().toString(QStringLiteral("yyyy-MM-dd"));
}

size_t _footprintHash(const GeoZone::Volume &volume)
{
    if (volume.isCircle) {
        return qHashMulti(0, volume.center.latitude(), volume.center.longitude(), volume.radius);
    }
    size_t hash = 0;
    for (const QList<QGeoCoordinate> &ring : volume.rings) {
        for (const QGeoCoordinate &coordinate : ring) {
            hash = qHashMulti(hash, coordinate.latitude(), coordinate.longitude());
        }
    }
    return hash;
}

bool _sameFootprint(const GeoZone::Volume &a, const GeoZone::Volume &b)
{
    if (a.isCircle != b.isCircle) {
        return false;
    }
    return a.isCircle ? ((a.center == b.center) && (a.radius == b.radius)) : (a.rings == b.rings);
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

QString GeoZoneSource::_sourceText() const
{
    return _dateText().isEmpty() ? name() : QStringLiteral("%1 (%2)").arg(name(), _dateText());
}

void GeoZoneSource::_setMetadata(const GeoZoneParser::Result &result, const QDateTime &loadedAt, const QDateTime &fileDate)
{
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
}

void GeoZoneSource::markCurrent(const GeoZoneParser::Result &result, const QDateTime &loadedAt, const QDateTime &fileDate)
{
    _setMetadata(result, loadedAt, fileDate);
    const QString sourceText = _sourceText();
    for (GeoZone *zone : std::as_const(_zones)) {
        zone->setSourceText(sourceText);
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
    _setMetadata(result, loadedAt, fileDate);

    const QString sourceText = _sourceText();
    for (const GeoZone::Data &zoneData : result.zones) {
        GeoZone *const zone = new GeoZone(zoneData, this);
        zone->setSourceKey(_key);
        zone->setSourceText(sourceText);
        _zones.append(zone);
        if (zone->boundingBox().isValid()) {
            _boundingBox = _boundingBox.isValid() ? _boundingBox.united(zone->boundingBox()) : zone->boundingBox();
        }

        // Volumes stacked at different altitudes often share the same footprint, only draw it once
        QMultiHash<size_t, qsizetype> drawnVolumes;
        for (qsizetype i = 0; i < zoneData.volumes.count(); i++) {
            const GeoZone::Volume &volume = zoneData.volumes[i];
            const size_t hash = _footprintHash(volume);
            bool drawn = false;
            for (auto it = drawnVolumes.constFind(hash); !drawn && (it != drawnVolumes.cend()) && (it.key() == hash); ++it) {
                drawn = _sameFootprint(zoneData.volumes[it.value()], volume);
            }
            if (drawn) {
                continue;
            }
            drawnVolumes.insert(hash, i);
            _shapes.append(volume.isCircle ? new GeoZoneShape(zone, volume.center, volume.radius) : new GeoZoneShape(zone, volume.rings));
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
