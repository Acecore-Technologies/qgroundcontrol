/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRoutePlanner.h"
#include "FactMetaData.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMissionReader.h"
#include "GeoZoneMissionWorker.h"
#include "GeoZonePlanning.h"
#include "GeoZoneTerrain.h"
#include "MissionController.h"
#include "QGroundControlQmlGlobal.h"
#include "QmlObjectListModel.h"
#include "SimpleMissionItem.h"

#include <QtCore/QHash>
#include <QtCore/QLocale>
#include <QtCore/QMetaEnum>
#include <QtCore/QMutex>
#include <QtCore/QPromise>
#include <QtCore/QSet>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <tuple>

namespace {

constexpr double kLegTerrainSpacing = 50;   ///< Meters between the positions of a leg whose terrain is looked up for its search
constexpr int kMaxLegTerrainSteps = 2000;
constexpr int kMaxListedZones = 3;

QString _zoneList(const QStringList &names)
{
    if (names.count() <= kMaxListedZones) {
        return names.join(QStringLiteral(", "));
    }
    return GeoZoneRoutePlanner::tr("%1 and %n more", nullptr, static_cast<int>(names.count() - kMaxListedZones))
        .arg(names.first(kMaxListedZones).join(QStringLiteral(", ")));
}

} // namespace

/// Ground elevations for the searches on worker threads, by cells of about 100 m. The searches use the cells known
/// when they start, which the GUI thread looked up in the terrain tiles, and note those they miss for the next search.
class GeoZoneRoutePlanner::Terrain
{
public:
    static constexpr double cellDegrees = 0.001;

    /// @return Elevation of the cell of the coordinate, NaN when unknown
    /// @param missing Set when the cell wasn't looked up
    double elevation(const QGeoCoordinate &coordinate, bool &missing) const
    {
        const quint64 key = _key(coordinate);
        const QMutexLocker locker(&_mutex);
        const auto it = _cells.constFind(key);
        if (it == _cells.constEnd()) {
            _missing.insert(key);
            missing = true;
            return qQNaN();
        }
        return *it;
    }

    /// Sets the elevation of the cell of the coordinate when it isn't known
    void set(const QGeoCoordinate &coordinate, double elevation)
    {
        const quint64 key = _key(coordinate);
        const QMutexLocker locker(&_mutex);
        if (!_cells.contains(key)) {
            _cells.insert(key, elevation);
        }
    }

    /// Looks up the elevation of the cell of the coordinate in the terrain tiles when it isn't known, on the GUI thread
    void lookUp(const QGeoCoordinate &coordinate)
    {
        const quint64 key = _key(coordinate);
        {
            const QMutexLocker locker(&_mutex);
            if (_cells.contains(key)) {
                return;
            }
        }
        const double elevation = GeoZoneTerrain::elevationAt(_center(key));
        const QMutexLocker locker(&_mutex);
        _cells.insert(key, elevation);
    }

    /// Looks up the cells the searches missed in the terrain tiles, on the GUI thread
    /// @return true if the elevation of one of them is known
    bool fill()
    {
        QSet<quint64> missing;
        {
            const QMutexLocker locker(&_mutex);
            missing.swap(_missing);
        }
        bool known = false;
        for (const quint64 key : std::as_const(missing)) {
            const double elevation = GeoZoneTerrain::elevationAt(_center(key));
            known = known || !qIsNaN(elevation);
            const QMutexLocker locker(&_mutex);
            _cells.insert(key, elevation);
        }
        return known;
    }

    /// Forgets the cells whose elevation wasn't known, so that they are looked up again when their tiles are loaded
    void forgetUnknown()
    {
        const QMutexLocker locker(&_mutex);
        (void) _cells.removeIf([](const QHash<quint64, double>::iterator &it) { return qIsNaN(it.value()); });
        _missing.clear();
    }

private:
    static quint64 _key(const QGeoCoordinate &coordinate)
    {
        const auto row = static_cast<qint32>(std::floor(coordinate.latitude() / cellDegrees));
        const auto column = static_cast<qint32>(std::floor(coordinate.longitude() / cellDegrees));
        return (static_cast<quint64>(static_cast<quint32>(row)) << 32) | static_cast<quint32>(column);
    }

    static QGeoCoordinate _center(quint64 key)
    {
        const auto row = static_cast<qint32>(static_cast<quint32>(key >> 32));
        const auto column = static_cast<qint32>(static_cast<quint32>(key & 0xffffffffu));
        return QGeoCoordinate((row + 0.5) * cellDegrees, (column + 0.5) * cellDegrees);
    }

