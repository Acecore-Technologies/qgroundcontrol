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
#include <QtCore/QMutexLocker>
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
constexpr const char *kindKey = "kind";
constexpr const char *notificationKind = "notification";
constexpr const char *authorisationKind = "authorisation";  ///< Read, authorisations are written without kind
constexpr const char *prohibitedKey = "prohibited";
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
    // Making the zone, and projecting it for its first query, costs far more than a query
    const QMutexLocker locker(&scopeCache.mutex);
    const QByteArray zoneHash = zone->data().contentHash;
    if (scopeCache.scope && !zoneHash.isEmpty() && (scopeCache.zone == zone) && (scopeCache.zoneHash == zoneHash) && (scopeCache.ceiling == ceiling) &&
        (scopeCache.ceilingAboveGround == ceilingAboveGround) && (scopeCache.area == area) && (scopeCache.areaCircle == areaCircle)) {
        return scopeCache.scope;
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
    const std::shared_ptr<const GeoZone> scope = std::make_shared<const GeoZone>(data);
    scopeCache.zone = zone;
    scopeCache.zoneHash = zoneHash;
    scopeCache.ceiling = ceiling;
    scopeCache.ceilingAboveGround = ceilingAboveGround;
    scopeCache.area = area;
    scopeCache.areaCircle = areaCircle;
    scopeCache.scope = zoneHash.isEmpty() ? nullptr : scope;
    return scope;
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
    if (notification) {
        json.insert(kindKey, QLatin1String(notificationKind));
    }
    if (prohibitedZone) {
        json.insert(prohibitedKey, true);
    }
    return json;
}

bool GeoZoneAuthorisation::validFor(const GeoZone *zone) const
{
    const bool prohibited = prohibitedZone || (zone && (zone->restriction() == GeoZone::Prohibited));
    return notification || !prohibited || hasPermitAndEnd();
}

bool GeoZoneAuthorisation::ceilingValid(double meters)
{
    return std::isfinite(meters) && (meters > 0) && (meters < GeoZoneVertical::unlimitedSentinel);
}

bool GeoZoneAuthorisation::limitsValid() const
{
    if (validFrom.isValid() && validTo.isValid() && (validTo <= validFrom)) {
        return false;
    }
    if (ceiling && !ceilingValid(*ceiling)) {
        return false;
    }
    if (!area.isEmpty() && ((area.count() < 3) || !std::all_of(area.cbegin(), area.cend(), [](const QGeoCoordinate &vertex) { return vertex.isValid(); }))) {
        return false;
    }
    // Without circle its center is invalid and its radius negative
    const bool circle = areaCircle.center().isValid() || (areaCircle.radius() >= 0);
    if (circle && (!areaCircle.isValid() || !std::isfinite(areaCircle.radius()) || !(areaCircle.radius() > 0))) {
        return false;
    }
    return dailyFrom.isValid() == dailyTo.isValid();
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
    authorisation.aircraft = json.value(aircraftKey).toString();

    // Without scope, like those saved before scopes, the authorisation covers the whole zone. A limit which is there
    // but can't be read must not be taken as no limit: the entry is left out, so that the zone is checked.
    const auto rejected = [&authorisation](const char *key) {
        qCWarning(GeoZoneAuthorisationsLog) << "Authorisation of" << authorisation.zoneKey << "left out, unreadable" << key;
        return std::nullopt;
    };
    for (const auto &[key, dateTime] : { std::pair{ validFromKey, &authorisation.validFrom }, std::pair{ validToKey, &authorisation.validTo } }) {
        if (json.contains(QLatin1String(key))) {
            *dateTime = QDateTime::fromString(json.value(QLatin1String(key)).toString(), Qt::ISODate).toUTC();
            if (!dateTime->isValid()) {
                return rejected(key);
            }
        }
    }
    if (json.contains(QLatin1String(ceilingKey)) || json.contains(QLatin1String(ceilingReferenceKey))) {
        // The ceiling is written with its reference
        const QString reference = json.value(ceilingReferenceKey).toString();
        if (!json.value(ceilingKey).isDouble() || ((reference != QStringLiteral("AGL")) && (reference != QStringLiteral("AMSL")))) {
            return rejected(ceilingKey);
        }
        authorisation.ceiling = json.value(ceilingKey).toDouble();
        authorisation.ceilingAboveGround = (reference == QStringLiteral("AGL"));
    }
    if (json.contains(QLatin1String(areaKey))) {
        for (const QJsonValue &value : json.value(areaKey).toArray()) {
            const QJsonArray vertex = value.toArray();
            authorisation.area.append(QGeoCoordinate(vertex.at(0).toDouble(qQNaN()), vertex.at(1).toDouble(qQNaN())));
        }
        if (authorisation.area.count() < 3) {
            return rejected(areaKey);
        }
    }
    if (json.contains(QLatin1String(areaCircleKey))) {
        const QJsonObject circle = json.value(areaCircleKey).toObject();
        authorisation.areaCircle = QGeoCircle(QGeoCoordinate(circle.value(QStringLiteral("lat")).toDouble(qQNaN()), circle.value(QStringLiteral("lon")).toDouble(qQNaN())),
                                              circle.value(QStringLiteral("radius")).toDouble(qQNaN()));
        if (!authorisation.areaCircle.isValid()) {
            return rejected(areaCircleKey);
        }
    }
    if (json.contains(QLatin1String(dailyFromKey)) || json.contains(QLatin1String(dailyToKey))) {
        authorisation.dailyFrom = QTime::fromString(json.value(dailyFromKey).toString(), QStringLiteral("HH:mm"));
        authorisation.dailyTo = QTime::fromString(json.value(dailyToKey).toString(), QStringLiteral("HH:mm"));
        if (!authorisation.hasDailyHours()) {
            return rejected(dailyFromKey);
        }
    }
    if (json.contains(QLatin1String(kindKey))) {
        // An unknown kind may be less than an authorisation
        const QString kind = json.value(kindKey).toString();
        authorisation.notification = (kind == QLatin1String(notificationKind));
        if (!authorisation.notification && (kind != QLatin1String(authorisationKind))) {
            return rejected(kindKey);
        }
    }
    if (json.contains(QLatin1String(prohibitedKey))) {
        if (!json.value(prohibitedKey).isBool()) {
            return rejected(prohibitedKey);
        }
        authorisation.prohibitedZone = json.value(prohibitedKey).toBool();
    }
    if (!authorisation.limitsValid()) {
        return rejected("limits");
    }
    if (!authorisation.validFor(nullptr)) {
        qCWarning(GeoZoneAuthorisationsLog) << "Authorisation of the prohibited zone" << authorisation.zoneKey << "left out, it has no permit reference or no end";
        return std::nullopt;
    }
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
    // Checked against the zone as it is now: it may have become prohibited, or the entry may not say that it was
    const GeoZoneAuthorisation *const authorisation = _find(zone, false);
    return (authorisation && authorisation->validFor(zone)) ? authorisation : nullptr;
}

