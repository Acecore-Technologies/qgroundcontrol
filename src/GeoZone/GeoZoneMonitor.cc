/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMonitor.h"
#include "AudioOutput.h"
#include "GeoZone.h"
#include "GeoZoneAlertLog.h"
#include "GeoZoneManager.h"
#include "GeoZoneSource.h"
#include "GeoZoneSettings.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "QGCLoggingCategory.h"
#include "SettingsManager.h"
#include "Vehicle.h"

#include <QtCore/qapplicationstatic.h>
#include <QtCore/QSet>
#include <QtCore/QtMath>
#include <QtPositioning/QGeoCircle>

#include <algorithm>
#include <cmath>
#include <limits>

QGC_LOGGING_CATEGORY(GeoZoneMonitorLog, "qgc.geozone.geozonemonitor")

GeoZoneNearbyModel::GeoZoneNearbyModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void GeoZoneNearbyModel::setItems(const QList<Item> &items)
{
    const bool sameZones = (items.count() == _items.count()) &&
                           std::equal(items.cbegin(), items.cend(), _items.cbegin(), [](const Item &a, const Item &b) { return a.zone == b.zone; });
    if (!sameZones) {
        const bool countChanged = (items.count() != _items.count());
        beginResetModel();
        _items = items;
        endResetModel();
        if (countChanged) {
            emit this->countChanged();
        }
        return;
    }

    for (qsizetype row = 0; row < items.count(); row++) {
        if (items[row] != _items[row]) {
            _items[row] = items[row];
            const QModelIndex changed = index(static_cast<int>(row));
            emit dataChanged(changed, changed, { InsideRole, DistanceRole, TimeToEntryRole });
        }
    }
}

int GeoZoneNearbyModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant GeoZoneNearbyModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return QVariant();
    }
    const Item &item = _items[index.row()];
    switch (role) {
    case ZoneRole:
        return QVariant::fromValue(static_cast<QObject*>(item.zone));
    case InsideRole:
        return item.inside;
    case DistanceRole:
        return item.distance;
    case TimeToEntryRole:
        return item.timeToEntry;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GeoZoneNearbyModel::roleNames() const
{
    return { { ZoneRole, "zone" }, { InsideRole, "inside" }, { DistanceRole, "distance" }, { TimeToEntryRole, "timeToEntry" } };
}

Q_APPLICATION_STATIC(GeoZoneMonitor, _geoZoneMonitor);

GeoZoneMonitor::GeoZoneMonitor(QObject *parent)
    : GeoZoneMonitor(GeoZoneManager::instance(), parent)
{
    _alertLog = new GeoZoneAlertLog(this);
    (void) connect(this, &GeoZoneMonitor::alertEvent, _alertLog, &GeoZoneAlertLog::append);

    // Evaluated periodically, since a hovering or climbing vehicle doesn't report a new coordinate
    _evaluateTimer.setInterval(evaluateIntervalMSecs);
    (void) connect(&_evaluateTimer, &QTimer::timeout, this, &GeoZoneMonitor::_evaluateVehicles);

    (void) connect(this, &GeoZoneMonitor::alert, this, &GeoZoneMonitor::_speak);
    (void) connect(this, &GeoZoneMonitor::monitoringChanged, this, &GeoZoneMonitor::_updateEvaluateTimer);
    (void) connect(SettingsManager::instance()->geoZoneSettings()->alertsEnabled(), &Fact::rawValueChanged, this, &GeoZoneMonitor::_alertsEnabledChanged);

    MultiVehicleManager *const multiVehicleManager = MultiVehicleManager::instance();
    (void) connect(multiVehicleManager, &MultiVehicleManager::vehicleAdded, this, &GeoZoneMonitor::_vehicleAdded);
    (void) connect(multiVehicleManager, &MultiVehicleManager::vehicleRemoved, this, &GeoZoneMonitor::_vehicleRemoved);
    (void) connect(multiVehicleManager, &MultiVehicleManager::activeVehicleChanged, this, &GeoZoneMonitor::_activeVehicleChanged);
    for (int i = 0; i < multiVehicleManager->vehicles()->count(); i++) {
        _vehicleAdded(multiVehicleManager->vehicles()->value<Vehicle*>(i));
    }
    _activeVehicleChanged(multiVehicleManager->activeVehicle());
}

GeoZoneMonitor::GeoZoneMonitor(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _nearbyModel(new GeoZoneNearbyModel(this))
{
    _clock.start();
    // Zones of a source are about to be deleted. Their alert states are kept so reloading the source doesn't repeat alerts.
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMonitor::_forgetZones);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMonitor::monitoringChanged);
}