    mutable QMutex _mutex;
    QHash<quint64, double> _cells;
    mutable QSet<quint64> _missing;
};

GeoZoneRoutePlanner::GeoZoneRoutePlanner(QObject *parent)
    : GeoZoneRoutePlanner(GeoZoneManager::instance(), parent)
{
}

GeoZoneRoutePlanner::GeoZoneRoutePlanner(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _worker(new GeoZoneMissionWorker(_updateDelayMSecs, this))
    , _terrain(std::make_shared<Terrain>())
{
    _worker->setActive(false);
    (void) connect(_worker, &GeoZoneMissionWorker::triggered, this, &GeoZoneRoutePlanner::startUpdate);

    // Suggestions refer to zones and mission items, so they are dropped at once when either changes, with the search in
    // progress, and computed again
    (void) connect(_worker, &GeoZoneMissionWorker::missionChanged, this, &GeoZoneRoutePlanner::_invalidate);
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneRoutePlanner::_invalidate);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, _worker, &GeoZoneMissionWorker::schedule);
}

GeoZoneRoutePlanner::~GeoZoneRoutePlanner()
{
    // The computation in progress ends on its own, its results are dropped with the worker
    _worker->stop();
}

bool GeoZoneRoutePlanner::active() const
{
    return _worker->active();
}

void GeoZoneRoutePlanner::setActive(bool active)
{
    if (active == _worker->active()) {
        return;
    }
    _worker->setActive(active);
    emit activeChanged();
    if (active) {
        startUpdate();
    } else {
        _stop();
    }
}

void GeoZoneRoutePlanner::setMaxAltitude(double maxAltitude)
{
    if (qFuzzyCompare(maxAltitude, _maxAltitude) || !(maxAltitude > 0)) {
        return;
    }
    _maxAltitude = maxAltitude;
    emit maxAltitudeChanged();
    _invalidate();
}

QString GeoZoneRoutePlanner::maxAltitudeText() const
{
    return QString::number(qRound(FactMetaData::metersToAppSettingsVerticalDistanceUnits(_maxAltitude).toDouble()));
}

void GeoZoneRoutePlanner::setMaxAltitudeText(const QString &text)
{
    bool ok = false;
    const int value = text.trimmed().toInt(&ok);
    if (!ok || (value < 1) || (text.trimmed() == maxAltitudeText())) {
        return;
    }
    setMaxAltitude(FactMetaData::appSettingsVerticalDistanceUnitsToMeters(value).toDouble());
}

MissionController *GeoZoneRoutePlanner::missionController() const
{
    return _worker->missionController();
}

void GeoZoneRoutePlanner::setMissionController(MissionController *missionController)
{
    if (!_worker->setMissionController(missionController)) {
        return;
    }
    emit missionControllerChanged();
    _invalidate();
}

QVariantList GeoZoneRoutePlanner::suggestions() const
{
    QVariantList suggestions;
    for (const Suggestion &suggestion : _suggestions) {
        QVariantMap map{
            { QStringLiteral("text"), suggestion.text },
            { QStringLiteral("kind"), static_cast<int>(suggestion.kind) },
            { QStringLiteral("legText"), suggestion.legText },
            { QStringLiteral("notes"), suggestion.notes },
            { QStringLiteral("recommended"), suggestion.recommended },
            { QStringLiteral("first"), suggestion.first },
            { QStringLiteral("canApply"), (suggestion.insertIndex >= 0) || !suggestion.altitudes.isEmpty() },
            { QStringLiteral("applyText"), suggestion.altitudes.isEmpty() ? tr("Insert") : tr("Apply") },
        };
        // Only when there is one, NaN would make equal suggestions differ
        if (!qIsNaN(suggestion.suggestedMaxAltitude)) {
            map.insert(QStringLiteral("suggestedMaxAltitude"), suggestion.suggestedMaxAltitude);
        }
        suggestions.append(map);
    }
    return suggestions;
}

GeoZoneRouteSearch::Settings GeoZoneRoutePlanner::settings() const
{
    GeoZoneRouteSearch::Settings settings;
    settings.margin = detourMargin;
    settings.verticalMargin = floorMargin;
    settings.minGroundClearance = minGroundClearance;
    settings.maxAltitudeAGL = _maxAltitude;
    return settings;
}

