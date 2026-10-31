/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMissionCheck.h"
#include "ComplexMissionItem.h"
#include "FlightPathSegment.h"
#include "GeoZone.h"
#include "GeoZoneManager.h"
#include "MissionController.h"
#include "QmlObjectListModel.h"

#include <algorithm>
#include <cmath>

GeoZoneMissionCheck::GeoZoneMissionCheck(QObject *parent, GeoZoneManager *manager)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
{
    _updateTimer.setSingleShot(true);
    _updateTimer.setInterval(_updateDelayMSecs);
    (void) connect(&_updateTimer, &QTimer::timeout, this, &GeoZoneMissionCheck::update);

    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMissionCheck::_clearZones);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMissionCheck::_scheduleUpdate);
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
    }

    emit missionControllerChanged();
    _scheduleUpdate();
}

QList<QObject*> GeoZoneMissionCheck::crossedZones() const
{
    return QList<QObject*>(_crossedZones.cbegin(), _crossedZones.cend());
}

QString GeoZoneMissionCheck::warningText() const
{
    if (_crossedZones.isEmpty()) {
        return QString();
    }

    QStringList names;
    for (qsizetype i = 0; i < std::min<qsizetype>(_crossedZones.count(), maxListedZones); i++) {
        const GeoZone *const zone = _crossedZones[i];
        names.append(QStringLiteral("%1 (%2)").arg(zone->name().isEmpty() ? zone->identifier() : zone->name(), zone->restrictionText()));
    }
    QString text = tr("The mission path enters restricted geozones: %1").arg(names.join(QStringLiteral(", ")));
    if (_crossedZones.count() > maxListedZones) {
        text += tr(" and %1 more").arg(_crossedZones.count() - maxListedZones);
    }
    return text;
}

void GeoZoneMissionCheck::_scheduleUpdate()
{
    _updateTimer.start();
}

void GeoZoneMissionCheck::_clearZones()
{
    _updateTimer.stop();
    if (!_crossedZones.isEmpty()) {
        _crossedZones.clear();
        emit crossedZonesChanged();
    }
}

void GeoZoneMissionCheck::update()
{
    _updateTimer.stop();

    static const QList<GeoZone::Restriction> restrictions = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional };
    QList<GeoZone*> zones = _manager->zonesCrossingPaths(restrictions, pathsFromSegments(_missionSegments()));
    std::stable_sort(zones.begin(), zones.end(), [](const GeoZone *a, const GeoZone *b) {
        return GeoZone::restrictionSeverity(a->restriction()) < GeoZone::restrictionSeverity(b->restriction());
    });

    if (zones != _crossedZones) {
        _crossedZones = zones;
        emit crossedZonesChanged();
    }
}

QList<std::pair<QGeoCoordinate, QGeoCoordinate>> GeoZoneMissionCheck::_missionSegments() const
{
    QList<std::pair<QGeoCoordinate, QGeoCoordinate>> segments;
    if (!_missionController) {
        return segments;
    }

    const auto appendSegments = [&segments](const QmlObjectListModel *model) {
        for (int i = 0; i < model->count(); i++) {
            const FlightPathSegment *const segment = model->value<const FlightPathSegment*>(i);
            if (segment) {
                segments.append({ segment->coordinate1(), segment->coordinate2() });
            }
        }
    };

    appendSegments(_missionController->simpleFlightPathSegments());
    const QmlObjectListModel *const visualItems = _missionController->visualItems();
    for (int i = 0; visualItems && (i < visualItems->count()); i++) {
        ComplexMissionItem *const complexItem = visualItems->value<ComplexMissionItem*>(i);
        if (complexItem) {
            appendSegments(complexItem->flightPathSegments());
        }
    }
    return segments;
}

QList<QList<QGeoCoordinate>> GeoZoneMissionCheck::pathsFromSegments(const QList<std::pair<QGeoCoordinate, QGeoCoordinate>> &segments)
{
    QList<QList<QGeoCoordinate>> paths;
    for (const auto &[from, to] : segments) {
        if (!from.isValid() || !to.isValid()) {
            continue;
        }
        if (paths.isEmpty() || (paths.last().last() != from)) {
            paths.append({ from });
        }

        // Vehicles fly long legs along the great circle, which zone checks only follow with intermediate points
        const double distance = from.distanceTo(to);
        const double azimuth = from.azimuthTo(to);
        const int steps = std::min(static_cast<int>(std::ceil(distance / maxStepDistance)), maxStepsPerSegment);
        for (int step = 1; step < steps; step++) {
            paths.last().append(from.atDistanceAndAzimuth(distance * step / steps, azimuth));
        }
        paths.last().append(to);
    }
    return paths;
}