GeoZoneMonitor *GeoZoneMonitor::instance()
{
    return _geoZoneMonitor();
}

void GeoZoneMonitor::_vehicleAdded(Vehicle *vehicle)
{
    if (!vehicle || _trackers.contains(vehicle->id())) {
        return;
    }
    _vehicles.append(vehicle);
    _trackers.insert(vehicle->id(), Tracker());
    emit monitoringChanged();
}

void GeoZoneMonitor::_vehicleRemoved(Vehicle *vehicle)
{
    _vehicles.removeIf([vehicle](const QPointer<Vehicle> &monitored) { return !monitored || (monitored == vehicle); });
    if (vehicle) {
        removeVehicle(vehicle->id());
    }
    emit monitoringChanged();
}

void GeoZoneMonitor::_activeVehicleChanged(Vehicle *vehicle)
{
    setActiveVehicleId(vehicle ? vehicle->id() : noVehicle);
}

void GeoZoneMonitor::setActiveVehicleId(int vehicleId)
{
    if (vehicleId != _activeVehicleId) {
        _activeVehicleId = vehicleId;
        _publishNearby();
    }
}

void GeoZoneMonitor::removeVehicle(int vehicleId)
{
    const auto it = _trackers.find(vehicleId);
    if (it == _trackers.end()) {
        return;
    }
    const bool hadNearby = !it->nearby.isEmpty();
    _trackers.erase(it);
    if (hadNearby && (vehicleId == _activeVehicleId)) {
        _publishNearby();
    }
}

void GeoZoneMonitor::_updateEvaluateTimer()
{
    if (monitoring()) {
        if (!_evaluateTimer.isActive()) {
            _evaluateTimer.start();
            _evaluateVehicles();
        }
    } else {
        _evaluateTimer.stop();
    }
}

void GeoZoneMonitor::_evaluateVehicles()
{
    const qint64 msecs = _clock.elapsed();
    for (const QPointer<Vehicle> &vehicle : std::as_const(_vehicles)) {
        if (vehicle && vehicle->coordinate().isValid()) {
            const GeoZone::Position position = vehiclePosition(vehicle, vehicle->coordinate());
            const Velocity velocity = _estimateVelocity(_trackers[vehicle->id()].samples, msecs, position);
            evaluate(position, vehicle->id(), velocity);
        }
    }
}

void GeoZoneMonitor::_alertsEnabledChanged()
{
    _resetTrackers();
    emit monitoringChanged();
}

bool GeoZoneMonitor::monitoring() const
{
    return !_vehicles.isEmpty() && (_manager->zones()->count() > 0) && SettingsManager::instance()->geoZoneSettings()->alertsEnabled()->rawValue().toBool();
}

void GeoZoneMonitor::_publishNearby()
{
    _nearbyModel->setItems(_activeNearby());
    emit nearbyZonesChanged();
    emit verticalBandsChanged();
}

const QList<GeoZoneMonitor::Nearby> &GeoZoneMonitor::_activeNearby() const
{
    static const QList<Nearby> none;
    const auto it = _trackers.constFind(_activeVehicleId);
    return (it != _trackers.constEnd()) ? it->nearby : none;
}

QVariantList GeoZoneMonitor::nearbyZones() const
{
    QVariantList zones;
    for (const Nearby &nearby : _activeNearby()) {
        zones.append(QVariantMap{
            { QStringLiteral("zone"), QVariant::fromValue(static_cast<QObject*>(nearby.zone)) },
            { QStringLiteral("inside"), nearby.inside },
            { QStringLiteral("distance"), nearby.distance },
            { QStringLiteral("timeToEntry"), nearby.timeToEntry },
        });
    }
    return zones;
}

int GeoZoneMonitor::alertRestriction() const
{
    const QList<Nearby> &nearby = _activeNearby();
    return nearby.isEmpty() ? -1 : static_cast<int>(nearby.first().zone->restriction());
}

bool GeoZoneMonitor::inside() const
{
    const QList<Nearby> &nearby = _activeNearby();
    return std::any_of(nearby.cbegin(), nearby.cend(), [](const Nearby &zone) { return zone.inside; });
}