std::optional<QList<QGeoCoordinate>> GeoZoneRoutePlanner::detourAround(const QGeoCoordinate &from, const QGeoCoordinate &to, const QList<const GeoZone*> &zones,
                                                                       double margin)
{
    GeoZoneRouteSearch::Settings settings;
    settings.margin = margin;
    const GeoZoneRouteSearch::Route route = GeoZoneRouteSearch::detourAround(from, to, zones, settings);
    if (!route.isValid()) {
        return std::nullopt;
    }
    return route.waypoints;
}

GeoZoneRoutePlanner::Job GeoZoneRoutePlanner::_makeJob() const
{
    Job job;
    job.catalog = _manager->catalog();
    MissionController *const missionController = _worker->missionController();
    const auto [start, end] = GeoZoneMissionCheck::plannedFlightTimes(_manager, missionController);
    job.time = start;
    // A copy of the authorisations for the worker thread, since they change on the GUI thread
    const std::shared_ptr<GeoZoneAuthorisations> authorisations(new GeoZoneAuthorisations(QString()), [](GeoZoneAuthorisations *copy) { copy->deleteLater(); });
    authorisations->merge(_manager->planning()->authorisations()->authorisations());
    job.authorisations = authorisations;
    job.policy = GeoZonePolicy::routing(start, end).excludingAuthorised(authorisations.get());
    job.settings = settings();
    job.terrain = _terrain;
    _terrain->forgetUnknown();
    if (!missionController) {
        return job;
    }

    QmlObjectListModel *const visualItems = missionController->visualItems();
    const GeoZoneMissionPath missionPath = GeoZoneMissionReader::path(missionController);
    for (const GeoZoneMissionPath::Leg &missionLeg : missionPath.legs()) {
        const GeoZoneMissionPath::Segment &segment = missionLeg.segment;
        const double length = segment.isValid() ? segment.from.distanceTo(segment.to) : 0;
        // Nor the legs the vehicle doesn't fly, like those after the first return to launch, which the mission check
        // leaves out too
        if ((length < GeoZoneMissionPath::samePlaceTolerance) || !missionLeg.flown) {
            continue;
        }

        LegJob legJob;
        legJob.fromItemIndex = missionLeg.fromItemIndex;
        legJob.toItemIndex = missionLeg.toItemIndex;
        legJob.toSequenceNumber = missionLeg.toSequenceNumber;
        // The items the suggestions of the leg are for, null for indexes beyond the mission
        legJob.fromItem = visualItems ? visualItems->get(missionLeg.fromItemIndex) : nullptr;
        legJob.toItem = visualItems ? visualItems->get(missionLeg.toItemIndex) : nullptr;
        if (const VisualMissionItem *const fromItem = qobject_cast<const VisualMissionItem*>(legJob.fromItem.data())) {
            legJob.fromSequenceNumber = fromItem->sequenceNumber();
        }
        legJob.leg.from = GeoZone::Position{ segment.from, segment.fromAltitudeAMSL, segment.terrainAt(0) };
        legJob.leg.to = GeoZone::Position{ segment.to, segment.toAltitudeAMSL, segment.terrainAt(length) };
        // When the vehicle flies the leg, like for the mission check, which detours take too
        const double legEndDistance = missionPath.itemDistance(missionLeg.toItemIndex);
        const double legStartDistance = legEndDistance - ((segment.distance > 0) ? segment.distance : length);
        legJob.leg.start = GeoZoneMissionPath::timeAt(legStartDistance, missionPath.drawnFlightDistance(), start, end);
        legJob.leg.end = GeoZoneMissionPath::timeAt(legEndDistance, missionPath.drawnFlightDistance(), start, end);

        // The terrain along the leg from its samples or the terrain tiles, which climbs and descents need
        const int steps = std::clamp(static_cast<int>(std::ceil(length / kLegTerrainSpacing)), 1, kMaxLegTerrainSteps);
        for (int i = 0; i <= steps; i++) {
            const GeoZone::Position position = segment.positionAt((length * i) / steps);
            if (qIsNaN(position.terrainAMSL)) {
                _terrain->lookUp(position.coordinate);
            } else {
                _terrain->set(position.coordinate, position.terrainAMSL);
            }
        }
        job.legs.append(legJob);
    }
    return job;
}

