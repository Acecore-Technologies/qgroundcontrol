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
