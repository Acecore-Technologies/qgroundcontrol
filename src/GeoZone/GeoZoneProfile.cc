/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneProfile.h"
#include "GeoZoneCatalog.h"
#include "GeoZoneBox.h"
#include "GeoZoneManager.h"
#include "GeoZonePlanning.h"
#include "GeoZoneMissionCheck.h"
#include "MissionController.h"

#include <QtCore/QHash>
#include <QtCore/QSet>

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFutureWatcher>

#include <algorithm>
#include <cmath>
#include <optional>

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
    (void) connect(&_updateTimer, &QTimer::timeout, this, &GeoZoneProfile::_updateConcurrently);

    // The bands reference no zones, but they are outdated
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneProfile::_clear);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, &_updateTimer, qOverload<>(&QTimer::start));
    (void) connect(_manager->planning(), &GeoZonePlanning::plannedStartChanged, &_updateTimer, qOverload<>(&QTimer::start));
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
    _evaluation++;
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
    _evaluation++;
    if (!_active) {
        return;
    }
    const auto [plannedStart, plannedEnd] = GeoZoneMissionCheck::plannedFlightTimes(_manager, _missionController);
    _apply(computeBands(GeoZoneMissionPath::fromMission(_missionController).segments(), _manager->catalog(), plannedStart, plannedEnd));
}

void GeoZoneProfile::_updateConcurrently()
{
    const quint64 evaluation = ++_evaluation;
    if (!_active) {
        return;
    }
    const auto [plannedStart, plannedEnd] = GeoZoneMissionCheck::plannedFlightTimes(_manager, _missionController);
    const GeoZoneCatalog catalog = _manager->catalog();
    const QList<GeoZoneMissionPath::Segment> segments = GeoZoneMissionPath::fromMission(_missionController).segments();
    auto *const watcher = new QFutureWatcher<QList<Band>>(this);
    (void) connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, evaluation]() {
        watcher->deleteLater();
        if (evaluation == _evaluation) {
            _apply(watcher->result());
        }
    });
    watcher->setFuture(QtConcurrent::run([segments, catalog, start = plannedStart, end = plannedEnd]() { return computeBands(segments, catalog, start, end); }));
}

void GeoZoneProfile::_apply(const QList<Band> &bands)
{
    if (bands != _bandList) {
        _bandList = bands;
        _bands = toVariantList(_bandList);
        emit bandsChanged();
    }
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

QList<GeoZoneProfile::Band> GeoZoneProfile::computeBands(const QList<GeoZoneMissionPath::Segment> &segments, const GeoZoneCatalog &catalog,
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
    const auto validSegment = [](const GeoZoneMissionPath::Segment &segment) {
        return segment.isValid() && (segment.distance > 0);
    };

    // One spacing for the whole path, so that long missions are sampled up to their end
    double totalLength = 0;
    for (const GeoZoneMissionPath::Segment &segment : segments) {
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
    for (const GeoZoneMissionPath::Segment &segment : segments) {
        if (!validSegment(segment)) {
            closeAll();
            offset += segment.distance;
            continue;
        }

        const double length = segment.from.distanceTo(segment.to);
        // The segment takes the shorter way, which may cross the antimeridian
        const GeoZoneBox segmentBox = GeoZoneBox::fromPath({ segment.from, segment.to });

        QList<Candidate> candidates;
        for (const GeoZone *zone : catalog.zonesInArea(segmentBox, policy)) {
            const QList<GeoZone::Volume> &volumes = zone->data().volumes;
            for (int v = 0; v < volumes.count(); v++) {
                if (volumes[v].boundingBox().intersects(segmentBox)) {
                    candidates.append(Candidate{ zone, v });
                }
            }
        }

        const int sampleCount = (std::max)(1, static_cast<int>(std::ceil(length / spacing)));
        for (int i = 0; i <= sampleCount; i++) {
            const double fraction = static_cast<double>(i) / sampleCount;
            const double along = length * fraction;
            const GeoZone::Position position = segment.positionAt(along);
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
