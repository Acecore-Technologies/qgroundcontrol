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
#include "GeoZoneEnvironment.h"
#include "GeoZoneInfo.h"
#include "GeoZoneVertical.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(GeoZoneAuthorisationsLog, "qgc.geozone.geozoneauthorisations")

namespace {

constexpr const char *zoneKeyKey = "zone";
constexpr const char *zoneNameKey = "name";
constexpr const char *permitKey = "permit";
constexpr const char *validFromKey = "validFrom";
constexpr const char *validToKey = "validTo";
constexpr const char *ceilingKey = "ceiling";
constexpr const char *ceilingReferenceKey = "ceilingReference";
constexpr const char *areaKey = "area";
constexpr const char *areaCircleKey = "areaCircle";
constexpr const char *dailyFromKey = "dailyFrom";
constexpr const char *dailyToKey = "dailyTo";
constexpr const char *aircraftKey = "aircraft";
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

QString timeText(const QTime &time)
{
    return time.toString(QStringLiteral("HH:mm"));
}

/// @return true if the zone has limits above ground, so that the ceiling of its authorisations is too
bool aboveGround(const GeoZone *zone)
{
    const QList<GeoZone::Volume> &volumes = zone->data().volumes;
    return std::any_of(volumes.cbegin(), volumes.cend(), [](const GeoZone::Volume &volume) { return GeoZoneVertical::aboveGround(volume); });
}

/// Meters per vertical distance unit
double unitMeters()
{
    return 1.0 / GeoZoneEnvironment::metersToVerticalDistanceUnits(1.0);
}

} // namespace

bool GeoZoneAuthorisation::covers(const QDateTime &start, const QDateTime &end) const
{
    return (!validFrom.isValid() || (validFrom <= start)) && (!validTo.isValid() || (end < validTo)) && withinDailyHours(start, end);
}

bool GeoZoneAuthorisation::withinDailyHours(const QDateTime &start, const QDateTime &end) const
{
    if (!hasDailyHours() || (dailyFrom == dailyTo) || !start.isValid()) {
        return true;
    }
    const QDateTime localStart = start.toLocalTime();
    const QDateTime localEnd = ((end.isValid() && (end > start)) ? end : start).toLocalTime();
    // The daily period containing the start, which may begin the day before when it spans midnight
    const QDate day = localStart.date();
    QDateTime periodStart(day, dailyFrom);
    QDateTime periodEnd(day, dailyTo);
    if (dailyTo < dailyFrom) {
        if (localStart.time() < dailyTo) {
            periodStart = QDateTime(day.addDays(-1), dailyFrom);
        } else {
            periodEnd = QDateTime(day.addDays(1), dailyTo);
        }
    }
    return (periodStart <= localStart) && (localEnd <= periodEnd);
}

std::shared_ptr<const GeoZone> GeoZoneAuthorisation::scopeZone(const GeoZone *zone) const
{
    if (!zone) {
        return nullptr;
    }
    GeoZone::Volume limits;
    if (ceiling) {
        limits.upperLimit = *ceiling;
        limits.upperReference = ceilingAboveGround ? GeoZone::VerticalReference::Ground : GeoZone::VerticalReference::MeanSeaLevel;
    }
    QList<GeoZone::Volume> volumes;
    if (areaCircle.isValid()) {
        GeoZone::Volume volume = limits;
        volume.isCircle = true;
        volume.center = areaCircle.center();
        volume.radius = areaCircle.radius();
        volumes.append(volume);
    } else if (area.count() >= 3) {
        GeoZone::Volume volume = limits;
        volume.rings = { GeoZone::toRing(area) };
        volumes.append(volume);
    } else {
        // The zone's horizontal extent
        for (GeoZone::Volume volume : zone->data().volumes) {
            volume.lowerLimit = limits.lowerLimit;
            volume.upperLimit = limits.upperLimit;
            volume.lowerReference = limits.lowerReference;
            volume.upperReference = limits.upperReference;
            volume.feet = false;
            volumes.append(volume);
        }
    }
    GeoZone::Data data;
    data.identifier = zone->identifier();
    data.name = zone->name();
    data.restriction = zone->restriction();
    data.volumes = volumes;
    return std::make_shared<const GeoZone>(data);
}

bool GeoZoneAuthorisation::scopeContains(const GeoZone *zone, const GeoZone::Position &position) const
{
    if (ceiling && (std::isnan(position.altitudeAMSL) || (ceilingAboveGround && std::isnan(position.terrainAMSL)))) {
        return false;
    }
    const std::shared_ptr<const GeoZone> scope = scopeZone(zone);
    return scope && scope->contains(position);
}