QList<GeoZoneRoutePlanner::LegResult> GeoZoneRoutePlanner::_run(const Job &job, const QList<int> &legs, const GeoZoneCancelToken &cancel,
                                                                const std::function<void(int done)> &progress)
{
    GeoZoneRouteSearch search(job.catalog, job.policy, job.time);
    search.setSettings(job.settings);
    search.setCancelToken(cancel);
    bool terrainMissing = false;
    search.setTerrainLookup([terrain = job.terrain, &terrainMissing](const QGeoCoordinate &coordinate) { return terrain->elevation(coordinate, terrainMissing); });
    QList<LegResult> results;
    for (const int leg : legs) {
        if (cancel.isCancelled()) {
            break;
        }
        terrainMissing = false;
        results.append(LegResult{ leg, search.plan(job.legs[leg].leg), terrainMissing });
        if (progress) {
            progress(static_cast<int>(results.count()));
        }
    }
    return results;
}

QList<int> GeoZoneRoutePlanner::_legsToRetry(const Job &job, const QList<LegResult> &results, int round)
{
    if (round >= maxTerrainRounds) {
        return {};
    }
    QList<int> legs;
    for (const LegResult &result : results) {
        if (result.terrainMissing && !result.result.cancelled) {
            legs.append(result.leg);
        }
    }
    if (legs.isEmpty() || !job.terrain->fill()) {
        return {};
    }
    return legs;
}

void GeoZoneRoutePlanner::_merge(QList<LegResult> &results, const QList<LegResult> &update)
{
    for (const LegResult &result : update) {
        const auto it = std::find_if(results.begin(), results.end(), [&result](const LegResult &other) { return other.leg == result.leg; });
        if (it == results.end()) {
            results.append(result);
        } else {
            *it = result;
        }
    }
    std::sort(results.begin(), results.end(), [](const LegResult &a, const LegResult &b) { return a.leg < b.leg; });
}

void GeoZoneRoutePlanner::update()
{
    _stop();
    _setStatusText(QString());
    _worker->runNow([this]() {
        const Job job = _makeJob();
        QList<int> legs(job.legs.count());
        std::iota(legs.begin(), legs.end(), 0);
        QList<LegResult> results;
        for (int round = 0; !legs.isEmpty(); round++) {
            _merge(results, _run(job, legs, _worker->cancelToken(), nullptr));
            legs = _legsToRetry(job, results, round);
        }
        return std::make_pair(job, results);
    }, [this](const std::pair<Job, QList<LegResult>> &computed) { _apply(computed.first, computed.second); });
}

void GeoZoneRoutePlanner::startUpdate()
{
    _worker->stop();
    _setStatusText(QString());
    _clearSuggestions();
    const auto job = std::make_shared<const Job>(_makeJob());
    if (job->legs.isEmpty()) {
        _apply(*job, {});
        return;
    }
    QList<int> legs(job->legs.count());
    std::iota(legs.begin(), legs.end(), 0);
    _setProgress(0);
    _setBusy(true);
    _launch(job, legs, {}, 0);
}

void GeoZoneRoutePlanner::_launch(const std::shared_ptr<const Job> &job, const QList<int> &legs, const QList<LegResult> &results, int round)
{
    // The searches again with more terrain are usually quick, the progress stays at the end of the first ones
    std::function<void(int)> progress;
    if (round == 0) {
        progress = [this, count = legs.count()](int done) { _setProgress(static_cast<double>(done) / count); };
    }
    // Replaced or cancelled searches aren't applied
    _worker->watch(QtConcurrent::run([job, legs, cancel = _worker->cancelToken()](QPromise<QList<LegResult>> &promise) {
        promise.setProgressRange(0, static_cast<int>(legs.count()));
        promise.addResult(_run(*job, legs, cancel, [&promise](int done) { promise.setProgressValue(done); }));
    }), [this, job, results, round](const QList<LegResult> &roundResults) {
        QList<LegResult> merged = results;
        _merge(merged, roundResults);
        const QList<int> retry = _legsToRetry(*job, merged, round);
        if (!retry.isEmpty()) {
            _launch(job, retry, merged, round + 1);
            return;
        }
        _apply(*job, merged);
    }, progress);
}

void GeoZoneRoutePlanner::cancel()
{
    if (!_busy) {
        return;
    }
    _stop();
    _clearSuggestions();
    _setStatusText(tr("The search for routes was cancelled."));
}

void GeoZoneRoutePlanner::_stop()
{
    _worker->stop();
    _setBusy(false);
}

void GeoZoneRoutePlanner::_invalidate()
{
    _worker->invalidate();
    _clearSuggestions();
    _setStatusText(QString());
    // Searched again once the changes settle, while active
    _worker->schedule();
    _setProgress(0);
    _setBusy(_worker->scheduled());
}