const GeoZoneAuthorisation *GeoZoneAuthorisations::_find(const GeoZone *zone, bool notification) const
{
    if (_authorisations.isEmpty() || !zone) {
        return nullptr;
    }
    const QString key = zoneKey(zone);
    for (const GeoZoneAuthorisation &authorisation : _authorisations) {
        if ((authorisation.zoneKey == key) && (authorisation.notification == notification)) {
            return &authorisation;
        }
    }
    return nullptr;
}

void GeoZoneAuthorisations::set(const GeoZoneAuthorisation &authorisation)
{
    QList<GeoZoneAuthorisation> updated = _authorisations;
    const auto it = std::find_if(updated.begin(), updated.end(), [&authorisation](const GeoZoneAuthorisation &existing) {
        return (existing.zoneKey == authorisation.zoneKey) && (existing.notification == authorisation.notification);
    });
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
        const auto it = std::find_if(merged.begin(), merged.end(), [&authorisation](const GeoZoneAuthorisation &existing) {
            return (existing.zoneKey == authorisation.zoneKey) && (existing.notification == authorisation.notification);
        });
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
    GeoZoneAuthorisation authorisation;
    const QString error = _readInput(zone, permit, validFrom, validTo, scope, false, authorisation);
    if (error.isEmpty()) {
        set(authorisation);
    }
    return error;
}