GeoZone::Position GeoZoneMonitor::vehiclePosition(Vehicle *vehicle, const QGeoCoordinate &coordinate)
{
    GeoZone::Position position{ coordinate };
    if (!vehicle) {
        return position;
    }

    const double altitudeAMSL = vehicle->altitudeAMSL()->rawValue().toDouble();
    if (qIsNaN(altitudeAMSL)) {
        return position;
    }
    position.altitudeAMSL = altitudeAMSL;

    // Without terrain data limits above ground can't be excluded. The height above home is not used instead
    // since it misses zones over rising ground.
    const double heightAboveTerrain = vehicle->altitudeAboveTerr()->rawValue().toDouble();
    if (!qIsNaN(heightAboveTerrain)) {
        position.terrainAMSL = altitudeAMSL - heightAboveTerrain;
    }
    return position;
}

const GeoZoneMonitor::Tracker *GeoZoneMonitor::_activeTracker() const
{
    const auto it = _trackers.constFind(_activeVehicleId);
    return (it != _trackers.constEnd()) ? &it.value() : nullptr;
}

QVariantList GeoZoneMonitor::verticalBands() const
{
    QVariantList bands;
    const Tracker *const tracker = _activeTracker();
    if (!tracker) {
        return bands;
    }
    for (const VerticalBand &band : tracker->bands) {
        bands.append(QVariantMap{
            { QStringLiteral("restriction"), static_cast<int>(band.zone->restriction()) },
            { QStringLiteral("name"), band.zone->displayName() },
            { QStringLiteral("limitsText"), band.zone->verticalLimitsText() },
            { QStringLiteral("floor"), band.floor },
            { QStringLiteral("ceiling"), band.ceiling },
        });
    }
    return bands;
}

double GeoZoneMonitor::altitudeAMSL() const
{
    const Tracker *const tracker = _activeTracker();
    return tracker ? tracker->position.altitudeAMSL : qQNaN();
}

double GeoZoneMonitor::terrainAMSL() const
{
    const Tracker *const tracker = _activeTracker();
    return tracker ? tracker->position.terrainAMSL : qQNaN();
}

QList<GeoZoneMonitor::VerticalBand> GeoZoneMonitor::_verticalBands(const GeoZone::Position &position) const
{
    static const QList<GeoZone::Restriction> restrictions = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::USpace };
    QList<VerticalBand> bands;
    for (QObject *object : _manager->zonesAt(position.coordinate)) {
        GeoZone *const zone = qobject_cast<GeoZone*>(object);
        if (!zone->active() || !restrictions.contains(zone->restriction())) {
            continue;
        }
        for (const GeoZone::Volume &volume : zone->data().volumes) {
            if (GeoZone::volumeContains(volume, position.coordinate)) {
                bands.append(VerticalBand{ zone, GeoZone::limitAMSL(volume, false, position), GeoZone::limitAMSL(volume, true, position) });
            }
        }
    }
    return bands;
}

QString GeoZoneMonitor::pathWarning(const QList<GeoZone::Position> &path) const
{
    static const QList<GeoZone::Restriction> restrictions = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::USpace };
    QList<GeoZone*> zones = _manager->zonesCrossingPaths(restrictions, { path });
    // Zones the vehicle is already in were alerted when it entered them
    zones.removeIf([&path](const GeoZone *zone) { return !zone->active() || zone->contains(path.constFirst()); });
    if (zones.isEmpty()) {
        return QString();
    }
    std::stable_sort(zones.begin(), zones.end(), [](const GeoZone *a, const GeoZone *b) {
        return GeoZone::restrictionSeverity(a->restriction()) < GeoZone::restrictionSeverity(b->restriction());
    });

    constexpr qsizetype maxListed = 4;
    QStringList names;
    for (qsizetype i = 0; i < (std::min)(zones.count(), maxListed); i++) {
        names.append(QStringLiteral("%1 (%2)").arg(zones[i]->displayName(), zones[i]->restrictionText()));
    }
    QString text = names.join(QStringLiteral(", "));
    if (zones.count() > maxListed) {
        text += tr(" and %1 more").arg(zones.count() - maxListed);
    }
    return tr("The vehicle would enter geozones: %1").arg(text);
}

QString GeoZoneMonitor::gotoWarning(QObject *vehicle, const QGeoCoordinate &coordinate, double altitudeAMSL) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid() || !coordinate.isValid()) {
        return QString();
    }
    const GeoZone::Position from = vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    GeoZone::Position to = from;
    to.coordinate = coordinate;
    if (!std::isnan(altitudeAMSL)) {
        to.altitudeAMSL = altitudeAMSL;
    }
    return pathWarning({ from, to });
}