VisualMissionItem *GeoZoneRoutePlanner::_itemAt(int index, const QPointer<QObject> &expected) const
{
    MissionController *const missionController = _worker->missionController();
    QmlObjectListModel *const visualItems = missionController ? missionController->visualItems() : nullptr;
    // Range checked, null beyond the items
    QObject *const item = visualItems ? visualItems->get(index) : nullptr;
    return (item && (item == expected.data())) ? qobject_cast<VisualMissionItem*>(item) : nullptr;
}

std::optional<double> GeoZoneRoutePlanner::altitudeInMode(int altitudeMode, double altitudeAMSL, double homeAltitudeAMSL, double terrainAMSL)
{
    // The mode of an item read from a plan file may be none of the modes
    if (!QMetaEnum::fromType<QGroundControlQmlGlobal::AltMode>().valueToKey(altitudeMode)) {
        return std::nullopt;
    }
    double altitude = qQNaN();
    switch (static_cast<QGroundControlQmlGlobal::AltMode>(altitudeMode)) {
    case QGroundControlQmlGlobal::AltitudeModeAbsolute:
        altitude = altitudeAMSL;
        break;
    case QGroundControlQmlGlobal::AltitudeModeRelative:
        altitude = altitudeAMSL - homeAltitudeAMSL;
        break;
    case QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain:
    case QGroundControlQmlGlobal::AltitudeModeTerrainFrame:
        altitude = altitudeAMSL - terrainAMSL;
        break;
    case QGroundControlQmlGlobal::AltitudeModeMixed:
    case QGroundControlQmlGlobal::AltitudeModeNone:
        // Of the mission or of distances, not of an item
        break;
    }
    return qIsNaN(altitude) ? std::nullopt : std::make_optional(altitude);
}

void GeoZoneRoutePlanner::_clearSuggestions()
{
    if (!_suggestions.isEmpty()) {
        _suggestions.clear();
        emit suggestionsChanged();
    }
}

void GeoZoneRoutePlanner::_setBusy(bool busy)
{
    if (busy != _busy) {
        _busy = busy;
        emit busyChanged();
    }
}

void GeoZoneRoutePlanner::_setProgress(double progress)
{
    if (progress != _progress) {
        _progress = progress;
        emit progressChanged();
    }
}

void GeoZoneRoutePlanner::_setStatusText(const QString &statusText)
{
    if (statusText != _statusText) {
        _statusText = statusText;
        emit statusTextChanged();
    }
}

void GeoZoneRoutePlanner::_apply(const Job &job, const QList<LegResult> &results)
{
    _setBusy(false);
    QList<Suggestion> suggestions;
    for (const LegResult &legResult : results) {
        const GeoZoneRouteSearch::Result &result = legResult.result;
        if (result.cancelled) {
            _setStatusText(tr("The search for routes was cancelled."));
            suggestions.clear();
            break;
        }
        if (result.crossedZones.isEmpty()) {
            continue;
        }
        const LegJob &legJob = job.legs[legResult.leg];
        const QString legText = tr("Leg to item %1").arg(legJob.toSequenceNumber);
        QStringList notes;
        double climbAltitude = qQNaN();
        QList<Suggestion> legSuggestions;
        for (const GeoZoneRouteSearch::Route &route : result.routes) {
            if (!route.isValid()) {
                notes.append(_failureText(legJob, route));
                // A higher maximum altitude allows the climb
                if ((route.kind == GeoZoneRouteSearch::Kind::Climb) && (route.failure == GeoZoneRouteSearch::Failure::AboveMaxAltitude) && (route.limit > 0)) {
                    climbAltitude = std::ceil(route.limit);
                }
                continue;
            }
            Suggestion suggestion;
            suggestion.legText = legText;
            suggestion.text = _routeText(legText, legJob, route);
            suggestion.recommended = legSuggestions.isEmpty();
            switch (route.kind) {
            case GeoZoneRouteSearch::Kind::Detour: {
                suggestion.kind = Detour;
                if (!route.waypoints.isEmpty() && _itemAt(legJob.toItemIndex, legJob.toItem)) {
                    suggestion.insertIndex = legJob.toItemIndex;
                    suggestion.insertBefore = legJob.toItem;
                    suggestion.waypoints = route.waypoints;
                    // Like the detour was checked, the altitude changes evenly from the start to the end
                    double along = legJob.leg.from.coordinate.distanceTo(route.waypoints.constFirst());
                    for (qsizetype i = 0; i < route.waypoints.count(); i++) {
                        if (i > 0) {
                            along += route.waypoints[i - 1].distanceTo(route.waypoints[i]);
                        }
                        const double fraction = (route.length > 0) ? (std::min)(along / route.length, 1.0) : 0;
                        suggestion.waypointAltitudes.append(legJob.leg.from.altitudeAMSL + ((legJob.leg.to.altitudeAMSL - legJob.leg.from.altitudeAMSL) * fraction));
                    }
                }
                break;
            }
            case GeoZoneRouteSearch::Kind::Climb:
            case GeoZoneRouteSearch::Kind::Descent:
                suggestion.kind = (route.kind == GeoZoneRouteSearch::Kind::Climb) ? Climb : Descent;
                suggestion.altitudes = _itemAltitudes(legJob, route);
                if (suggestion.altitudes.isEmpty()) {
                    suggestion.text += QStringLiteral(" ") + tr("Change the altitudes of the waypoints in the mission, they can't be changed here.");
                }
                break;
            }
            legSuggestions.append(suggestion);
        }
        if (legSuggestions.isEmpty()) {
            Suggestion suggestion;
            suggestion.legText = legText;
            suggestion.kind = NoRoute;
            suggestion.text = tr("%1: no route avoids %2.").arg(legText, _zoneList(result.crossedZones));
            legSuggestions.append(suggestion);
        }
        legSuggestions.first().first = true;
        legSuggestions.last().notes = notes;
        legSuggestions.last().suggestedMaxAltitude = climbAltitude;
        suggestions.append(legSuggestions);
    }
    _suggestions = suggestions;
    emit suggestionsChanged();
}

