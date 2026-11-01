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
#include "GeoZoneAirac.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneShape.h"

#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
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

void GeoZoneSource::setService(const GeoZoneService &service)
{
    if (!(service == _service)) {
        _service = service;
        emit serviceChanged();
        emit changed();
    }
}

void GeoZoneSource::setCredentialsMissing(bool missing)
{
    if (missing != _credentialsMissing) {
        _credentialsMissing = missing;
        emit serviceChanged();
    }
}

void GeoZoneSource::setDatasetUrl(const QString &datasetUrl)
{
    if (datasetUrl != _datasetUrl) {
        _datasetUrl = datasetUrl;
        updateValidity(GeoZoneEnvironment::currentDateTimeUtc());
        emit changed();
    }
}

void GeoZoneSource::setArea(const QGeoRectangle &area)
{
    if (area != _area) {
        _area = area;
        emit changed();
    }
}

void GeoZoneSource::setProvenance(const GeoZoneProvenance &provenance)
{
    _provenance = provenance;
    emit changed();
}

void GeoZoneSource::setIntegrityFailed(bool failed)
{
    if (failed != _integrityFailed) {
        _integrityFailed = failed;
        emit changed();
    }
}

QString GeoZoneSource::areaText() const
{
    if (!_area.isValid()) {
        return QString();
    }
    return tr("Zones around the mission and the vehicle, downloaded for latitudes %1 to %2 and longitudes %3 to %4")
        .arg(_area.bottomRight().latitude(), 0, 'f', 3).arg(_area.topLeft().latitude(), 0, 'f', 3)
        .arg(_area.topLeft().longitude(), 0, 'f', 3).arg(_area.bottomRight().longitude(), 0, 'f', 3);
}

