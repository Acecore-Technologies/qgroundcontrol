/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneProfile.h"
#include "ComplexMissionItem.h"
#include "FlightPathSegment.h"
#include "GeoZoneGeometry.h"
#include "GeoZoneManager.h"
#include "MissionController.h"
#include "QmlObjectListModel.h"
#include "VisualMissionItem.h"

#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtPositioning/QGeoRectangle>

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

/// Ground elevation at a distance along a segment, NaN without terrain samples
double terrainAt(const GeoZoneProfile::Segment &segment, double along, double length)
{
    const qsizetype count = segment.terrainAMSL.count();
    if ((count < 2) || (segment.terrainSpacing <= 0)) {
        return qQNaN();
    }
    const qsizetype last = count - 1;
    const auto sampleDistance = [&segment, last, length](qsizetype index) {
        return (index == last) ? length : std::min(index * segment.terrainSpacing, length);
    };
    const qsizetype index = std::clamp<qsizetype>(static_cast<qsizetype>(along / segment.terrainSpacing), 0, last - 1);
    const double start = sampleDistance(index);
    const double end = sampleDistance(index + 1);
    const double fraction = (end > start) ? std::clamp((along - start) / (end - start), 0.0, 1.0) : 0.0;
    return segment.terrainAMSL[index] + ((segment.terrainAMSL[index + 1] - segment.terrainAMSL[index]) * fraction);
}

} // namespace

GeoZoneProfile::GeoZoneProfile(QObject *parent)
    : GeoZoneProfile(GeoZoneManager::instance(), parent)
{
}

GeoZoneProfile::GeoZoneProfile(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
{
    _updateTimer.setSingleShot(true);
    _updateTimer.setInterval(_updateDelayMSecs);
    (void) connect(&_updateTimer, &QTimer::timeout, this, &GeoZoneProfile::update);

    // The bands reference no zones, but they are outdated
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneProfile::_clear);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, &_updateTimer, qOverload<>(&QTimer::start));
}

MissionController *GeoZoneProfile::missionController() const
{
    return _missionController;
}

void GeoZoneProfile::setMissionController(MissionController *missionController)
{
    if (missionController == _missionController) {
        return;
    }

    if (_missionController) {
        (void) disconnect(_missionController, nullptr, this, nullptr);
    }
    _missionController = missionController;
    if (_missionController) {
        (void) connect(_missionController, &MissionController::recalcTerrainProfile, &_updateTimer, qOverload<>(&QTimer::start));
        (void) connect(_missionController, &MissionController::visualItemsChanged, &_updateTimer, qOverload<>(&QTimer::start));
    }

    emit missionControllerChanged();
    _updateTimer.start();
}

void GeoZoneProfile::_clear()
{
    _updateTimer.stop();
    if (!_bands.isEmpty()) {
        _bands.clear();
        emit bandsChanged();
    }
}

void GeoZoneProfile::setActive(bool active)
{
    if (active == _active) {
        return;
    }
    _active = active;
    emit activeChanged();
    if (_active) {
        _updateTimer.start();
    } else {
        _updateTimer.stop();
    }
}

void GeoZoneProfile::update()
{
    _updateTimer.stop();
    if (!_active) {
        return;
    }
    const QVariantList bands = bandsForSegments(_missionSegments(), _manager);
    if (bands != _bands) {
        _bands = bands;
        emit bandsChanged();
    }
}