QString GeoZoneAuthorisations::_readInput(const QVariant &zone, const QString &permit, const QString &validFrom, const QString &validTo, const QVariantMap &scope,
                                     bool notification, GeoZoneAuthorisation &authorisation) const
{
    const GeoZoneInfo info = zone.value<GeoZoneInfo>();
    if (!info.valid()) {
        return tr("The zone is no longer loaded.");
    }

    // Local days, the last one included
    authorisation = GeoZoneAuthorisation();
    authorisation.zoneKey = zoneKey(info.zone());
    authorisation.zoneName = info.displayName();
    authorisation.permit = permit.trimmed();
    authorisation.notification = notification;
    authorisation.prohibitedZone = !notification && (info.zone()->restriction() == GeoZone::Prohibited);
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
    // A prohibited zone is only left out for a permit which can be referred to and which ends
    if (authorisation.prohibitedZone && authorisation.permit.isEmpty()) {
        return tr("Enter the permit number or reference: a prohibited zone can only be authorised with one.");
    }
    if (authorisation.prohibitedZone && !authorisation.validTo.isValid()) {
        return tr("Enter the last day of the permit: a prohibited zone can only be authorised until an end date.");
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
        if (!GeoZoneAuthorisation::ceilingValid(value * unitMeters())) {
            return tr("The ceiling is too high. Leave it empty when the permit has none.");
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
        const GeoZoneAuthorisation *const existing = _find(info.zone(), false);
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
    // The checks above tell which entry to correct, this is the one stored authorisations are read with
    if (!authorisation.limitsValid()) {
        return tr("The limits of the authorisation are not valid.");
    }
    return QString();
}

QString GeoZoneAuthorisations::recordNotification(const QVariant &zone, const QString &reference, const QString &validFrom, const QString &validTo)
{
    // Read like an authorisation without scope. It authorises nothing, so its reference and end stay optional.
    GeoZoneAuthorisation notification;
    const QString error = _readInput(zone, reference, validFrom, validTo, QVariantMap(), true, notification);
    if (error.isEmpty()) {
        set(notification);
    }
    return error;
}

void GeoZoneAuthorisations::revoke(const QString &zoneKey, bool notification)
{
    QList<GeoZoneAuthorisation> remaining = _authorisations;
    if (remaining.removeIf([&zoneKey, notification](const GeoZoneAuthorisation &authorisation) {
            return (authorisation.zoneKey == zoneKey) && (authorisation.notification == notification);
        }) > 0) {
        _reset(remaining);
        _save();
    }
}

QVariantMap GeoZoneAuthorisations::authorisationOf(const QVariant &zone) const
{
    const GeoZone *const geoZone = zone.value<GeoZoneInfo>().zone();
    const GeoZoneAuthorisation *const authorisation = _find(geoZone, false);
    if (!authorisation) {
        return QVariantMap();
    }
    QString status = statusText(*authorisation, GeoZoneEnvironment::currentDateTimeUtc());
    if (!authorisation->validFor(geoZone)) {
        // What it misses to stand for the zone
        status = authorisation->permit.trimmed().isEmpty() ?
                     (authorisation->validTo.isValid() ? tr("Not valid for a prohibited zone: add the permit reference") :
                                                         tr("Not valid for a prohibited zone: add the permit reference and the last day")) :
                     tr("Not valid for a prohibited zone: add the last day");
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
        { QStringLiteral("statusText"), status },
    };
}

QVariantMap GeoZoneAuthorisations::notificationOf(const QVariant &zone) const
{
    const GeoZoneAuthorisation *const notification = findNotification(zone.value<GeoZoneInfo>().zone());
    if (!notification) {
        return QVariantMap();
    }
    return QVariantMap{
        { QStringLiteral("zoneKey"), notification->zoneKey },
        { QStringLiteral("permit"), notification->permit },
        { QStringLiteral("validFrom"), firstDayText(notification->validFrom) },
        { QStringLiteral("validTo"), lastDayText(notification->validTo) },
        { QStringLiteral("statusText"), statusText(*notification, GeoZoneEnvironment::currentDateTimeUtc()) },
    };
}

QString GeoZoneAuthorisations::notificationRequirement(const QVariant &zone) const
{
    const GeoZone *const geoZone = zone.value<GeoZoneInfo>().zone();
    QString text = geoZone ? notificationText(geoZone, QDateTime(), QDateTime()) : QString();
    if (!text.isEmpty()) {
        text[0] = text[0].toUpper();
    }
    return text;
}

QString GeoZoneAuthorisations::notificationText(const GeoZone *zone, const QDateTime &start, const QDateTime &now, bool *overdue)
{
    if (overdue) {
        *overdue = false;
    }
    const qint64 lead = zone ? zone->data().notificationLeadSecs : -1;
    if (lead < 0) {
        return QString();
    }
    const QString authority = zone->data().notificationAuthority.isEmpty() ? tr("the zone authority") : zone->data().notificationAuthority;
    if (start.isValid()) {
        const QDateTime due = start.addSecs(-lead);
        if (overdue) {
            *overdue = now.isValid() && (due <= now);
        }
        return tr("notify %1 by %2").arg(authority, QLocale().toString(due.toLocalTime(), QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    if (lead == 0) {
        return tr("notify %1 before the flight").arg(authority);
    }
    // In days and hours, rounded up to the hour, abbreviated since English has no translation choosing plural forms
    const qint64 hours = (lead + 3599) / 3600;
    const qint64 days = hours / 24;
    const qint64 remainder = hours % 24;
    QStringList parts;
    if (days > 0) {
        parts.append(tr("%1 d").arg(days));
    }
    if ((remainder > 0) || (days == 0)) {
        parts.append(tr("%1 h").arg(remainder));
    }
    return tr("notify %1 %2 before the flight").arg(authority, parts.join(QStringLiteral(" ")));
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
    if (authorisation.notification) {
        return tr("Notified, %1").arg(validityText(authorisation, now).toLower());
    }
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
    case NotificationRole:
        return authorisation.notification;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GeoZoneAuthorisations::roleNames() const
{
    return { { ZoneKeyRole, "zoneKey" }, { ZoneNameRole, "zoneName" }, { PermitRole, "permit" }, { StatusTextRole, "statusText" },
             { NotificationRole, "notification" } };
}

void GeoZoneAuthorisations::_reset(const QList<GeoZoneAuthorisation> &authorisations)
{
    beginResetModel();
    _authorisations = authorisations;
    endResetModel();
    emit changed();
}

QString GeoZoneAuthorisations::_readFile(QList<GeoZoneAuthorisation> &authorisations, int &unread, bool &newer) const
{
    QFile file(_filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return file.errorString();
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return parseError.errorString();
    }
    const QJsonObject root = document.object();
    const QJsonValue version = root.value(versionKey);
    if (!document.isObject() || !version.isDouble() || (version.toInt() < 1) || !root.value(authorisationsKey).isArray()) {
        return tr("unexpected content");
    }
    if (version.toInt() > fileVersion) {
        // Its entries may limit authorisations in ways this version doesn't know
        newer = true;
        return QString();
    }
    for (const QJsonValue &value : root.value(authorisationsKey).toArray()) {
        if (const auto authorisation = GeoZoneAuthorisation::fromJson(value.toObject())) {
            authorisations.append(*authorisation);
        } else {
            unread++;
        }
    }
    return QString();
}

QString GeoZoneAuthorisations::_backUp(bool move) const
{
    const QString name = QStringLiteral("%1.%2").arg(_filePath, GeoZoneEnvironment::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    for (int attempt = 1; attempt <= 10; attempt++) {
        const QString backup = (attempt == 1) ? QStringLiteral("%1.bak").arg(name) : QStringLiteral("%1-%2.bak").arg(name).arg(attempt);
        if (QFile::exists(backup)) {
            continue;
        }
        return (move ? QFile::rename(_filePath, backup) : QFile::copy(_filePath, backup)) ? backup : QString();
    }
    return QString();
}

void GeoZoneAuthorisations::_load()
{
    if (_filePath.isEmpty() || !QFile::exists(_filePath)) {
        return;
    }
    QList<GeoZoneAuthorisation> authorisations;
    int unread = 0;
    bool newer = false;
    const QString error = _readFile(authorisations, unread, newer);
    if (newer) {
        _keepFile = true;
        _loadError = tr("The geozone authorisations were saved by a newer version of QGroundControl and are not loaded: no zone is authorised. "
                        "Authorisations added now will not be kept.");
    } else if (!error.isEmpty()) {
        // Nothing of it is known, so it is kept for the user instead of being replaced at the next change
        const QString backup = _backUp(true);
        _keepFile = backup.isEmpty();
        _loadError = _keepFile ?
            tr("The geozone authorisations could not be read (%1): no zone is authorised. The file %2 is left as it is, authorisations added now will not be kept.").arg(error, _filePath) :
            tr("The geozone authorisations could not be read (%1): no zone is authorised. The file was moved to %2. Add the authorisations again.").arg(error, backup);
    } else if (unread > 0) {
        // The next change writes the entries which were read only
        const QString backup = _backUp(false);
        _keepFile = backup.isEmpty();
        _loadError = _keepFile ?
            tr("%n geozone authorisation(s) could not be read and are left out, their zones are checked and alerted. "
               "The file %1 is left as it is, authorisations added now will not be kept.", nullptr, unread).arg(_filePath) :
            tr("%n geozone authorisation(s) could not be read and are left out, their zones are checked and alerted. "
               "The file as it was is kept in %1.", nullptr, unread).arg(backup);
    }
    _authorisations = authorisations;
    if (!_loadError.isEmpty()) {
        qCWarning(GeoZoneAuthorisationsLog) << _loadError;
        // Nothing is connected yet
        (void) QMetaObject::invokeMethod(this, [this]() { emit loadFailed(_loadError); }, Qt::QueuedConnection);
    }
}

void GeoZoneAuthorisations::_save()
{
    if (_filePath.isEmpty()) {
        return;
    }
    if (_keepFile) {
        // What the file holds isn't known here, or only in part
        if (!_keepFileReported) {
            _keepFileReported = true;
            emit saveFailed(tr("The geozone authorisations are not saved, they will be lost at the next start. %1").arg(_loadError));
        }
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