QString GeoZoneMonitor::orbitWarning(QObject *vehicle, const QGeoCoordinate &center, double radius, double altitudeAMSL) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid() || !center.isValid() || !(radius > 0)) {
        return QString();
    }
    const GeoZone::Position from = vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    QList<GeoZone::Position> path = { from };
    constexpr int circlePoints = 36;
    const double startAzimuth = center.azimuthTo(from.coordinate);
    for (int i = 0; i <= circlePoints; i++) {
        GeoZone::Position position = from;
        position.coordinate = center.atDistanceAndAzimuth(radius, startAzimuth + (360.0 * i / circlePoints));
        if (!std::isnan(altitudeAMSL)) {
            position.altitudeAMSL = altitudeAMSL;
        }
        path.append(position);
    }
    return pathWarning(path);
}

QString GeoZoneMonitor::altitudeWarning(QObject *vehicle, double relativeAltitude) const
{
    Vehicle *const guidedVehicle = qobject_cast<Vehicle*>(vehicle);
    if (!guidedVehicle || !guidedVehicle->coordinate().isValid() || std::isnan(relativeAltitude)) {
        return QString();
    }
    const GeoZone::Position from = vehiclePosition(guidedVehicle, guidedVehicle->coordinate());
    const double currentRelative = guidedVehicle->altitudeRelative()->rawValue().toDouble();
    if (std::isnan(from.altitudeAMSL) || std::isnan(currentRelative)) {
        // Unknown altitudes are inside all limits, so only zones applying at any altitude would be named
        return pathWarning({ from, from });
    }
    GeoZone::Position to = from;
    to.altitudeAMSL = from.altitudeAMSL + (relativeAltitude - currentRelative);
    return pathWarning({ from, to });
}

void GeoZoneMonitor::_resetTrackers()
{
    const bool hadNearby = !_activeNearby().isEmpty();
    for (Tracker &tracker : _trackers) {
        tracker = Tracker();
    }
    if (hadNearby) {
        _publishNearby();
    }
}

void GeoZoneMonitor::_forgetZones(GeoZoneSource *source)
{
    bool activeChanged = false;
    for (auto it = _trackers.begin(); it != _trackers.end(); ++it) {
        const qsizetype removedBands = it->bands.removeIf([source](const VerticalBand &band) { return band.zone->parent() == source; });
        if ((it->nearby.removeIf([source](const Nearby &nearby) { return nearby.zone->parent() == source; }) + removedBands) > 0) {
            activeChanged = activeChanged || (it.key() == _activeVehicleId);
        }
    }
    if (activeChanged) {
        _publishNearby();
    }
}

QString GeoZoneMonitor::_vehicleLabel(int vehicleId) const
{
    if (vehicleId == noVehicle) {
        return QString();
    }
    const qsizetype vehicleCount = std::count_if(_trackers.keyBegin(), _trackers.keyEnd(), [](int id) { return id != noVehicle; });
    return (vehicleCount > 1) ? tr("Vehicle %1").arg(vehicleId) : QString();
}

GeoZone::Position GeoZoneMonitor::_predictedPosition(const GeoZone::Position &position, const Velocity &velocity, double seconds)
{
    GeoZone::Position predicted = position;
    const double course = qRadiansToDegrees(std::atan2(velocity.east, velocity.north));
    predicted.coordinate = position.coordinate.atDistanceAndAzimuth(velocity.groundSpeed() * seconds, course);
    if (!std::isnan(position.altitudeAMSL)) {
        predicted.altitudeAMSL = position.altitudeAMSL + (velocity.up * seconds);
    }
    // The terrain ahead is unknown, limits above ground use the terrain below the vehicle
    return predicted;
}

double GeoZoneMonitor::_timeToEntry(const GeoZone *zone, const GeoZone::Position &position, const Velocity &velocity, double horizon)
{
    if (!zone->intersectsPath(QList<GeoZone::Position>{ position, _predictedPosition(position, velocity, horizon) })) {
        return qQNaN();
    }
    // The path up to a time enters the zone from the entry on, so the entry is found by bisection
    double before = 0;
    double after = horizon;
    while ((after - before) > 0.25) {
        const double middle = (before + after) / 2;
        if (zone->intersectsPath(QList<GeoZone::Position>{ position, _predictedPosition(position, velocity, middle) })) {
            after = middle;
        } else {
            before = middle;
        }
    }
    return after;
}

