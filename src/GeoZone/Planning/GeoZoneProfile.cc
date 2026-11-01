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
#include "GeoZoneManager.h"
#include "GeoZonePlanning.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMissionWorker.h"
#include "GeoZonePathEvaluator.h"
#include "MissionController.h"

#include <algorithm>

GeoZoneProfile::GeoZoneProfile(QObject *parent)
    : GeoZoneProfile(GeoZoneManager::instance(), parent)
{
}

GeoZoneProfile::GeoZoneProfile(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _worker(new GeoZoneMissionWorker(_updateDelayMSecs, this))
{
    (void) connect(_worker, &GeoZoneMissionWorker::triggered, this, &GeoZoneProfile::_updateConcurrently);

    // The bands reference no zones, but they are outdated
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneProfile::_clear);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, _worker, &GeoZoneMissionWorker::schedule);
    (void) connect(_manager->planning(), &GeoZonePlanning::plannedStartChanged, _worker, &GeoZoneMissionWorker::schedule);
}

MissionController *GeoZoneProfile::missionController() const
{
    return _worker->missionController();
}

void GeoZoneProfile::setMissionController(MissionController *missionController)
{
    if (!_worker->setMissionController(missionController)) {
        return;
    }
    // The bands of the previous mission don't apply to this one
    _apply({});
    emit missionControllerChanged();
    _worker->schedule();
}

void GeoZoneProfile::_clear()
{
    _worker->stop();
    if (!_bandList.isEmpty()) {
        _bandList.clear();
        _bands.clear();
        emit bandsChanged();
    }
}

bool GeoZoneProfile::active() const
{
    return _worker->active();
}

void GeoZoneProfile::setActive(bool active)
{
    if (active == _worker->active()) {
        return;
    }
    _worker->setActive(active);
    emit activeChanged();
    _worker->schedule();
}

void GeoZoneProfile::update()
{
    if (!_worker->active()) {
        _worker->stop();
        return;
    }
    _worker->runNow([this]() {
        const auto [plannedStart, plannedEnd] = GeoZoneMissionCheck::plannedFlightTimes(_manager, _worker->missionController());
        return computeBands(GeoZoneMissionPath::fromMission(_worker->missionController()).segments(), _manager->catalog(), plannedStart, plannedEnd);
    }, [this](const QList<Band> &bands) { _apply(bands); });
}

void GeoZoneProfile::_updateConcurrently()
{
    if (!_worker->active()) {
        _worker->stop();
        return;
    }
    const auto [plannedStart, plannedEnd] = GeoZoneMissionCheck::plannedFlightTimes(_manager, _worker->missionController());
    const GeoZoneCatalog catalog = _manager->catalog();
    const QList<GeoZoneMissionPath::Segment> segments = GeoZoneMissionPath::fromMission(_worker->missionController()).segments();
    _worker->run([segments, catalog, start = plannedStart, end = plannedEnd]() { return computeBands(segments, catalog, start, end); },
                 [this](const QList<Band> &bands) { _apply(bands); });
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
    const GeoZoneMissionPath missionPath = GeoZoneMissionPath::fromSegments(segments);
    const QList<GeoZoneMissionPath::Path> &paths = missionPath.paths();
    const GeoZonePathEvaluator evaluator(catalog, GeoZonePolicy::profile(activeFrom, activeTo), activeFrom);
    const GeoZonePathEvaluator::Result result = evaluator.evaluate(missionPath.evaluatorPaths(activeFrom, activeTo));

    // One spacing for the whole path, so that long missions have a bounded number of points up to their end
    const double spacing = (std::max)(minSampleSpacing, missionPath.totalDistance() / maxSamples);

    QList<Band> bands;
    for (const GeoZonePathEvaluator::ZoneResult &zoneResult : result.zones) {
        for (const GeoZonePathEvaluator::Stretch &stretch : zoneResult.stretches) {
            const QList<double> &distances = paths[stretch.entry.path].distances;
            const QList<GeoZone::Position> &positions = paths[stretch.entry.path].positions;
            const auto missionDistance = [&distances](const GeoZonePathEvaluator::PathPoint &point) {
                const double from = distances[point.segment];
                const double to = distances[(std::min)(point.segment + 1, distances.count() - 1)];
                return (point.fraction <= 0) ? from : ((point.fraction >= 1) ? to : (from + ((to - from) * point.fraction)));
            };
            const double entryDistance = missionDistance(stretch.entry);
            const double exitDistance = missionDistance(stretch.exit);
            if (!stretch.active || !(exitDistance > entryDistance)) {
                continue;
            }

            const GeoZone::Volume &volume = zoneResult.zone->data().volumes[stretch.volume];
            const auto bandPoint = [&volume](double distance, const GeoZone::Position &position) {
                return BandPoint{ distance, GeoZone::limitAMSL(volume, false, position), GeoZone::limitAMSL(volume, true, position) };
            };
            Band band{ zoneResult.zone->restriction(), zoneResult.zone->displayName(), { bandPoint(entryDistance, stretch.entry.position) } };
            // Limits above ground follow the terrain, which is linear between the positions
            for (qsizetype i = stretch.entry.segment + 1; i <= stretch.exit.segment; i++) {
                if (((distances[i] - band.points.constLast().distance) >= spacing) && (distances[i] < exitDistance)) {
                    band.points.append(bandPoint(distances[i], positions[i]));
                }
            }
            band.points.append(bandPoint(exitDistance, stretch.exit.position));
            bands.append(band);
        }
    }

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