QList<GeoZoneProfile::Segment> GeoZoneProfile::_missionSegments() const
{
    QList<Segment> segments;
    const QmlObjectListModel *const visualItems = _missionController ? _missionController->visualItems() : nullptr;
    if (!visualItems) {
        return segments;
    }

    const auto appendSegment = [&segments](const FlightPathSegment *flightPathSegment) {
        Segment segment;
        segment.from = flightPathSegment->coordinate1();
        segment.to = flightPathSegment->coordinate2();
        segment.distance = flightPathSegment->totalDistance();
        for (const QVariant &height : flightPathSegment->amslTerrainHeights()) {
            segment.terrainAMSL.append(height.toDouble());
        }
        segment.terrainSpacing = flightPathSegment->distanceBetween();
        segments.append(segment);
    };

    // Same order and distances as TerrainProfile
    for (int i = 0; i < visualItems->count(); i++) {
        VisualMissionItem *const visualItem = visualItems->value<VisualMissionItem*>(i);
        if (ComplexMissionItem *const complexItem = qobject_cast<ComplexMissionItem*>(visualItem)) {
            const QmlObjectListModel *const complexSegments = complexItem->flightPathSegments();
            if (complexSegments->count() == 0) {
                Segment gap;
                gap.distance = complexItem->complexDistance();
                segments.append(gap);
            }
            for (int j = 0; j < complexSegments->count(); j++) {
                appendSegment(complexSegments->value<const FlightPathSegment*>(j));
            }
        }
        if (visualItem && visualItem->simpleFlightPathSegment()) {
            appendSegment(visualItem->simpleFlightPathSegment());
        }
    }
    return segments;
}

