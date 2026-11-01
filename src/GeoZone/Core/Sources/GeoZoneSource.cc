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
#include "GeoZoneEnvironment.h"
#include "GeoZoneShape.h"

#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QLocale>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(GeoZoneSourceLog, "qgc.geozone.geozonesource")

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
    for (const GeoZone::Ring &ring : volume.rings) {
        for (const QPointF &vertex : ring) {
            hash = qHashMulti(hash, vertex.y(), vertex.x());
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

void GeoZoneSource::setEnabled(bool enabled)
{
    if (enabled != _enabled) {
        _enabled = enabled;
        emit enabledChanged();
    }
}

GeoZoneSource::~GeoZoneSource()
{
    _clearZones();
}

QString GeoZoneSource::name() const
{
    QString name = _title.isEmpty() ? (isFeed() ? QUrl(_url).host() : QFileInfo(_key).fileName()) : _title;

    // Feeds of an area, like the map area downloaded from a WFS, are told apart by where they are
    const QUrlQuery query(QUrl(_url).query());
    for (const auto &[key, value] : query.queryItems()) {
        if (key.compare(QStringLiteral("BBOX"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QStringList bounds = value.split(QLatin1Char(','));
        if (bounds.count() >= 4) {
            const double longitude = (bounds[0].toDouble() + bounds[2].toDouble()) / 2;
            const double latitude = (bounds[1].toDouble() + bounds[3].toDouble()) / 2;
            name = tr("%1, area around %2, %3").arg(name).arg(latitude, 0, 'f', 2).arg(longitude, 0, 'f', 2);
        }
    }
    return name;
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

QString GeoZoneSource::warningText() const
{
    QStringList lines;
    if (_missingCount > 0) {
        lines.append(tr("The server left out %1 zones beyond its limit, which could not be downloaded. Download a smaller area.").arg(_missingCount));
    }
    for (auto it = _issues.cbegin(); it != _issues.cend(); ++it) {
        lines.append(tr("%1 (%n zone(s))", nullptr, it.value()).arg(it.key()));
    }
    return lines.join(QStringLiteral("\n"));
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
    // Zones are deleted when GeoZoneInfo values in QML no longer share them
    _zones.clear();
    _sharedZones.clear();
    _box = GeoZoneBox();
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
    _missingCount = result.missingFeatures;
    _issues = result.issues;
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
    updateValidity(GeoZoneEnvironment::currentDateTimeUtc());
    emit changed();
}

void GeoZoneSource::setStoredDates(const QDateTime &loadedAt, const QDateTime &fileDate)
{
    _loadedAt = loadedAt;
    _fileDate = fileDate;
    updateValidity(GeoZoneEnvironment::currentDateTimeUtc());
    emit changed();
}

void GeoZoneSource::setResult(const GeoZoneParser::Result &result, const QDateTime &loadedAt, const QDateTime &fileDate)
{
    const bool reload = loaded();
    QList<GeoZone::Data> previous;
    previous.reserve(_zones.count());
    for (const GeoZone *zone : std::as_const(_zones)) {
        GeoZone::Data summary;
        summary.identifier = zone->identifier();
        summary.name = zone->name();
        summary.contentHash = zone->data().contentHash;
        previous.append(summary);
    }
    _clearZones();
    _setMetadata(result, loadedAt, fileDate);

    const QString sourceText = _sourceText();
    for (const GeoZone::Data &zoneData : result.zones) {
        const QSharedPointer<GeoZone> sharedZone = QSharedPointer<GeoZone>::create(zoneData);
        GeoZone *const zone = sharedZone.data();
        _sharedZones.append(sharedZone);
        zone->setSourceKey(_key);
        zone->setSourceText(sourceText);
        _zones.append(zone);
        _box = _box.united(zone->box());

        // Volumes stacked at different altitudes often share the same footprint, only draw it once. The zone's rings
        // are drawn, which are continuous across the antimeridian.
        const QList<GeoZone::Volume> &volumes = zone->data().volumes;
        QMultiHash<size_t, qsizetype> drawnVolumes;
        for (qsizetype i = 0; i < volumes.count(); i++) {
            const GeoZone::Volume &volume = volumes[i];
            const size_t hash = _footprintHash(volume);
            bool drawn = false;
            for (auto it = drawnVolumes.constFind(hash); !drawn && (it != drawnVolumes.cend()) && (it.key() == hash); ++it) {
                drawn = _sameFootprint(volumes[it.value()], volume);
            }
            if (drawn) {
                continue;
            }
            drawnVolumes.insert(hash, i);
            _shapes.append(volume.isCircle ? new GeoZoneShape(zone, volume.center, volume.radius) : new GeoZoneShape(zone, volume.rings));
        }
    }

    _changedZoneKeys.clear();
    if (reload) {
        _summarizeChanges(previous, _zones);
    }
    updateValidity(GeoZoneEnvironment::currentDateTimeUtc());
    emit changed();
}

void GeoZoneSource::_summarizeChanges(const QList<GeoZone::Data> &previous, const QList<GeoZone*> &zones)
{
    // Zones are recognized by identifier, or name without, several shapes of a TFR share one
    const auto identity = [](const QString &identifier, const QString &name) { return identifier.isEmpty() ? name : identifier; };
    QHash<QString, QList<QByteArray>> before;
    QHash<QString, QString> names;
    for (const GeoZone::Data &zone : previous) {
        const QString id = identity(zone.identifier, zone.name);
        before[id].append(zone.contentHash);
        names.insert(id, zone.name.isEmpty() ? zone.identifier : zone.name);
    }
    QHash<QString, QList<QByteArray>> after;
    QHash<QString, QStringList> keys;
    for (const GeoZone *zone : zones) {
        const QString id = identity(zone->identifier(), zone->name());
        after[id].append(zone->data().contentHash);
        keys[id].append(zone->key());
        names.insert(id, zone->displayName());
    }

    QStringList added;
    QStringList removed;
    QStringList changedNames;
    for (auto it = after.begin(); it != after.end(); ++it) {
        const auto old = before.constFind(it.key());
        if (old == before.cend()) {
            added.append(names.value(it.key()));
            continue;
        }
        QList<QByteArray> oldHashes = old.value();
        QList<QByteArray> newHashes = it.value();
        std::sort(oldHashes.begin(), oldHashes.end());
        std::sort(newHashes.begin(), newHashes.end());
        if (oldHashes != newHashes) {
            changedNames.append(names.value(it.key()));
            for (const QString &zoneKey : keys.value(it.key())) {
                _changedZoneKeys.insert(zoneKey);
            }
        }
    }
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        if (!after.contains(it.key())) {
            removed.append(names.value(it.key()));
        }
    }
    if (added.isEmpty() && removed.isEmpty() && changedNames.isEmpty()) {
        return;
    }

    const auto list = [](QStringList namesList) {
        constexpr qsizetype maxNames = 3;
        namesList.sort(Qt::CaseInsensitive);
        QString text = namesList.mid(0, maxNames).join(QStringLiteral(", "));
        if (namesList.count() > maxNames) {
            text += tr(" and %1 more").arg(namesList.count() - maxNames);
        }
        return text;
    };
    QStringList parts;
    if (!added.isEmpty()) {
        parts.append(tr("added %1").arg(list(added)));
    }
    if (!removed.isEmpty()) {
        parts.append(tr("removed %1").arg(list(removed)));
    }
    if (!changedNames.isEmpty()) {
        parts.append(tr("changed %1").arg(list(changedNames)));
    }
    _changesText = tr("Update of %1: %2").arg(QLocale().toString(GeoZoneEnvironment::currentDateTimeUtc().toLocalTime(), QLocale::ShortFormat), parts.join(QStringLiteral("; ")));
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
    const GeoZoneEnvironment::SourceSettings settings = GeoZoneEnvironment::sourceSettings();
    QString text;
    if (loaded()) {
        const QDateTime date = dataDate();
        if (_validTo.isValid() && (_validTo < now)) {
            text = tr("Expired on %1").arg(_formatDate(_validTo));
        } else if (_validFrom.isValid() && (_validFrom > now)) {
            text = tr("Not valid until %1").arg(_formatDate(_validFrom));
        } else if (isLive() && date.isValid() && (date.secsTo(now) > (qint64{ settings.liveStaleHours } * 3600))) {
            text = tr("Data is %1 hours old").arg(date.secsTo(now) / 3600);
        } else if (!isLive() && date.isValid() && (date.daysTo(now) > settings.staleDays)) {
            text = tr("Data is %1 days old").arg(date.daysTo(now));
        }
    }

    if (text != _validityText) {
        _validityText = text;
        emit validityChanged();
    }
}
