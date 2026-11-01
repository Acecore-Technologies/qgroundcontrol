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
#include <QtPositioning/QGeoCircle>

#include <algorithm>
#include <cmath>

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
            emit dataChanged(changed, changed, { InsideRole, DistanceRole });
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
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GeoZoneNearbyModel::roleNames() const
{
    return { { ZoneRole, "zone" }, { InsideRole, "inside" }, { DistanceRole, "distance" } };
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
    for (const QPointer<Vehicle> &vehicle : std::as_const(_vehicles)) {
        if (vehicle && vehicle->coordinate().isValid()) {
            evaluate(vehiclePosition(vehicle, vehicle->coordinate()), vehicle->id());
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
        if (it->nearby.removeIf([source](const Nearby &nearby) { return nearby.zone->parent() == source; }) > 0) {
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

void GeoZoneMonitor::evaluate(const GeoZone::Position &position, int vehicleId)
{
    if (!position.coordinate.isValid()) {
        return;
    }

    // U-space airspace requires a U-space flight authorisation, no restriction zones are only shown on the map
    static const QList<GeoZone::Restriction> restrictions = { GeoZone::Prohibited, GeoZone::ReqAuthorisation, GeoZone::Conditional, GeoZone::USpace };
    GeoZoneSettings *const settings = SettingsManager::instance()->geoZoneSettings();
    const double approachDistance = settings->approachDistance()->rawValue().toDouble();
    const double approachVerticalDistance = settings->approachVerticalDistance()->rawValue().toDouble();
    const QGeoRectangle area = QGeoCircle(position.coordinate, approachDistance + hysteresisDistance).boundingGeoRectangle();

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
        if (nearDistance > nearLimit) {
            continue;
        }
        const double insideDistance = zone->distanceTo(position, (previous == State::Inside) ? hysteresisDistance : 0, hysteresisDistance);
        const double distance = (std::min)(insideDistance, nearDistance);
        const State state = ((insideDistance <= 0) || ((previous == State::Inside) && (insideDistance <= hysteresisDistance))) ? State::Inside : State::Near;

        nearbyZones.insert(key);
        tracker.states[key] = state;
        nearby.append(Nearby{ zone, state == State::Inside, static_cast<int>(std::lround(distance)) });
        if (state <= previous) {
            continue;
        }

        const QString name = zone->displayName();
        QString message = (state == State::Inside) ?
            tr("Entered geozone %1 (%2)").arg(name, zone->restrictionText()) :
            tr("Approaching geozone %1 (%2)").arg(name, zone->restrictionText());
        if (!vehicleLabel.isEmpty()) {
            message = QStringLiteral("%1: %2").arg(vehicleLabel, message);
        }
        qCDebug(GeoZoneMonitorLog) << message << "distance" << distance;

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
