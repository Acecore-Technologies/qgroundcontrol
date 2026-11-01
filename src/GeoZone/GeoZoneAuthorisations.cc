/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAuthorisations.h"
#include "GeoZone.h"
#include "GeoZoneInfo.h"
#include "QGCLoggingCategory.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>

#include <algorithm>

QGC_LOGGING_CATEGORY(GeoZoneAuthorisationsLog, "qgc.geozone.geozoneauthorisations")

namespace {

constexpr const char *zoneKeyKey = "zone";
constexpr const char *zoneNameKey = "name";
constexpr const char *permitKey = "permit";
constexpr const char *validFromKey = "validFrom";
constexpr const char *validToKey = "validTo";
constexpr const char *versionKey = "version";
constexpr const char *authorisationsKey = "authorisations";
constexpr int fileVersion = 1;

/// @return Last day of an exclusive end, in local time
QString lastDayText(const QDateTime &validTo)
{
    return validTo.isValid() ? validTo.toLocalTime().addSecs(-1).date().toString(Qt::ISODate) : QString();
}

QString firstDayText(const QDateTime &validFrom)
{
    return validFrom.isValid() ? validFrom.toLocalTime().date().toString(Qt::ISODate) : QString();
}

} // namespace

bool GeoZoneAuthorisation::covers(const QDateTime &start, const QDateTime &end) const
{
    return (!validFrom.isValid() || (validFrom <= start)) && (!validTo.isValid() || (end < validTo));
}

QJsonObject GeoZoneAuthorisation::toJson() const
{
    QJsonObject json{ { zoneKeyKey, zoneKey }, { zoneNameKey, zoneName } };
    if (!permit.isEmpty()) {
        json.insert(permitKey, permit);
    }
    if (validFrom.isValid()) {
        json.insert(validFromKey, validFrom.toUTC().toString(Qt::ISODate));
    }
    if (validTo.isValid()) {
        json.insert(validToKey, validTo.toUTC().toString(Qt::ISODate));
    }
    return json;
}

std::optional<GeoZoneAuthorisation> GeoZoneAuthorisation::fromJson(const QJsonObject &json)
{
    GeoZoneAuthorisation authorisation;
    authorisation.zoneKey = json.value(zoneKeyKey).toString();
    if (authorisation.zoneKey.isEmpty()) {
        return std::nullopt;
    }
    authorisation.zoneName = json.value(zoneNameKey).toString();
    authorisation.permit = json.value(permitKey).toString();
    authorisation.validFrom = QDateTime::fromString(json.value(validFromKey).toString(), Qt::ISODate).toUTC();
    authorisation.validTo = QDateTime::fromString(json.value(validToKey).toString(), Qt::ISODate).toUTC();
    return authorisation;
}

GeoZoneAuthorisations::GeoZoneAuthorisations(const QString &filePath, QObject *parent)
    : QAbstractListModel(parent)
    , _filePath(filePath)
{
    _load();
}

QString GeoZoneAuthorisations::zoneKey(const GeoZone *zone)
{
    if (!zone) {
        return QString();
    }
    return zone->identifier().isEmpty() ? zone->key() : QStringLiteral("%1/%2").arg(zone->country(), zone->identifier());
}

const GeoZoneAuthorisation *GeoZoneAuthorisations::find(const GeoZone *zone) const
{
    if (_authorisations.isEmpty() || !zone) {
        return nullptr;
    }
    const QString key = zoneKey(zone);
    for (const GeoZoneAuthorisation &authorisation : _authorisations) {
        if (authorisation.zoneKey == key) {
            return &authorisation;
        }
    }
    return nullptr;
}

void GeoZoneAuthorisations::set(const GeoZoneAuthorisation &authorisation)
{
    QList<GeoZoneAuthorisation> updated = _authorisations;
    const auto it = std::find_if(updated.begin(), updated.end(), [&authorisation](const GeoZoneAuthorisation &existing) { return existing.zoneKey == authorisation.zoneKey; });
    if (it == updated.end()) {
        updated.append(authorisation);
    } else {
        *it = authorisation;
    }
    if (updated != _authorisations) {
        _reset(updated);
        _save();
    }
}