GeoZoneMonitor::Velocity GeoZoneMonitor::_estimateVelocity(QList<Sample> &samples, qint64 msecs, const GeoZone::Position &position)
{
    samples.removeIf([msecs](const Sample &sample) { return (msecs - sample.msecs) > velocityWindowMSecs; });
    samples.append(Sample{ msecs, position });

    Velocity velocity;
    const Sample &oldest = samples.constFirst();
    const double seconds = static_cast<double>(msecs - oldest.msecs) / 1000.0;
    if ((seconds < 0.5) || !oldest.position.coordinate.isValid()) {
        return velocity;
    }
    const double distance = oldest.position.coordinate.distanceTo(position.coordinate);
    const double azimuth = qDegreesToRadians(oldest.position.coordinate.azimuthTo(position.coordinate));
    velocity.north = distance * std::cos(azimuth) / seconds;
    velocity.east = distance * std::sin(azimuth) / seconds;
    if (!std::isnan(oldest.position.altitudeAMSL) && !std::isnan(position.altitudeAMSL)) {
        velocity.up = (position.altitudeAMSL - oldest.position.altitudeAMSL) / seconds;
    }
    velocity.valid = true;
    return velocity;
}

void GeoZoneMonitor::evaluate(const GeoZone::Position &position, int vehicleId, const Velocity &velocity)
{
    if (!position.coordinate.isValid()) {
        return;
    }

    // U-space airspace requires a U-space flight authorisation, no restriction zones are only shown on the map
    static const QList<GeoZone::Restriction> restrictions = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::USpace };
    GeoZoneSettings *const settings = SettingsManager::instance()->geoZoneSettings();
    const double approachDistance = settings->approachDistance()->rawValue().toDouble();
    const double approachVerticalDistance = settings->approachVerticalDistance()->rawValue().toDouble();
    const double approachTime = settings->approachTime()->rawValue().toDouble();
    QGeoRectangle area = QGeoCircle(position.coordinate, approachDistance + hysteresisDistance).boundingGeoRectangle();

    // Zones the vehicle would enter at its velocity within the approach time are near too
    const bool predict = velocity.valid && (approachTime > 0) &&
                         ((velocity.groundSpeed() >= minPredictionSpeed) || (std::abs(velocity.up) >= minPredictionSpeed));
    const double horizon = approachTime + hysteresisTime;
    if (predict) {
        const GeoZone::Position ahead = _predictedPosition(position, velocity, horizon);
        area = area.united(QGeoRectangle(QList<QGeoCoordinate>{ position.coordinate, ahead.coordinate }));
    }

    Tracker &tracker = _trackers[vehicleId];
    const QString vehicleLabel = _vehicleLabel(vehicleId);

    QSet<QString> nearbyZones;
    QList<Nearby> nearby;
    QList<AlertEvent> alerts;
    for (GeoZone *zone : _manager->zonesInArea(restrictions, area)) {
        if (!zone->active()) {
            continue;
        }

        const QString &key = zone->key();
        const State previous = tracker.states.value(key, State::Away);
        const double nearLimit = approachDistance + ((previous != State::Away) ? hysteresisDistance : 0);
        // The approach margin includes the inside margin, so a zone which isn't near isn't entered either
        const double nearDistance = zone->distanceTo(position, approachVerticalDistance + ((previous != State::Away) ? hysteresisDistance : 0), nearLimit);
        const bool nearByDistance = (nearDistance <= nearLimit);
        double insideDistance = qInf();
        if (nearByDistance) {
            insideDistance = zone->distanceTo(position, (previous == State::Inside) ? hysteresisDistance : 0, hysteresisDistance);
        }
        const bool inside = (insideDistance <= 0) || ((previous == State::Inside) && (insideDistance <= hysteresisDistance));

        double timeToEntry = qQNaN();
        if (!inside && predict) {
            timeToEntry = _timeToEntry(zone, position, velocity, horizon);
        }
        const double timeLimit = approachTime + ((previous != State::Away) ? hysteresisTime : 0);
        const bool nearByTime = !std::isnan(timeToEntry) && (timeToEntry <= timeLimit);
        if (!inside && !nearByDistance && !nearByTime) {
            continue;
        }

        const State state = inside ? State::Inside : State::Near;
        // Zones only near in time can be far, or above or below the vehicle
        const double distance = nearByDistance ? (std::min)(insideDistance, nearDistance) : zone->distanceTo(position, std::numeric_limits<double>::max());

        nearbyZones.insert(key);
        tracker.states[key] = state;
        nearby.append(Nearby{ zone, inside, static_cast<int>(std::lround(distance)), std::isnan(timeToEntry) ? -1 : static_cast<int>(std::lround(timeToEntry)) });
        if (state <= previous) {
            continue;
        }

        const QString name = zone->displayName();
        QString message;
        if (state == State::Inside) {
            message = tr("Entered geozone %1 (%2)").arg(name, zone->restrictionText());
        } else if (!std::isnan(timeToEntry)) {
            message = tr("Approaching geozone %1 (%2), entry in about %3 seconds").arg(name, zone->restrictionText()).arg(std::lround(timeToEntry));
        } else {
            message = tr("Approaching geozone %1 (%2)").arg(name, zone->restrictionText());
        }
        if (!vehicleLabel.isEmpty()) {
            message = QStringLiteral("%1: %2").arg(vehicleLabel, message);
        }
        qCDebug(GeoZoneMonitorLog) << message << "distance" << distance << "time to entry" << timeToEntry;

        AlertEvent event;
        event.time = QDateTime::currentDateTimeUtc();
        event.vehicleId = vehicleId;
        event.position = position;
        event.inside = (state == State::Inside);
        event.distance = event.inside ? 0 : distance;
        event.zoneIdentifier = zone->identifier();
        event.zoneName = zone->name();
        event.restriction = zone->restriction();
        event.sourceText = zone->sourceText();
        event.message = message;
        event.timeToEntry = timeToEntry;
        alerts.append(event);
    }

    // Simultaneous alerts most severe first, entries before approaches
    std::stable_sort(alerts.begin(), alerts.end(), [](const AlertEvent &a, const AlertEvent &b) {
        const int severityA = GeoZone::restrictionSeverity(a.restriction);
        const int severityB = GeoZone::restrictionSeverity(b.restriction);
        return (severityA != severityB) ? (severityA < severityB) : (a.inside && !b.inside);
    });

    // Zones out of range or no longer active can alert again
    for (auto it = tracker.states.begin(); it != tracker.states.end();) {
        it = nearbyZones.contains(it.key()) ? std::next(it) : tracker.states.erase(it);
    }

    // The most restrictive first, so being inside large U-space airspace doesn't hide a prohibited zone ahead,
    // then zones the vehicle is in, then the closest
    std::stable_sort(nearby.begin(), nearby.end(), [](const Nearby &a, const Nearby &b) {
        const int severityA = GeoZone::restrictionSeverity(a.zone->restriction());
        const int severityB = GeoZone::restrictionSeverity(b.zone->restriction());
        if (severityA != severityB) {
            return severityA < severityB;
        }
        if (a.inside != b.inside) {
            return a.inside;
        }
        return a.distance < b.distance;
    });

    if (nearby != tracker.nearby) {
        tracker.nearby = nearby;
        if (vehicleId == _activeVehicleId) {
            _publishNearby();
        }
    }

    // Zones above and below are only shown for the active vehicle
    if (vehicleId == _activeVehicleId) {
        const QList<VerticalBand> bands = _verticalBands(position);
        const bool altitudeChanged = (std::isnan(position.altitudeAMSL) != std::isnan(tracker.position.altitudeAMSL)) ||
                                     (std::abs(position.altitudeAMSL - tracker.position.altitudeAMSL) >= 0.5) ||
                                     (std::isnan(position.terrainAMSL) != std::isnan(tracker.position.terrainAMSL)) ||
                                     (std::abs(position.terrainAMSL - tracker.position.terrainAMSL) >= 0.5);
        if ((bands != tracker.bands) || altitudeChanged) {
            tracker.bands = bands;
            tracker.position = position;
            emit verticalBandsChanged();
        }
    }

    for (const AlertEvent &event : std::as_const(alerts)) {
        emit alertEvent(event);
        emit alert(event.message, GeoZone::restrictionSeverity(event.restriction));
    }
}

void GeoZoneMonitor::_speak(const QString &message)
{
    // MainWindow shows the message without blocking the user
    if (SettingsManager::instance()->geoZoneSettings()->audioAlerts()->rawValue().toBool()) {
        AudioOutput::instance()->say(message);
    }
}