QString GeoZoneSource::name() const
{
    if (!_service.name.isEmpty()) {
        return _service.name;
    }
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
    if (const QDate airac = airacDate(); airac.isValid()) {
        parts.append(tr("AIRAC %1").arg(GeoZoneAirac::cycleName(airac)));
    }
    if (!_provider.isEmpty()) {
        parts.append(_provider);
    }
    if (_isDownloaded() && _checkedAt.isValid()) {
        parts.append(tr("checked %1").arg(QLocale().toString(_checkedAt.toLocalTime(), QLocale::ShortFormat)));
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
        lines.append(tr("The server left out %n zone(s) beyond its limit, which could not be downloaded. Download a smaller area.", nullptr, _missingCount));
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

bool GeoZoneSource::_isDownloaded() const
{
    return isFeed() && !QUrl(_url).isLocalFile();
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
    _checkedAt = loadedAt;
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

void GeoZoneSource::setStoredDates(const QDateTime &loadedAt, const QDateTime &fileDate, const QDateTime &checkedAt)
{
    _loadedAt = loadedAt;
    _fileDate = fileDate;
    _checkedAt = (checkedAt.isValid() && (checkedAt > loadedAt)) ? checkedAt : loadedAt;
    updateValidity(GeoZoneEnvironment::currentDateTimeUtc());
    emit changed();
}

void GeoZoneSource::setCheckedAt(const QDateTime &checkedAt)
{
    _checkedAt = checkedAt;
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

    const auto sorted = [](QStringList namesList) {
        namesList.sort(Qt::CaseInsensitive);
        return namesList.mid(0, GeoZoneSourceChanges::maxNames);
    };
    _changes = GeoZoneSourceChanges();
    _changes.at = GeoZoneEnvironment::currentDateTimeUtc();
    _changes.addedCount = static_cast<int>(added.count());
    _changes.removedCount = static_cast<int>(removed.count());
    _changes.changedCount = static_cast<int>(changedNames.count());
    _changes.added = sorted(added);
    _changes.removed = sorted(removed);
    _changes.changed = sorted(changedNames);
}

void GeoZoneSource::setChanges(const GeoZoneSourceChanges &changes)
{
    if (!(changes == _changes)) {
        _changes = changes;
        emit changed();
    }
}

QString GeoZoneSourceChanges::summary() const
{
    if (isEmpty()) {
        return QString();
    }
    QStringList counts;
    if (addedCount > 0) {
        counts.append(tr("%n new", nullptr, addedCount));
    }
    if (changedCount > 0) {
        counts.append(tr("%n changed", nullptr, changedCount));
    }
    if (removedCount > 0) {
        counts.append(tr("%n removed", nullptr, removedCount));
    }
    // The time of a reload today, otherwise the date too
    const QDateTime local = at.toLocalTime();
    const QLocale locale;
    const QString time = (local.date() == GeoZoneEnvironment::currentDateTimeUtc().toLocalTime().date()) ?
        locale.toString(local.time(), QLocale::ShortFormat) : locale.toString(local, QLocale::ShortFormat);
    return tr("Updated %1: %2").arg(time, counts.join(QStringLiteral(", ")));
}

QString GeoZoneSourceChanges::details() const
{
    const auto line = [](const QString &label, const QStringList &names, int count) {
        QString text = names.join(QStringLiteral(", "));
        if (count > names.count()) {
            text += tr(" and %n more", nullptr, static_cast<int>(count - names.count()));
        }
        return label.arg(text);
    };
    QStringList lines;
    if (addedCount > 0) {
        lines.append(line(tr("New: %1"), added, addedCount));
    }
    if (changedCount > 0) {
        lines.append(line(tr("Changed: %1"), changed, changedCount));
    }
    if (removedCount > 0) {
        lines.append(line(tr("Removed: %1"), removed, removedCount));
    }
    return lines.join(QLatin1Char('\n'));
}

QJsonObject GeoZoneSourceChanges::toJson() const
{
    if (isEmpty()) {
        return QJsonObject();
    }
    return QJsonObject{
        { QStringLiteral("at"), at.toUTC().toString(Qt::ISODate) },
        { QStringLiteral("addedCount"), addedCount },
        { QStringLiteral("changedCount"), changedCount },
        { QStringLiteral("removedCount"), removedCount },
        { QStringLiteral("added"), QJsonArray::fromStringList(added) },
        { QStringLiteral("changed"), QJsonArray::fromStringList(changed) },
        { QStringLiteral("removed"), QJsonArray::fromStringList(removed) },
    };
}

GeoZoneSourceChanges GeoZoneSourceChanges::fromJson(const QJsonObject &object)
{
    const auto names = [&object](const char *key) {
        QStringList list;
        for (const QJsonValue &value : object.value(QLatin1String(key)).toArray()) {
            list.append(value.toString());
        }
        return list.mid(0, maxNames);
    };
    GeoZoneSourceChanges changes;
    changes.at = QDateTime::fromString(object.value(QStringLiteral("at")).toString(), Qt::ISODate);
    changes.added = names("added");
    changes.changed = names("changed");
    changes.removed = names("removed");
    changes.addedCount = (std::max)(static_cast<int>(changes.added.count()), object.value(QStringLiteral("addedCount")).toInt());
    changes.changedCount = (std::max)(static_cast<int>(changes.changed.count()), object.value(QStringLiteral("changedCount")).toInt());
    changes.removedCount = (std::max)(static_cast<int>(changes.removed.count()), object.value(QStringLiteral("removedCount")).toInt());
    return changes;
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
        const QDateTime date = (!_issued.isValid() && _isDownloaded() && _checkedAt.isValid()) ? _checkedAt : dataDate();
        if (_validTo.isValid() && (_validTo < now)) {
            text = tr("Expired on %1").arg(_formatDate(_validTo));
        } else if (_validFrom.isValid() && (_validFrom > now)) {
            text = tr("Not valid until %1").arg(_formatDate(_validFrom));
        } else if (const QDate airac = airacDate(); airac.isValid() && (GeoZoneAirac::nextCycleStart(airac) <= now.toUTC().date())) {
            text = tr("AIRAC cycle %1 ended on %2, a newer cycle is in effect")
                       .arg(GeoZoneAirac::cycleName(airac), GeoZoneAirac::nextCycleStart(airac).toString(Qt::ISODate));
        } else if (isLive() && date.isValid() && (date.secsTo(now) > (qint64{ settings.liveStaleHours } * 3600))) {
            text = tr("Data is %n hour(s) old", nullptr, static_cast<int>(date.secsTo(now) / 3600));
        } else if (!isLive() && date.isValid() && (date.daysTo(now) > settings.staleDays)) {
            text = tr("Data is %n days old", nullptr, static_cast<int>(date.daysTo(now)));
        }
    }

    if (text != _validityText) {
        _validityText = text;
        emit validityChanged();
    }
}