QList<GeoZoneAuthorisation> GeoZoneAuthorisations::_merged(const QList<GeoZoneAuthorisation> &authorisations) const
{
    QList<GeoZoneAuthorisation> merged = _authorisations;
    for (const GeoZoneAuthorisation &authorisation : authorisations) {
        const auto it = std::find_if(merged.begin(), merged.end(), [&authorisation](const GeoZoneAuthorisation &existing) { return existing.zoneKey == authorisation.zoneKey; });
        if (it == merged.end()) {
            merged.append(authorisation);
        } else if (it->validTo.isValid() && (!authorisation.validTo.isValid() || (authorisation.validTo > it->validTo))) {
            *it = authorisation;
        }
    }
    return merged;
}

void GeoZoneAuthorisations::merge(const QList<GeoZoneAuthorisation> &authorisations)
{
    const QList<GeoZoneAuthorisation> merged = _merged(authorisations);
    if (merged != _authorisations) {
        _reset(merged);
        _save();
    }
}

void GeoZoneAuthorisations::offer(const QList<GeoZoneAuthorisation> &authorisations)
{
    // Only those which would add a zone or extend an authorisation are worth asking about
    QList<GeoZoneAuthorisation> pending;
    for (const GeoZoneAuthorisation &authorisation : authorisations) {
        if (_merged({ authorisation }) != _authorisations) {
            pending.append(authorisation);
        }
    }
    if (pending != _pending) {
        _pending = pending;
        emit pendingChanged();
    }
}

QVariantList GeoZoneAuthorisations::pendingAuthorisations() const
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVariantList list;
    for (const GeoZoneAuthorisation &authorisation : _pending) {
        list.append(QVariantMap{
            { QStringLiteral("zoneName"), authorisation.zoneName.isEmpty() ? authorisation.zoneKey : authorisation.zoneName },
            { QStringLiteral("permit"), authorisation.permit },
            { QStringLiteral("statusText"), statusText(authorisation, now) },
        });
    }
    return list;
}

void GeoZoneAuthorisations::acceptPending()
{
    const QList<GeoZoneAuthorisation> pending = _pending;
    dismissPending();
    merge(pending);
}

void GeoZoneAuthorisations::dismissPending()
{
    if (!_pending.isEmpty()) {
        _pending.clear();
        emit pendingChanged();
    }
}

QString GeoZoneAuthorisations::authorise(const QVariant &zone, const QString &permit, const QString &validFrom, const QString &validTo)
{
    const GeoZoneInfo info = zone.value<GeoZoneInfo>();
    if (!info.valid()) {
        return tr("The zone is no longer loaded.");
    }

    // Local days, the last one included
    GeoZoneAuthorisation authorisation;
    authorisation.zoneKey = zoneKey(info.zone());
    authorisation.zoneName = info.displayName();
    authorisation.permit = permit.trimmed();
    if (!validFrom.trimmed().isEmpty()) {
        const QDate date = QDate::fromString(validFrom.trimmed(), Qt::ISODate);
        if (!date.isValid()) {
            return tr("Enter the first day like %1.").arg(QDate::currentDate().toString(Qt::ISODate));
        }
        authorisation.validFrom = QDateTime(date, QTime(0, 0)).toUTC();
    }
    if (!validTo.trimmed().isEmpty()) {
        const QDate date = QDate::fromString(validTo.trimmed(), Qt::ISODate);
        if (!date.isValid()) {
            return tr("Enter the last day like %1.").arg(QDate::currentDate().toString(Qt::ISODate));
        }
        authorisation.validTo = QDateTime(date.addDays(1), QTime(0, 0)).toUTC();
    }
    if (authorisation.validFrom.isValid() && authorisation.validTo.isValid() && (authorisation.validTo <= authorisation.validFrom)) {
        return tr("The last day is before the first day.");
    }

    set(authorisation);
    return QString();
}