QVariantList GeoZoneProfile::bandsForSegments(const QList<Segment> &segments, const GeoZoneManager *manager)
{
    static const QList<GeoZone::Restriction> restrictions = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::USpace };

    struct Run {
        const GeoZone *zone = nullptr;
        QVariantList points;
    };
    QHash<QPair<const GeoZone*, int>, Run> runs;   ///< Stretches being followed by zone volume
    QVariantList bands;
    const auto closeRun = [&bands](const Run &run) {
        bands.append(QVariantMap{
            { QStringLiteral("restriction"), static_cast<int>(run.zone->restriction()) },
            { QStringLiteral("name"), run.zone->displayName() },
            { QStringLiteral("points"), run.points },
        });
    };
    const auto closeAll = [&runs, &closeRun]() {
        for (const Run &run : std::as_const(runs)) {
            closeRun(run);
        }
        runs.clear();
    };
    const auto validSegment = [](const Segment &segment) {
        return segment.from.isValid() && segment.to.isValid() && (segment.distance > 0);
    };

    // One spacing for the whole path, so that long missions are sampled up to their end
    double totalLength = 0;
    for (const Segment &segment : segments) {
        if (validSegment(segment)) {
            totalLength += segment.from.distanceTo(segment.to);
        }
    }
    const double spacing = std::max(minSampleSpacing, totalLength / maxSamples);

    // Zone volume near a segment with its bounds, and its rings projected around the current stretch of the segment
    struct Candidate {
        const GeoZone *zone = nullptr;
        int volumeIndex = 0;
        double minLatitude = 0;
        double maxLatitude = 0;
        double minLongitude = 0;
        double maxLongitude = 0;
        QList<QList<QPointF>> rings;
    };

    double offset = 0;
    for (const Segment &segment : segments) {
        if (!validSegment(segment)) {
            closeAll();
            offset += segment.distance;
            continue;
        }

        const double length = segment.from.distanceTo(segment.to);
        const double azimuth = segment.from.azimuthTo(segment.to);
        const double segmentMinLatitude = std::min(segment.from.latitude(), segment.to.latitude());
        const double segmentMaxLatitude = std::max(segment.from.latitude(), segment.to.latitude());
        const double segmentMinLongitude = std::min(segment.from.longitude(), segment.to.longitude());
        const double segmentMaxLongitude = std::max(segment.from.longitude(), segment.to.longitude());

        QList<Candidate> candidates;
        for (const GeoZone *zone : manager->zonesInArea(restrictions, QGeoRectangle(QList<QGeoCoordinate>{ segment.from, segment.to }))) {
            const QList<GeoZone::Volume> &volumes = zone->data().volumes;
            for (int v = 0; v < volumes.count(); v++) {
                const QGeoRectangle box = volumes[v].boundingBox();
                if (!box.isValid()) {
                    continue;
                }
                Candidate candidate;
                candidate.zone = zone;
                candidate.volumeIndex = v;
                candidate.minLatitude = box.bottomLeft().latitude();
                candidate.maxLatitude = box.topLeft().latitude();
                candidate.minLongitude = box.topLeft().longitude();
                candidate.maxLongitude = box.bottomRight().longitude();
                if ((candidate.maxLatitude >= segmentMinLatitude) && (candidate.minLatitude <= segmentMaxLatitude) &&
                    (candidate.maxLongitude >= segmentMinLongitude) && (candidate.minLongitude <= segmentMaxLongitude)) {
                    candidates.append(candidate);
                }
            }
        }

        const int sampleCount = std::max(1, static_cast<int>(std::ceil(length / spacing)));
        std::optional<GeoZoneGeometry::LocalProjection> projection;
        double projectionEnd = -1;
        for (int i = 0; i <= sampleCount; i++) {
            const double fraction = static_cast<double>(i) / sampleCount;
            const double along = length * fraction;
            GeoZone::Position position;
            position.coordinate = (i == 0) ? segment.from : ((i == sampleCount) ? segment.to : segment.from.atDistanceAndAzimuth(along, azimuth));
            position.terrainAMSL = terrainAt(segment, along, length);
            const double distance = offset + (segment.distance * fraction);

            // The flat projection is accurate near its center, so it moves along long segments
            if (!projection || (along > projectionEnd)) {
                const double center = std::min(along + (projectionStretch / 2), length);
                projection.emplace(segment.from.atDistanceAndAzimuth(center, azimuth));
                projectionEnd = along + projectionStretch;
                for (Candidate &candidate : candidates) {
                    candidate.rings.clear();
                    const GeoZone::Volume &volume = candidate.zone->data().volumes[candidate.volumeIndex];
                    for (const QList<QGeoCoordinate> &ring : volume.rings) {
                        candidate.rings.append(projection->toLocal(ring));
                    }
                }
            }
            const QPointF point = projection->toLocal(position.coordinate);

            QSet<QPair<const GeoZone*, int>> inside;
            for (const Candidate &candidate : std::as_const(candidates)) {
                const double latitude = position.coordinate.latitude();
                const double longitude = position.coordinate.longitude();
                if ((latitude < candidate.minLatitude) || (latitude > candidate.maxLatitude) ||
                    (longitude < candidate.minLongitude) || (longitude > candidate.maxLongitude)) {
                    continue;
                }
                const GeoZone::Volume &volume = candidate.zone->data().volumes[candidate.volumeIndex];
                bool contained = false;
                if (volume.isCircle) {
                    contained = volume.center.isValid() && (volume.center.distanceTo(position.coordinate) <= volume.radius);
                } else {
                    contained = GeoZoneGeometry::polygonContains(candidate.rings, point);
                }
                if (!contained) {
                    continue;
                }

                const QPair<const GeoZone*, int> key(candidate.zone, candidate.volumeIndex);
                inside.insert(key);
                Run &run = runs[key];
                run.zone = candidate.zone;
                run.points.append(QVariantMap{
                    { QStringLiteral("distance"), distance },
                    { QStringLiteral("floor"), GeoZone::limitAMSL(volume, false, position) },
                    { QStringLiteral("ceiling"), GeoZone::limitAMSL(volume, true, position) },
                });
            }

            for (auto it = runs.begin(); it != runs.end();) {
                if (inside.contains(it.key())) {
                    ++it;
                } else {
                    closeRun(it.value());
                    it = runs.erase(it);
                }
            }
        }
        offset += segment.distance;
    }
    closeAll();

    // Along the path, the most restrictive drawn last, on top
    const auto severity = [](const QVariantMap &band) {
        return GeoZone::restrictionSeverity(static_cast<GeoZone::Restriction>(band.value(QStringLiteral("restriction")).toInt()));
    };
    const auto start = [](const QVariantMap &band) {
        return band.value(QStringLiteral("points")).toList().constFirst().toMap().value(QStringLiteral("distance")).toDouble();
    };
    std::stable_sort(bands.begin(), bands.end(), [&severity, &start](const QVariant &a, const QVariant &b) {
        const QVariantMap bandA = a.toMap();
        const QVariantMap bandB = b.toMap();
        if (severity(bandA) != severity(bandB)) {
            return severity(bandA) > severity(bandB);
        }
        if (start(bandA) != start(bandB)) {
            return start(bandA) < start(bandB);
        }
        return bandA.value(QStringLiteral("name")).toString() < bandB.value(QStringLiteral("name")).toString();
    });
    return bands;
}