QList<GeoZoneRoutePlanner::ItemAltitude> GeoZoneRoutePlanner::_itemAltitudes(const LegJob &legJob, const GeoZoneRouteSearch::Route &route) const
{
    MissionController *const missionController = _worker->missionController();
    if (!missionController) {
        return {};
    }
    const bool climb = (route.kind == GeoZoneRouteSearch::Kind::Climb);
    const double homeAltitude = missionController->plannedHomePosition().altitude();
    QList<ItemAltitude> altitudes;
    for (const auto &[itemIndex, expected, current, target] : { std::make_tuple(legJob.fromItemIndex, legJob.fromItem, legJob.leg.from.altitudeAMSL, route.fromAltitude),
                                                                std::make_tuple(legJob.toItemIndex, legJob.toItem, legJob.leg.to.altitudeAMSL, route.toAltitude) }) {
        if (qIsNaN(target) || (std::abs(target - current) < 0.01)) {
            continue;
        }
        const SimpleMissionItem *const item = qobject_cast<const SimpleMissionItem*>(_itemAt(itemIndex, expected));
        const std::optional<double> altitude = item ? altitudeInMode(item->altitudeMode(), target, homeAltitude, item->terrainAltitude()) : std::nullopt;
        if (!altitude) {
            return {};
        }
        // Whole meters, still clear of the zones
        altitudes.append(ItemAltitude{ itemIndex, expected, item->altitudeMode(), climb ? std::ceil(*altitude - 1e-6) : std::floor(*altitude + 1e-6) });
    }
    return altitudes;
}

QString GeoZoneRoutePlanner::_routeText(const QString &legText, const LegJob &legJob, const GeoZoneRouteSearch::Route &route) const
{
    const QString zones = _zoneList(route.zones);
    switch (route.kind) {
    case GeoZoneRouteSearch::Kind::Detour:
        return tr("%1: detour around %2, %n waypoint(s) adding %3.", nullptr, static_cast<int>(route.waypoints.count()))
            .arg(legText, zones, _distanceText(route.cost));
    case GeoZoneRouteSearch::Kind::Climb:
    case GeoZoneRouteSearch::Kind::Descent: {
        const double fromChange = std::abs(route.fromAltitude - legJob.leg.from.altitudeAMSL);
        const double toChange = std::abs(route.toAltitude - legJob.leg.to.altitudeAMSL);
        const int changed = ((fromChange >= 0.01) ? 1 : 0) + ((toChange >= 0.01) ? 1 : 0);
        const QString altitude = _altitudeText(legJob, route.altitude, route.kind == GeoZoneRouteSearch::Kind::Climb);
        const QString change = _heightText((std::max)(fromChange, toChange));
        if (route.kind == GeoZoneRouteSearch::Kind::Climb) {
            return tr("%1: climb to %2 over %3, raising %n waypoint(s) by up to %4.", nullptr, changed).arg(legText, altitude, zones, change);
        }
        return tr("%1: descend to %2 under %3, lowering %n waypoint(s) by up to %4.", nullptr, changed).arg(legText, altitude, zones, change);
    }
    }
    return QString();
}