void GeoZoneAuthorisations::revoke(const QString &zoneKey)
{
    QList<GeoZoneAuthorisation> remaining = _authorisations;
    if (remaining.removeIf([&zoneKey](const GeoZoneAuthorisation &authorisation) { return authorisation.zoneKey == zoneKey; }) > 0) {
        _reset(remaining);
        _save();
    }
}

QVariantMap GeoZoneAuthorisations::authorisationOf(const QVariant &zone) const
{
    const GeoZoneAuthorisation *const authorisation = find(zone.value<GeoZoneInfo>().zone());
    if (!authorisation) {
        return QVariantMap();
    }
    return QVariantMap{
        { QStringLiteral("zoneKey"), authorisation->zoneKey },
        { QStringLiteral("permit"), authorisation->permit },
        { QStringLiteral("validFrom"), firstDayText(authorisation->validFrom) },
        { QStringLiteral("validTo"), lastDayText(authorisation->validTo) },
        { QStringLiteral("statusText"), statusText(*authorisation, QDateTime::currentDateTimeUtc()) },
    };
}

QString GeoZoneAuthorisations::statusText(const GeoZoneAuthorisation &authorisation, const QDateTime &now)
{
    if (authorisation.expired(now)) {
        return tr("Expired after %1").arg(lastDayText(authorisation.validTo));
    }
    if (authorisation.validFrom.isValid() && (now < authorisation.validFrom)) {
        return authorisation.validTo.isValid() ? tr("Valid from %1 to %2").arg(firstDayText(authorisation.validFrom), lastDayText(authorisation.validTo)) :
                                                 tr("Valid from %1").arg(firstDayText(authorisation.validFrom));
    }
    return authorisation.validTo.isValid() ? tr("Valid until %1").arg(lastDayText(authorisation.validTo)) : tr("Valid");
}

int GeoZoneAuthorisations::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant GeoZoneAuthorisations::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return QVariant();
    }
    const GeoZoneAuthorisation &authorisation = _authorisations[index.row()];
    switch (role) {
    case ZoneKeyRole:
        return authorisation.zoneKey;
    case ZoneNameRole:
        return authorisation.zoneName;
    case PermitRole:
        return authorisation.permit;
    case StatusTextRole:
        return statusText(authorisation, QDateTime::currentDateTimeUtc());
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GeoZoneAuthorisations::roleNames() const
{
    return { { ZoneKeyRole, "zoneKey" }, { ZoneNameRole, "zoneName" }, { PermitRole, "permit" }, { StatusTextRole, "statusText" } };
}

void GeoZoneAuthorisations::_reset(const QList<GeoZoneAuthorisation> &authorisations)
{
    beginResetModel();
    _authorisations = authorisations;
    endResetModel();
    emit changed();
}

void GeoZoneAuthorisations::_load()
{
    if (_filePath.isEmpty()) {
        return;
    }
    QFile file(_filePath);
    if (!file.exists()) {
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(GeoZoneAuthorisationsLog) << "Unable to read" << _filePath << file.errorString();
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QList<GeoZoneAuthorisation> authorisations;
    for (const QJsonValue &value : root.value(authorisationsKey).toArray()) {
        if (const auto authorisation = GeoZoneAuthorisation::fromJson(value.toObject())) {
            authorisations.append(*authorisation);
        }
    }
    _authorisations = authorisations;
}

void GeoZoneAuthorisations::_save()
{
    if (_filePath.isEmpty()) {
        return;
    }
    (void) QDir().mkpath(QFileInfo(_filePath).absolutePath());
    QJsonArray values;
    for (const GeoZoneAuthorisation &authorisation : std::as_const(_authorisations)) {
        values.append(authorisation.toJson());
    }
    QSaveFile file(_filePath);
    if (!file.open(QIODevice::WriteOnly) ||
        (file.write(QJsonDocument(QJsonObject{ { versionKey, fileVersion }, { authorisationsKey, values } }).toJson()) < 0) ||
        !file.commit()) {
        qCWarning(GeoZoneAuthorisationsLog) << "Unable to save" << _filePath << file.errorString();
        emit saveFailed(tr("The geozone authorisations could not be saved, they will be lost at the next start. %1").arg(file.errorString()));
    }
}
