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
#include "GeoZoneSource.h"
#include "MissionController.h"
#include "QGCMapPolygon.h"
#include "QmlObjectListModel.h"
#include "StructureScanComplexItem.h"
#include "TransectStyleComplexItem.h"

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
    if (!_permittedZones.isEmpty()) {
        lines.append(tr("Areas without restriction: %1. Check their conditions.").arg(_zoneNames(_permittedZones, NameStyle::Name)));
    }
    return lines.join(QStringLiteral("\n"));
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
    // The other zones stay listed until the check runs again
    const auto fromSource = [source](const GeoZone *zone) { return zone->parent() == source; };
    const qsizetype removed = _crossedZones.removeIf(fromSource) + _uSpaceZones.removeIf(fromSource) + _permittedZones.removeIf(fromSource);
    if (removed > 0) {
        emit crossedZonesChanged();
    }
}

void GeoZoneMissionCheck::update()
{
    _updateTimer.stop();

    static const QList<GeoZone::Restriction> restrictions = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::USpace, GeoZone::NoRestriction };
    QList<GeoZone*> zones = _manager->zonesCrossingPaths(restrictions, pathsFromSegments(_missionSegments()));
    // Camera footprints and turns extend past the flight lines, so whole areas count
    for (GeoZone *zone : _manager->zonesIntersectingAreas(restrictions, _missionAreas())) {
        if (!zones.contains(zone)) {
            zones.append(zone);
        }
    }
    std::stable_sort(zones.begin(), zones.end(), [](const GeoZone *a, const GeoZone *b) {
        return GeoZone::restrictionSeverity(a->restriction()) < GeoZone::restrictionSeverity(b->restriction());
    });

    QList<GeoZone*> crossed;
    QList<GeoZone*> uSpace;
    QList<GeoZone*> permitted;
    for (GeoZone *zone : std::as_const(zones)) {
        switch (zone->restriction()) {
        case GeoZone::USpace:
            uSpace.append(zone);
            break;
        case GeoZone::NoRestriction:
            permitted.append(zone);
            break;
        case GeoZone::Prohibited:
        case GeoZone::ReqAuthorisation:
        case GeoZone::Conditional:
        case GeoZone::Unknown:
            crossed.append(zone);
            break;
        }
    }

    if ((crossed != _crossedZones) || (uSpace != _uSpaceZones) || (permitted != _permittedZones)) {
        _crossedZones = crossed;
        _uSpaceZones = uSpace;
        _permittedZones = permitted;
        emit crossedZonesChanged();
    }
}

