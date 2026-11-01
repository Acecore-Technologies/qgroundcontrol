/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMissionCheck.h"
#include "GeoFenceController.h"
#include "GeoZone.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneCatalog.h"
#include "GeoZoneManager.h"
#include "GeoZoneSource.h"
#include "MissionController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QGCMapPolygon.h"
#include "QmlObjectListModel.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFutureWatcher>

#include <algorithm>
#include <cmath>

GeoZoneMissionCheck::GeoZoneMissionCheck(QObject *parent)
    : GeoZoneMissionCheck(GeoZoneManager::instance(), parent)
{
}

GeoZoneMissionCheck::GeoZoneMissionCheck(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
{
    _updateTimer.setSingleShot(true);
    _updateTimer.setInterval(_updateDelayMSecs);
    (void) connect(&_updateTimer, &QTimer::timeout, this, &GeoZoneMissionCheck::_updateConcurrently);

    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMissionCheck::_clearZones);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
    (void) connect(_manager, &GeoZoneManager::plannedStartChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
    (void) connect(_manager->authorisations(), &GeoZoneAuthorisations::changed, this, &GeoZoneMissionCheck::_scheduleUpdate);
}

MissionController *GeoZoneMissionCheck::missionController() const
{
    return _missionController;
}

void GeoZoneMissionCheck::setMissionController(MissionController *missionController)
{
    if (missionController == _missionController) {
        return;
    }

    if (_missionController) {
        (void) disconnect(_missionController, nullptr, this, nullptr);
    }
    _missionController = missionController;
    if (_missionController) {
        // Emitted after the flight path or any item coordinate changes
        (void) connect(_missionController, &MissionController::recalcTerrainProfile, this, &GeoZoneMissionCheck::_scheduleUpdate);
        (void) connect(_missionController, &MissionController::visualItemsChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
        (void) connect(_missionController, &MissionController::missionTimeChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
    }

    emit missionControllerChanged();
    _scheduleUpdate();
}

void GeoZoneMissionCheck::setFlightNow(bool flightNow)
{
    if (flightNow != _flightNow) {
        _flightNow = flightNow;
        emit flightNowChanged();
        _scheduleUpdate();
    }
}

namespace {

enum class NameStyle : quint8 {
    Name,
    WithRestriction,
};

QString _zoneNames(const QList<GeoZone*> &zones, NameStyle style)
{
    QStringList names;
    for (qsizetype i = 0; i < (std::min)(zones.count(), qsizetype{ GeoZoneMissionCheck::maxListedZones }); i++) {
        const GeoZone *const zone = zones[i];
        names.append((style == NameStyle::WithRestriction) ? QStringLiteral("%1 (%2)").arg(zone->displayName(), zone->restrictionText()) : zone->displayName());
    }
    QString text = names.join(QStringLiteral(", "));
    if (zones.count() > GeoZoneMissionCheck::maxListedZones) {
        text += GeoZoneMissionCheck::tr(" and %1 more").arg(zones.count() - GeoZoneMissionCheck::maxListedZones);
    }
    return text;
}

} // namespace

void GeoZoneMissionCheck::setGeoFenceController(GeoFenceController *geoFenceController)
{
    if (geoFenceController == _geoFenceController) {
        return;
    }
    if (_geoFenceController) {
        (void) disconnect(_geoFenceController, nullptr, this, nullptr);
        (void) disconnect(_geoFenceController->polygons(), nullptr, this, nullptr);
        (void) disconnect(_geoFenceController->circles(), nullptr, this, nullptr);
    }
    _geoFenceController = geoFenceController;
    if (_geoFenceController) {
        // Fence edits mark the lists dirty, additions and deletions change their counts
        for (QmlObjectListModel *fences : { _geoFenceController->polygons(), _geoFenceController->circles() }) {
            (void) connect(fences, &QmlObjectListModel::countChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
            (void) connect(fences, &QmlObjectListModel::dirtyChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
        }
        (void) connect(_geoFenceController, &GeoFenceController::supportedChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
    }
    emit geoFenceControllerChanged();
    _scheduleUpdate();
}

GeoFenceController *GeoZoneMissionCheck::geoFenceController() const
{
    return _geoFenceController;
}

QString GeoZoneMissionCheck::fenceText() const
{
    return _unfencedZones.isEmpty() ? QString() : tr("The GeoFence doesn't exclude prohibited zones on the path: %1").arg(_zoneNames(_unfencedZones, NameStyle::Name));
}

bool GeoZoneMissionCheck::fenceExcludes(GeoFenceController *geoFenceController, const GeoZone *zone)
{
    // Tolerance for vertices on a fence boundary, like those of fences added from the zone
    constexpr double toleranceMeters = 1;

    QList<QList<QGeoCoordinate>> fencePolygons;
    for (int i = 0; i < geoFenceController->polygons()->count(); i++) {
        const QGCFencePolygon *const polygon = geoFenceController->polygons()->value<QGCFencePolygon*>(i);
        if (!polygon->inclusion() && (polygon->count() >= 3)) {
            fencePolygons.append(polygon->coordinateList());
        }
    }
    QList<QGeoCircle> fenceCircles;
    for (int i = 0; i < geoFenceController->circles()->count(); i++) {
        QGCFenceCircle *const circle = geoFenceController->circles()->value<QGCFenceCircle*>(i);
        if (!circle->inclusion()) {
            fenceCircles.append(QGeoCircle(circle->center(), circle->radius()->rawValue().toDouble()));
        }
    }

    const auto polygonCovers = [](const QList<QGeoCoordinate> &fence, const QList<QGeoCoordinate> &points) {
        const GeoZoneGeometry::LocalProjection projection(fence.first());
        const QList<QPointF> ring = projection.toLocal(fence);
        return std::all_of(points.cbegin(), points.cend(), [&projection, &ring](const QGeoCoordinate &point) {
            const QPointF local = projection.toLocal(point);
            return GeoZoneGeometry::ringContains(ring, local) || (GeoZoneGeometry::distanceToRing(local, ring) <= toleranceMeters);
        });
    };
    const auto circleCovers = [](const QGeoCircle &fence, const QList<QGeoCoordinate> &points) {
        return std::all_of(points.cbegin(), points.cend(), [&fence](const QGeoCoordinate &point) {
            return fence.center().distanceTo(point) <= (fence.radius() + toleranceMeters);
        });
    };

    for (const QList<QGeoCoordinate> &zonePolygon : zone->exclusionPolygons()) {
        const bool covered = std::any_of(fencePolygons.cbegin(), fencePolygons.cend(), [&](const QList<QGeoCoordinate> &fence) { return polygonCovers(fence, zonePolygon); }) ||
                             std::any_of(fenceCircles.cbegin(), fenceCircles.cend(), [&](const QGeoCircle &fence) { return circleCovers(fence, zonePolygon); });
        if (!covered) {
            return false;
        }
    }
    for (const QGeoCircle &zoneCircle : zone->exclusionCircles()) {
        const bool covered =
            std::any_of(fenceCircles.cbegin(), fenceCircles.cend(), [&zoneCircle](const QGeoCircle &fence) {
                return (fence.center().distanceTo(zoneCircle.center()) + zoneCircle.radius()) <= (fence.radius() + toleranceMeters);
            }) ||
            std::any_of(fencePolygons.cbegin(), fencePolygons.cend(), [&zoneCircle](const QList<QGeoCoordinate> &fence) {
                const GeoZoneGeometry::LocalProjection projection(zoneCircle.center());
                const QList<QPointF> ring = projection.toLocal(fence);
                return GeoZoneGeometry::ringContains(ring, QPointF(0, 0)) && (GeoZoneGeometry::distanceToRing(QPointF(0, 0), ring) >= (zoneCircle.radius() - toleranceMeters));
            });
        if (!covered) {
            return false;
        }
    }
    return true;
}

QString GeoZoneMissionCheck::warningText() const
{
    return _crossedZones.isEmpty() ? QString() : tr("The mission path enters restricted geozones: %1").arg(_zoneNames(_crossedZones, NameStyle::WithRestriction));
}

QString GeoZoneMissionCheck::infoText() const
{
    QStringList lines;
    if (!_uSpaceZones.isEmpty()) {
        lines.append(tr("U-space airspace: %1. A flight authorisation from a U-space service provider is required.").arg(_zoneNames(_uSpaceZones, NameStyle::Name)));
    }
    if (!_authorisedZones.isEmpty()) {
        lines.append(tr("Authorised: %1.").arg(_zoneNames(_authorisedZones, NameStyle::Name)));
    }
    if (!_permittedZones.isEmpty()) {
        lines.append(tr("Areas without restriction: %1. Check their conditions.").arg(_zoneNames(_permittedZones, NameStyle::Name)));
    }
    if ((_inactiveZoneCount > 0) && !_flightNow) {
        lines.append(tr("%n zone(s) on the path are not active at the planned flight time.", nullptr, _inactiveZoneCount));
    }
    return lines.join(QStringLiteral("\n"));
}

std::pair<QDateTime, QDateTime> GeoZoneMissionCheck::plannedFlightTimes(const GeoZoneManager *manager, const MissionController *missionController)
{
    const QDateTime start = manager->plannedStart();
    if (!start.isValid()) {
        return {};
    }
    const double missionSeconds = missionController ? missionController->missionTime() : 0;
    return { start, start.addSecs(std::llround((std::max)(missionSeconds, 0.0))) };
}

QGeoRectangle GeoZoneMissionCheck::crossedZonesBoundingBox() const
{
    QGeoRectangle boundingBox;
    for (const GeoZone *zone : _crossedZones) {
        if (zone->boundingBox().isValid()) {
            boundingBox = boundingBox.isValid() ? boundingBox.united(zone->boundingBox()) : zone->boundingBox();
        }
    }
    return boundingBox;
}

void GeoZoneMissionCheck::_scheduleUpdate()
{
    _updateTimer.start();
}

void GeoZoneMissionCheck::_clearZones(GeoZoneSource *source)
{
    _evaluation++;
    // The other zones stay listed until the check runs again
    const auto fromSource = [source](const GeoZone *zone) { return zone->sourceKey() == source->key(); };
    const qsizetype removed = _crossedZones.removeIf(fromSource) + _uSpaceZones.removeIf(fromSource) + _permittedZones.removeIf(fromSource) +
                              _authorisedZones.removeIf(fromSource) +
                              _unfencedZones.removeIf(fromSource);
    if (removed > 0) {
        emit crossedZonesChanged();
    }
}

void GeoZoneMissionCheck::update()
{
    _updateTimer.stop();
    _evaluation++;
    _apply(zonesOnPath(_manager->catalog(), GeoZoneMissionPath::fromMission(_missionController)));
}

void GeoZoneMissionCheck::_updateConcurrently()
{
    const quint64 evaluation = ++_evaluation;
    const GeoZoneCatalog catalog = _manager->catalog();
    const GeoZoneMissionPath path = GeoZoneMissionPath::fromMission(_missionController);
    auto *const watcher = new QFutureWatcher<QList<GeoZone*>>(this);
    (void) connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, evaluation]() {
        watcher->deleteLater();
        // Zones of an outdated evaluation may be gone
        if (evaluation == _evaluation) {
            _apply(watcher->result());
        }
    });
    watcher->setFuture(QtConcurrent::run([catalog, path]() { return zonesOnPath(catalog, path); }));
}

QList<GeoZone*> GeoZoneMissionCheck::zonesOnPath(const GeoZoneCatalog &catalog, const GeoZoneMissionPath &path)
{
    const GeoZonePolicy anyTime = GeoZonePolicy::mission(QDateTime(), QDateTime());
    QList<GeoZone*> zones;
    for (const GeoZoneCatalog::PathHit &hit : catalog.pathHits(path.positionLists(), anyTime)) {
        if (!zones.contains(hit.zone)) {
            zones.append(hit.zone);
        }
    }
    // Camera footprints and turns extend past the flight lines, so whole areas count
    for (GeoZone *zone : catalog.zonesIntersectingAreas(path.areas(), anyTime)) {
        if (!zones.contains(zone)) {
            zones.append(zone);
        }
    }
    std::stable_sort(zones.begin(), zones.end(), [](const GeoZone *a, const GeoZone *b) {
        return GeoZone::restrictionSeverity(a->restriction()) < GeoZone::restrictionSeverity(b->restriction());
    });
    return zones;
}

void GeoZoneMissionCheck::_apply(QList<GeoZone*> zones)
{
    // Zones which aren't active at any time during the planned flight don't apply
    auto [plannedStart, plannedEnd] = plannedFlightTimes(_manager, _missionController);
    if (_flightNow) {
        plannedStart = QDateTime::currentDateTimeUtc();
        plannedEnd = plannedStart.addSecs(std::llround((std::max)(_missionController ? _missionController->missionTime() : 0.0, 0.0)));
    }
    // Zones authorised during the whole flight are listed for information
    const GeoZonePolicy policy = GeoZonePolicy::mission(plannedStart, plannedEnd).excludingAuthorised(_manager->authorisations());

    const int inactiveZoneCount = static_cast<int>(zones.removeIf([&policy](const GeoZone *zone) { return !policy.acceptsSchedule(zone); }));
    QList<GeoZone*> authorised;
    for (GeoZone *zone : std::as_const(zones)) {
        if (policy.authorised(zone)) {
            authorised.append(zone);
        }
    }
    (void) zones.removeIf([&policy](const GeoZone *zone) { return policy.authorised(zone); });

    QList<GeoZone*> crossed;
    QList<GeoZone*> uSpace;
    QList<GeoZone*> permitted;
    for (GeoZone *zone : std::as_const(zones)) {
        if (GeoZonePolicy::isRestricted(zone->restriction())) {
            crossed.append(zone);
        } else if (zone->restriction() == GeoZone::USpace) {
            uSpace.append(zone);
        } else {
            permitted.append(zone);
        }
    }

    // Vehicles which support a fence can be stopped at prohibited zones by it
    QList<GeoZone*> unfenced;
    if (_geoFenceController && _geoFenceController->supported()) {
        for (GeoZone *zone : std::as_const(crossed)) {
            if ((zone->restriction() == GeoZone::Prohibited) && !fenceExcludes(_geoFenceController, zone)) {
                unfenced.append(zone);
            }
        }
    }

    if ((crossed != _crossedZones) || (uSpace != _uSpaceZones) || (permitted != _permittedZones) || (authorised != _authorisedZones) || (inactiveZoneCount != _inactiveZoneCount) ||
        (unfenced != _unfencedZones)) {
        _unfencedZones = unfenced;
        _crossedZones = crossed;
        _uSpaceZones = uSpace;
        _permittedZones = permitted;
        _authorisedZones = authorised;
        _inactiveZoneCount = inactiveZoneCount;
        emit crossedZonesChanged();
    }
}