QList<QGeoCoordinate> GeoZoneAuthorisation::missionArea(const QGeoRectangle &missionBounds)
{
    if (!missionBounds.isValid()) {
        return {};
    }
    const QGeoCoordinate topLeft = missionBounds.topLeft().atDistanceAndAzimuth(areaBuffer, 0).atDistanceAndAzimuth(areaBuffer, 270);
    const QGeoCoordinate bottomRight = missionBounds.bottomRight().atDistanceAndAzimuth(areaBuffer, 180).atDistanceAndAzimuth(areaBuffer, 90);
    return { topLeft, QGeoCoordinate(topLeft.latitude(), bottomRight.longitude()), bottomRight, QGeoCoordinate(bottomRight.latitude(), topLeft.longitude()) };
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
    if (ceiling) {
        json.insert(ceilingKey, *ceiling);
        json.insert(ceilingReferenceKey, ceilingAboveGround ? QStringLiteral("AGL") : QStringLiteral("AMSL"));
    }
    if (area.count() >= 3) {
        QJsonArray vertices;
        for (const QGeoCoordinate &vertex : area) {
            vertices.append(QJsonArray{ vertex.latitude(), vertex.longitude() });
        }
        json.insert(areaKey, vertices);
    }
    if (areaCircle.isValid()) {
        json.insert(areaCircleKey, QJsonObject{ { QStringLiteral("lat"), areaCircle.center().latitude() }, { QStringLiteral("lon"), areaCircle.center().longitude() },
                                                { QStringLiteral("radius"), areaCircle.radius() } });
    }
    if (hasDailyHours()) {
        json.insert(dailyFromKey, timeText(dailyFrom));
        json.insert(dailyToKey, timeText(dailyTo));
    }
    if (!aircraft.isEmpty()) {
        json.insert(aircraftKey, aircraft);
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

    // Without scope, like those saved before scopes, the authorisation covers the whole zone
    if (json.value(ceilingKey).isDouble()) {
        authorisation.ceiling = json.value(ceilingKey).toDouble();
        authorisation.ceilingAboveGround = (json.value(ceilingReferenceKey).toString() == QStringLiteral("AGL"));
    }
    for (const QJsonValue &value : json.value(areaKey).toArray()) {
        const QJsonArray vertex = value.toArray();
        const QGeoCoordinate coordinate(vertex.at(0).toDouble(qQNaN()), vertex.at(1).toDouble(qQNaN()));
        if (coordinate.isValid()) {
            authorisation.area.append(coordinate);
        }
    }
    if (authorisation.area.count() < 3) {
        authorisation.area.clear();
    }
    const QJsonObject circle = json.value(areaCircleKey).toObject();
    const QGeoCircle areaCircle(QGeoCoordinate(circle.value(QStringLiteral("lat")).toDouble(qQNaN()), circle.value(QStringLiteral("lon")).toDouble(qQNaN())),
                                circle.value(QStringLiteral("radius")).toDouble(-1));
    if (areaCircle.isValid() && (areaCircle.radius() > 0)) {
        authorisation.areaCircle = areaCircle;
    }
    const QTime dailyFrom = QTime::fromString(json.value(dailyFromKey).toString(), QStringLiteral("HH:mm"));
    const QTime dailyTo = QTime::fromString(json.value(dailyToKey).toString(), QStringLiteral("HH:mm"));
    if (dailyFrom.isValid() && dailyTo.isValid()) {
        authorisation.dailyFrom = dailyFrom;
        authorisation.dailyTo = dailyTo;
    }
    authorisation.aircraft = json.value(aircraftKey).toString();
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
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
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

QString GeoZoneAuthorisations::authorise(const QVariant &zone, const QString &permit, const QString &validFrom, const QString &validTo, const QVariantMap &scope)
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
            return tr("Enter the first day like %1.").arg(GeoZoneEnvironment::currentDateTimeUtc().toLocalTime().date().toString(Qt::ISODate));
        }
        authorisation.validFrom = QDateTime(date, QTime(0, 0)).toUTC();
    }
    if (!validTo.trimmed().isEmpty()) {
        const QDate date = QDate::fromString(validTo.trimmed(), Qt::ISODate);
        if (!date.isValid()) {
            return tr("Enter the last day like %1.").arg(GeoZoneEnvironment::currentDateTimeUtc().toLocalTime().date().toString(Qt::ISODate));
        }
        authorisation.validTo = QDateTime(date.addDays(1), QTime(0, 0)).toUTC();
    }
    if (authorisation.validFrom.isValid() && authorisation.validTo.isValid() && (authorisation.validTo <= authorisation.validFrom)) {
        return tr("The last day is before the first day.");
    }

    // The scope
    const QString ceiling = scope.value(QStringLiteral("ceiling")).toString().trimmed();
    if (!ceiling.isEmpty()) {
        bool ok = false;
        double value = QLocale().toDouble(ceiling, &ok);
        if (!ok) {
            value = ceiling.toDouble(&ok);
        }
        if (!ok || !(value > 0)) {
            return tr("Enter the ceiling as a height, like 120.");
        }
        authorisation.ceiling = value * unitMeters();
        authorisation.ceilingAboveGround = aboveGround(info.zone());
    }
    const QString dailyFrom = scope.value(QStringLiteral("dailyFrom")).toString().trimmed();
    const QString dailyTo = scope.value(QStringLiteral("dailyTo")).toString().trimmed();
    if (!dailyFrom.isEmpty() || !dailyTo.isEmpty()) {
        // Like 08:00, or 0800 as numeric keyboards type it
        const auto time = [](const QString &text) {
            const QTime colon = QTime::fromString(text, QStringLiteral("H:mm"));
            return colon.isValid() ? colon : QTime::fromString(text, QStringLiteral("HHmm"));
        };
        authorisation.dailyFrom = time(dailyFrom);
        authorisation.dailyTo = time(dailyTo);
        if (!authorisation.hasDailyHours()) {
            return tr("Enter both daily hours like 08:00 and 18:00.");
        }
    }
    authorisation.aircraft = scope.value(QStringLiteral("aircraft")).toString().trimmed();
    if (scope.value(QStringLiteral("limitToMission")).toBool()) {
        const GeoZoneAuthorisation *const existing = find(info.zone());
        const QGeoRectangle missionBounds = scope.value(QStringLiteral("missionArea")).value<QGeoRectangle>();
        if (missionBounds.isValid()) {
            authorisation.area = GeoZoneAuthorisation::missionArea(missionBounds);
        } else if (existing && existing->hasArea()) {
            authorisation.area = existing->area;
            authorisation.areaCircle = existing->areaCircle;
        } else {
            return tr("Plan a mission first to limit the authorisation to its area.");
        }
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
        { QStringLiteral("ceiling"), authorisation->ceiling ? QString::number(qRound(*authorisation->ceiling / unitMeters())) : QString() },
        { QStringLiteral("dailyFrom"), authorisation->hasDailyHours() ? timeText(authorisation->dailyFrom) : QString() },
        { QStringLiteral("dailyTo"), authorisation->hasDailyHours() ? timeText(authorisation->dailyTo) : QString() },
        { QStringLiteral("aircraft"), authorisation->aircraft },
        { QStringLiteral("limitToMission"), authorisation->hasArea() },
        { QStringLiteral("statusText"), statusText(*authorisation, GeoZoneEnvironment::currentDateTimeUtc()) },
    };
}

