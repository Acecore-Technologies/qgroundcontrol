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
#include "GeoZoneIndex.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionCheck.h"
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
    (void) connect(_manager, &GeoZoneManager::plannedStartChanged, &_updateTimer, qOverload<>(&QTimer::start));
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
        (void) disconnect(_missionController, nullptr, &_updateTimer, nullptr);
    }
    _missionController = missionController;
    if (_missionController) {
        (void) connect(_missionController, &MissionController::recalcTerrainProfile, &_updateTimer, qOverload<>(&QTimer::start));
        (void) connect(_missionController, &MissionController::visualItemsChanged, &_updateTimer, qOverload<>(&QTimer::start));
        (void) connect(_missionController, &MissionController::missionTimeChanged, &_updateTimer, qOverload<>(&QTimer::start));
    }

    emit missionControllerChanged();
    _updateTimer.start();
}

void GeoZoneProfile::_clear()
{
    _updateTimer.stop();
    if (!_bandList.isEmpty()) {
        _bandList.clear();
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
    const auto [plannedStart, plannedEnd] = GeoZoneMissionCheck::plannedFlightTimes(_manager, _missionController);
    const QList<Band> bands = computeBands(_missionSegments(), _manager, plannedStart, plannedEnd);
    if (bands != _bandList) {
        _bandList = bands;
        _bands = toVariantList(_bandList);
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

QVariantList GeoZoneProfile::toVariantList(const QList<Band> &bands)
{
    QVariantList list;
    list.reserve(bands.count());
    for (const Band &band : bands) {
        QVariantList points;
        points.reserve(band.points.count());
        for (const BandPoint &point : band.points) {
            points.append(QVariantMap{
                { QStringLiteral("distance"), point.distance },
                { QStringLiteral("floor"), point.floor },
                { QStringLiteral("ceiling"), point.ceiling },
            });
        }
        list.append(QVariantMap{
            { QStringLiteral("restriction"), static_cast<int>(band.restriction) },
            { QStringLiteral("name"), band.name },
            { QStringLiteral("points"), points },
        });
    }
    return list;
}

QList<GeoZoneProfile::Band> GeoZoneProfile::computeBands(const QList<Segment> &segments, const GeoZoneManager *manager,
                                                         const QDateTime &activeFrom, const QDateTime &activeTo)
{
    const GeoZonePolicy policy = GeoZonePolicy::profile(activeFrom, activeTo);

    struct Run {
        const GeoZone *zone = nullptr;
        QList<BandPoint> points;
    };
    QHash<std::pair<const GeoZone*, int>, Run> runs;   ///< Stretches being followed by zone volume
    QList<Band> bands;
    const auto closeRun = [&bands](const Run &run) {
        bands.append(Band{ run.zone->restriction(), run.zone->displayName(), run.points });
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
    const double spacing = (std::max)(minSampleSpacing, totalLength / maxSamples);

    // Zone volume near a segment
    struct Candidate {
        const GeoZone *zone = nullptr;
        int volumeIndex = 0;
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
        const double segmentMinLatitude = (std::min)(segment.from.latitude(), segment.to.latitude());
        const double segmentMaxLatitude = (std::max)(segment.from.latitude(), segment.to.latitude());
        // Longitudes are continuous across the antimeridian, east past 180 degrees when crossing it
        const auto continuousSpan = [](double west, double east) { return std::make_pair(west, (east < west) ? (east + 360) : east); };
        const auto segmentSpan = GeoZoneIndex::longitudeSpan(segment.from.longitude(), segment.to.longitude());
        const auto [segmentWest, segmentEast] = continuousSpan(segmentSpan.first, segmentSpan.second);

        QList<Candidate> candidates;
        for (const GeoZone *zone : manager->catalog().zonesInArea(QGeoRectangle(QList<QGeoCoordinate>{ segment.from, segment.to }), policy)) {
            const QList<GeoZone::Volume> &volumes = zone->data().volumes;
            for (int v = 0; v < volumes.count(); v++) {
                const QGeoRectangle box = volumes[v].boundingBox();
                if (!box.isValid()) {
                    continue;
                }
                if ((box.topLeft().latitude() < segmentMinLatitude) || (box.bottomLeft().latitude() > segmentMaxLatitude)) {
                    continue;
                }
                const auto [boxWest, boxEast] = continuousSpan(box.topLeft().longitude(), box.bottomRight().longitude());
                for (const double shift : { -360.0, 0.0, 360.0 }) {
                    if ((boxWest <= (segmentEast + shift)) && ((segmentWest + shift) <= boxEast)) {
                        candidates.append(Candidate{ zone, v });
                        break;
                    }
                }
            }
        }

        const int sampleCount = (std::max)(1, static_cast<int>(std::ceil(length / spacing)));
        for (int i = 0; i <= sampleCount; i++) {
            const double fraction = static_cast<double>(i) / sampleCount;
            const double along = length * fraction;
            GeoZone::Position position;
            position.coordinate = (i == 0) ? segment.from : ((i == sampleCount) ? segment.to : segment.from.atDistanceAndAzimuth(along, azimuth));
            position.terrainAMSL = terrainAt(segment, along, length);
            const double distance = offset + (segment.distance * fraction);

            QSet<std::pair<const GeoZone*, int>> inside;
            for (const Candidate &candidate : std::as_const(candidates)) {
                if (!candidate.zone->volumeContains(candidate.volumeIndex, position.coordinate)) {
                    continue;
                }
                const GeoZone::Volume &volume = candidate.zone->data().volumes[candidate.volumeIndex];

                const std::pair<const GeoZone*, int> key(candidate.zone, candidate.volumeIndex);
                inside.insert(key);
                Run &run = runs[key];
                run.zone = candidate.zone;
                run.points.append(BandPoint{ distance, GeoZone::limitAMSL(volume, false, position), GeoZone::limitAMSL(volume, true, position) });
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
    std::stable_sort(bands.begin(), bands.end(), [](const Band &a, const Band &b) {
        const int severityA = GeoZone::restrictionSeverity(a.restriction);
        const int severityB = GeoZone::restrictionSeverity(b.restriction);
        if (severityA != severityB) {
            return severityA > severityB;
        }
        if (a.points.constFirst().distance != b.points.constFirst().distance) {
            return a.points.constFirst().distance < b.points.constFirst().distance;
        }
        return a.name < b.name;
    });
    return bands;
}