QString GeoZoneRoutePlanner::_failureText(const LegJob &legJob, const GeoZoneRouteSearch::Route &route) const
{
    using Failure = GeoZoneRouteSearch::Failure;
    const QString zones = _zoneList(route.zones);
    QString reason;
    switch (route.failure) {
    case Failure::None:
        break;
    case Failure::Cancelled:
        reason = tr("the search was cancelled.");
        break;
    case Failure::StartInside:
        reason = tr("item %1 is inside %2.").arg(legJob.fromSequenceNumber).arg(zones);
        break;
    case Failure::EndInside:
        reason = tr("item %1 is inside %2.").arg(legJob.toSequenceNumber).arg(zones);
        break;
    case Failure::Enclosed:
        reason = tr("%1 surround the leg.").arg(zones);
        break;
    case Failure::TooLong: {
        const double direct = legJob.leg.from.coordinate.distanceTo(legJob.leg.to.coordinate);
        reason = tr("going around %1 adds at least %2, more than the limit of %3.").arg(zones, _distanceText(route.length - direct), _distanceText(route.limit - direct));
        break;
    }
    case Failure::SearchLimit:
        reason = tr("the search for a way around %1 reached its limit.").arg(zones);
        break;
    case Failure::AltitudeUnknown:
        reason = tr("the altitudes of the leg aren't known.");
        break;
    case Failure::NoCeiling:
        reason = tr("%1 has no ceiling in meters.").arg(zones);
        break;
    case Failure::ReachesGround:
        reason = tr("%1 extends to the ground.").arg(zones);
        break;
    case Failure::NoFloor:
        reason = tr("the floor of %1 isn't in meters.").arg(zones);
        break;
    case Failure::AboveMaxAltitude:
        reason = tr("climbing over %1 needs %2 above the terrain, more than the maximum of %3.").arg(zones, _heightText(route.limit), _heightText(_maxAltitude));
        break;
    case Failure::BelowMinClearance:
        reason = tr("flying under %1 leaves %2 above the terrain, less than %3.").arg(zones, _heightText(route.limit), _heightText(minGroundClearance));
        break;
    case Failure::TerrainUnknown:
        reason = tr("the terrain along the leg isn't known.");
        break;
    case Failure::EntersZone:
        reason = tr("it would enter %1.").arg(zones);
        break;
    }
    switch (route.kind) {
    case GeoZoneRouteSearch::Kind::Detour:
        return tr("No detour: %1").arg(reason);
    case GeoZoneRouteSearch::Kind::Climb:
        return tr("No climb: %1").arg(reason);
    case GeoZoneRouteSearch::Kind::Descent:
        return tr("No descent: %1").arg(reason);
    }
    return reason;
}

QString GeoZoneRoutePlanner::_altitudeText(const LegJob &legJob, double altitudeAMSL, bool roundUp) const
{
    // In the altitude mode of the waypoints, rounded like their new altitudes
    const auto rounded = [roundUp](double meters) {
        const double value = FactMetaData::metersToAppSettingsVerticalDistanceUnits(meters).toDouble();
        return QStringLiteral("%1 %2").arg(roundUp ? std::ceil(value - 1e-6) : std::floor(value + 1e-6)).arg(FactMetaData::appSettingsVerticalDistanceUnitsString());
    };
    MissionController *const missionController = _worker->missionController();
    const SimpleMissionItem *const item = qobject_cast<const SimpleMissionItem*>(_itemAt(legJob.toItemIndex, legJob.toItem));
    if (item && (item->altitudeMode() == QGroundControlQmlGlobal::AltitudeModeRelative)) {
        if (const std::optional<double> aboveHome = altitudeInMode(item->altitudeMode(), altitudeAMSL, missionController->plannedHomePosition().altitude(), qQNaN())) {
            return tr("%1 above home").arg(rounded(*aboveHome));
        }
    }
    return tr("%1 AMSL").arg(rounded(altitudeAMSL));
}