QList<GeoZoneMissionCheck::Segment> GeoZoneMissionCheck::_missionSegments() const
{
    QList<Segment> segments;
    if (!_missionController) {
        return segments;
    }

    const auto appendSegments = [&segments](const QmlObjectListModel *model) {
        for (int i = 0; i < model->count(); i++) {
            const FlightPathSegment *const flightPathSegment = model->value<const FlightPathSegment*>(i);
            if (!flightPathSegment) {
                continue;
            }
            Segment segment;
            segment.from = flightPathSegment->coordinate1();
            segment.to = flightPathSegment->coordinate2();
            segment.fromAltitudeAMSL = flightPathSegment->coord1AMSLAlt();
            segment.toAltitudeAMSL = flightPathSegment->coord2AMSLAlt();
            for (const QVariant &height : flightPathSegment->amslTerrainHeights()) {
                segment.terrainAMSL.append(height.toDouble());
            }
            segment.terrainSpacing = flightPathSegment->distanceBetween();
            segments.append(segment);
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

QList<GeoZone::Area> GeoZoneMissionCheck::_missionAreas() const
{
    QList<GeoZone::Area> areas;
    const QmlObjectListModel *const visualItems = _missionController ? _missionController->visualItems() : nullptr;
    for (int i = 0; visualItems && (i < visualItems->count()); i++) {
        ComplexMissionItem *const complexItem = visualItems->value<ComplexMissionItem*>(i);
        if (!complexItem) {
            continue;
        }

        QList<QGeoCoordinate> polygon;
        if (TransectStyleComplexItem *const transectItem = qobject_cast<TransectStyleComplexItem*>(complexItem)) {
            polygon = transectItem->surveyAreaPolygon()->coordinateList();
        } else if (StructureScanComplexItem *const structureItem = qobject_cast<StructureScanComplexItem*>(complexItem)) {
            polygon = structureItem->flightPolygon()->coordinateList();
        }
        if (polygon.count() < 3) {
            continue;
        }

        // Terrain along the item's flight path bounds the height above ground, it stays unknown without samples
        double minTerrain = qQNaN();
        double maxTerrain = qQNaN();
        const QmlObjectListModel *const segments = complexItem->flightPathSegments();
        for (int j = 0; j < segments->count(); j++) {
            for (const QVariant &height : segments->value<const FlightPathSegment*>(j)->amslTerrainHeights()) {
                const double terrain = height.toDouble();
                minTerrain = qIsNaN(minTerrain) ? terrain : std::min(minTerrain, terrain);
                maxTerrain = qIsNaN(maxTerrain) ? terrain : std::max(maxTerrain, terrain);
            }
        }

        const QGeoCoordinate center = QGeoRectangle(polygon).center();
        areas.append(GeoZone::Area{ polygon,
                                    GeoZone::Position{ center, complexItem->minAMSLAltitude(), maxTerrain },
                                    GeoZone::Position{ center, complexItem->maxAMSLAltitude(), minTerrain } });
    }
    return areas;
}

QList<QList<GeoZone::Position>> GeoZoneMissionCheck::pathsFromSegments(const QList<Segment> &segments)
{
    QList<QList<GeoZone::Position>> paths;
    for (const Segment &segment : segments) {
        if (!segment.from.isValid() || !segment.to.isValid()) {
            continue;
        }

        const double distance = segment.from.distanceTo(segment.to);
        const double azimuth = segment.from.azimuthTo(segment.to);
        const bool terrainKnown = (segment.terrainAMSL.count() >= 2) && (segment.terrainSpacing > 0);

        // Distance along the segment, fraction of the segment and the terrain elevation there
        struct Sample {
            double along;
            double fraction;
            double terrain;
        };
        QList<Sample> samples;
        if (terrainKnown) {
            const qsizetype lastIndex = segment.terrainAMSL.count() - 1;
            for (qsizetype i = 0; i <= lastIndex; i++) {
                const double along = (i == lastIndex) ? distance : std::min(i * segment.terrainSpacing, distance);
                samples.append({ along, (distance > 0) ? (along / distance) : (static_cast<double>(i) / lastIndex), segment.terrainAMSL[i] });
            }
        } else {
            // Vehicles fly long legs along the great circle, which zone checks only follow with intermediate points
            const int steps = std::max(1, std::min(static_cast<int>(std::ceil(distance / maxStepDistance)), maxStepsPerSegment));
            for (int step = 0; step <= steps; step++) {
                samples.append({ distance * step / steps, static_cast<double>(step) / steps, qQNaN() });
            }
        }

        QList<GeoZone::Position> positions;
        for (const auto &[along, fraction, terrain] : std::as_const(samples)) {
            GeoZone::Position position;
            if (along <= 0) {
                position.coordinate = segment.from;
            } else if (along >= distance) {
                position.coordinate = segment.to;
            } else {
                position.coordinate = segment.from.atDistanceAndAzimuth(along, azimuth);
            }
            position.altitudeAMSL = segment.fromAltitudeAMSL + ((segment.toAltitudeAMSL - segment.fromAltitudeAMSL) * fraction);
            position.terrainAMSL = terrain;
            positions.append(position);
        }

        // Continue the previous path, keeping both ends when the altitude changes at the shared waypoint
        if (!paths.isEmpty() && (paths.last().last().coordinate == segment.from)) {
            GeoZone::Position &last = paths.last().last();
            const bool sameAltitude = (last.altitudeAMSL == positions.first().altitudeAMSL) ||
                                      (qIsNaN(last.altitudeAMSL) && qIsNaN(positions.first().altitudeAMSL));
            if (sameAltitude) {
                if (qIsNaN(last.terrainAMSL)) {
                    last.terrainAMSL = positions.first().terrainAMSL;
                }
                positions.removeFirst();
            }
            paths.last().append(positions);
        } else {
            paths.append(positions);
        }
    }
    return paths;
}