QString GeoZoneAuthorisations::ceilingUnits(const QVariant &zone) const
{
    const GeoZone *const geoZone = zone.value<GeoZoneInfo>().zone();
    return QStringLiteral("%1 %2").arg(GeoZoneEnvironment::verticalDistanceUnits(), (geoZone && aboveGround(geoZone)) ? QStringLiteral("AGL") : QStringLiteral("AMSL"));
}

QString GeoZoneAuthorisations::heightText(double meters, bool aboveGround)
{
    return QStringLiteral("%1 %2 %3").arg(qRound(GeoZoneEnvironment::metersToVerticalDistanceUnits(meters)))
        .arg(GeoZoneEnvironment::verticalDistanceUnits(), aboveGround ? QStringLiteral("AGL") : QStringLiteral("AMSL"));
}

QString GeoZoneAuthorisations::scopeText(const GeoZoneAuthorisation &authorisation)
{
    QStringList limits;
    if (authorisation.ceiling) {
        limits.append(tr("up to %1").arg(heightText(*authorisation.ceiling, authorisation.ceilingAboveGround)));
    }
    if (authorisation.hasDailyHours()) {
        limits.append(tr("%1–%2 daily").arg(timeText(authorisation.dailyFrom), timeText(authorisation.dailyTo)));
    }
    if (authorisation.hasArea()) {
        limits.append(tr("within its area"));
    }
    if (!authorisation.aircraft.isEmpty()) {
        limits.append(tr("aircraft %1").arg(authorisation.aircraft));
    }
    return limits.join(QStringLiteral(", "));
}

QString GeoZoneAuthorisations::statusText(const GeoZoneAuthorisation &authorisation, const QDateTime &now)
{
    const QString scope = scopeText(authorisation);
    return scope.isEmpty() ? validityText(authorisation, now) : QStringLiteral("%1, %2").arg(validityText(authorisation, now), scope);
}

QString GeoZoneAuthorisations::validityText(const GeoZoneAuthorisation &authorisation, const QDateTime &now)
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

QString GeoZoneAuthorisations::uncoveredText(const GeoZoneAuthorisation &authorisation, const QDateTime &start, const QDateTime &end)
{
    if (authorisation.validFrom.isValid() && (start < authorisation.validFrom)) {
        return tr("authorised from %1").arg(firstDayText(authorisation.validFrom));
    }
    if (authorisation.validTo.isValid() && (authorisation.validTo <= end)) {
        return tr("authorised until %1").arg(lastDayText(authorisation.validTo));
    }
    if (!authorisation.withinDailyHours(start, end)) {
        return tr("authorised %1–%2 daily").arg(timeText(authorisation.dailyFrom), timeText(authorisation.dailyTo));
    }
    return QString();
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
        return statusText(authorisation, GeoZoneEnvironment::currentDateTimeUtc());
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