QString GeoZoneRoutePlanner::_distanceText(double meters)
{
    const QString units = FactMetaData::appSettingsHorizontalDistanceUnitsString();
    if ((units == QStringLiteral("m")) && (meters >= 1000)) {
        return tr("%1 km").arg(QLocale().toString(meters / 1000, 'f', 1));
    }
    return QStringLiteral("%1 %2").arg(qRound(FactMetaData::metersToAppSettingsHorizontalDistanceUnits(meters).toDouble())).arg(units);
}

QString GeoZoneRoutePlanner::_heightText(double meters)
{
    return QStringLiteral("%1 %2").arg(qRound(FactMetaData::metersToAppSettingsVerticalDistanceUnits(meters).toDouble()))
        .arg(FactMetaData::appSettingsVerticalDistanceUnitsString());
}

bool GeoZoneRoutePlanner::applySuggestion(int index)
{
    MissionController *const missionController = _worker->missionController();
    if (!missionController || (index < 0) || (index >= _suggestions.count())) {
        return false;
    }
    // The suggestions are dropped when the mission changes, like while this one is applied
    const Suggestion suggestion = _suggestions[index];
    const QString missionChanged = tr("The mission changed since the routes were searched. Search them again.");
    if (!suggestion.altitudes.isEmpty()) {
        // Only the items the altitudes were computed for, in the altitude modes they were computed in
        QList<SimpleMissionItem*> items;
        for (const ItemAltitude &itemAltitude : suggestion.altitudes) {
            SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(_itemAt(itemAltitude.itemIndex, itemAltitude.item));
            if (!item || (item->altitudeMode() != itemAltitude.altitudeMode)) {
                emit applyFailed(missionChanged);
                return false;
            }
            items.append(item);
        }
        for (qsizetype i = 0; i < items.count(); i++) {
            items[i]->altitude()->setRawValue(suggestion.altitudes[i].altitude);
        }
    } else if ((suggestion.insertIndex >= 0) && !suggestion.waypoints.isEmpty()) {
        if (!_itemAt(suggestion.insertIndex, suggestion.insertBefore)) {
            emit applyFailed(missionChanged);
            return false;
        }
        if (!_insertDetour(suggestion)) {
            emit applyFailed(tr("The detour wasn't inserted: its waypoints can't be given the altitudes it was checked at, since the "
                                "terrain or the home altitude they are above isn't known. Try again once the terrain is loaded."));
            return false;
        }
    } else {
        return false;
    }
    // Searched again once the changes of the mission settle, which the mission controller recalculates later: a search
    // started now would be of a mission still changing, and dropped for the one which follows
    _invalidate();
    return true;
}

bool GeoZoneRoutePlanner::_insertDetour(const Suggestion &suggestion)
{
    MissionController *const missionController = _worker->missionController();
    const double homeAltitude = missionController->plannedHomePosition().altitude();
    QList<VisualMissionItem*> inserted;
    for (qsizetype i = 0; i < suggestion.waypoints.count(); i++) {
        // The new waypoints take the altitude mode of the mission or of the previous item
        VisualMissionItem *const visualItem = missionController->insertSimpleMissionItem(suggestion.waypoints[i], suggestion.insertIndex + static_cast<int>(i));
        if (visualItem) {
            inserted.append(visualItem);
        }
        // The detour of a leg at unknown altitudes was checked at all altitudes, the waypoints keep the altitude the
        // mission gives them
        const double altitudeAMSL = suggestion.waypointAltitudes.value(i, qQNaN());
        if (visualItem && qIsNaN(altitudeAMSL)) {
            continue;
        }
        SimpleMissionItem *const item = qobject_cast<SimpleMissionItem*>(visualItem);
        std::optional<double> altitude;
        if (item) {
            const bool aboveTerrain = (item->altitudeMode() == QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain) ||
                                      (item->altitudeMode() == QGroundControlQmlGlobal::AltitudeModeTerrainFrame);
            altitude = altitudeInMode(item->altitudeMode(), altitudeAMSL, homeAltitude, aboveTerrain ? GeoZoneTerrain::elevationAt(suggestion.waypoints[i]) : qQNaN());
        }
        if (!altitude) {
            // Not at other altitudes than those the detour was checked at
            for (auto it = inserted.crbegin(); it != inserted.crend(); ++it) {
                missionController->removeVisualItem(missionController->visualItems()->indexOf(*it));
            }
            return false;
        }
        item->altitude()->setRawValue(*altitude);
    }
    return true;
}
