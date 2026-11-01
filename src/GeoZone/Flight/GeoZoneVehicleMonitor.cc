/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneVehicleMonitor.h"
#include "GeoZoneCatalog.h"
#include "GeoZoneMonitor.h"
#include "GeoZoneTerrain.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

#include <algorithm>
#include <cmath>

GeoZoneVehicleMonitor::GeoZoneVehicleMonitor(Vehicle *vehicle, int vehicleId, int key, const Clock &clock, QObject *parent)
    : QObject(parent)
    , _vehicle(vehicle)
    , _followsVehicle(vehicle != nullptr)
    , _vehicleId(vehicleId)
    , _key(key)
    , _clock(clock)
{
    if (vehicle) {
        (void) connect(vehicle, &Vehicle::mavlinkMessageReceived, this, [this](const mavlink_message_t &message) {
            handleMessage(message, _clock());
        });
    }
}

Vehicle *GeoZoneVehicleMonitor::vehicle() const
{
    return _vehicle;
}

void GeoZoneVehicleMonitor::_positionReceived(qint64 msecs)
{
    if (_positionMSecs >= 0) {
        _positionIntervalMSecs = msecs - _positionMSecs;
    }
    _positionMSecs = msecs;
}

void GeoZoneVehicleMonitor::handleMessage(const mavlink_message_t &message, qint64 msecs)
{
    switch (message.msgid) {
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t globalPosition{};
        mavlink_msg_global_position_int_decode(&message, &globalPosition);
        _positionReceived(msecs);
        // Ground velocity in centimeters per second, down positive
        _reported = Velocity{ globalPosition.vx / 100.0, globalPosition.vy / 100.0, -globalPosition.vz / 100.0, true };
        _velocityMSecs = msecs;
        break;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
        mavlink_gps_raw_int_t gps{};
        mavlink_msg_gps_raw_int_decode(&message, &gps);
        if (gps.fix_type >= GPS_FIX_TYPE_2D_FIX) {
            _positionReceived(msecs);
        }
        break;
    }
    case MAVLINK_MSG_ID_HIGH_LATENCY2:
        _positionReceived(msecs);
        break;
    case MAVLINK_MSG_ID_FENCE_STATUS: {
        mavlink_fence_status_t fenceStatus{};
        mavlink_msg_fence_status_decode(&message, &fenceStatus);
        const bool breached = (fenceStatus.breach_status != 0);
        const bool changed = !_fenceStatusKnown || (breached != _fenceBreached) ||
                             (fenceStatus.breach_type != _fenceBreachType) || (fenceStatus.breach_count != _fenceBreachCount);
        const bool started = breached && !_fenceBreached;
        _fenceStatusKnown = true;
        _fenceBreached = breached;
        _fenceBreachType = fenceStatus.breach_type;
        _fenceBreachCount = fenceStatus.breach_count;
        if (started) {
            emit fenceBreachStarted(fenceBreachText());
        }
        if (changed) {
            emit fenceStatusChanged();
        }
        break;
    }
    default:
        break;
    }
}

QString GeoZoneVehicleMonitor::fenceBreachText() const
{
    return _fenceBreached ? fenceBreachText(_fenceBreachType, _fenceBreachCount) : QString();
}

QString GeoZoneVehicleMonitor::fenceBreachText(int breachType, int breachCount)
{
    QString text;
    switch (breachType) {
    case FENCE_BREACH_MINALT:
        text = tr("Minimum altitude fence breached");
        break;
    case FENCE_BREACH_MAXALT:
        text = tr("Maximum altitude fence breached");
        break;
    case FENCE_BREACH_BOUNDARY:
        text = tr("Boundary fence breached");
        break;
    default:
        text = tr("Fence breached");
        break;
    }
    if (breachCount > 1) {
        text += QStringLiteral(" ") + tr("(%n time(s))", nullptr, breachCount);
    }
    return text;
}

bool GeoZoneVehicleMonitor::positionStale(qint64 msecs) const
{
    if (_positionMSecs < 0) {
        // Vehicles which never sent a position message, like those of tests, keep their coordinate
        return false;
    }
    // Links like satellite ones send positions less often
    const qint64 timeout = (std::max)(qint64{ positionTimeoutMSecs }, 3 * _positionIntervalMSecs);
    return (msecs - _positionMSecs) > timeout;
}

GeoZoneVehicleMonitor::Velocity GeoZoneVehicleMonitor::velocity(qint64 msecs, const GeoZone::Position &position)
{
    const Velocity estimated = _estimator.add(msecs, position);
    const bool fresh = (_velocityMSecs >= 0) && ((msecs - _velocityMSecs) <= reportedVelocityMaxAgeMSecs);
    // Some vehicles and simulators report no velocity as zero while moving, which their positions show
    const bool reportedZero = (_reported.north == 0) && (_reported.east == 0) && (_reported.up == 0);
    const bool estimatedMoving = estimated.valid && ((estimated.groundSpeed() >= GeoZoneAlertTracker::minPredictionSpeed) ||
                                                     (std::abs(estimated.up) >= GeoZoneAlertTracker::minPredictionSpeed));
    return (fresh && !(reportedZero && estimatedMoving)) ? _reported : estimated;
}

void GeoZoneVehicleMonitor::setPositionUnavailable(bool unavailable)
{
    if (unavailable == _unavailable) {
        return;
    }
    _unavailable = unavailable;
    if (unavailable) {
        // Positions from before the gap would distort the estimated velocity
        _estimator = GeoZoneVelocityEstimator();
        qCDebug(GeoZoneMonitorLog) << "Alerts paused, no current position of vehicle" << _vehicleId;
    }
    emit positionUnavailableChanged();
    if (_entry) {
        emit predictionChanged();
    }
}

QList<GeoZoneAlertTracker::Alert> GeoZoneVehicleMonitor::update(const Context &context)
{
    Vehicle *const vehicle = _vehicle;
    if (!vehicle) {
        return {};
    }
    const qint64 msecs = _clock();
    // The last known position of a vehicle which lost its link or position isn't where it is
    const VehicleLinkManager *const links = vehicle->vehicleLinkManager();
    const bool unavailable = !vehicle->coordinate().isValid() || (links && links->communicationLost()) || positionStale(msecs);
    setPositionUnavailable(unavailable);
    if (unavailable) {
        return {};
    }
    TerrainSource terrainSource = TerrainSource::Unknown;
    const GeoZone::Position position = vehiclePosition(vehicle, vehicle->coordinate(), &terrainSource);
    if (terrainSource != _terrainSource) {
        _terrainSource = terrainSource;
        emit terrainSourceChanged();
    }
    return evaluate(position, velocity(msecs, position), context);
}

QList<GeoZoneAlertTracker::Alert> GeoZoneVehicleMonitor::evaluate(const GeoZone::Position &position, const Velocity &velocity, const Context &context)
{
    if (!position.coordinate.isValid() || !context.catalog) {
        return {};
    }
    _position = position;

    const GeoZoneAlertTracker::Update update = _alerts.update(*context.catalog, context.policy, position, velocity, context.limits);
    if (update.nearby != _nearby) {
        _nearby = update.nearby;
        emit nearbyChanged();
    }
    const bool hadEntry = _entry.has_value();
    _entry = update.firstEntry;
    _entryFrom = position.coordinate;
    if (hadEntry || _entry) {
        emit predictionChanged();
    }

    const QList<GeoZoneVerticalBand> bands = GeoZoneVerticalBand::at(*context.catalog, context.policy, position);
    const auto moved = [](double a, double b) { return (std::isnan(a) != std::isnan(b)) || (std::abs(a - b) >= 0.5); };
    if ((bands != _bands) || moved(position.altitudeAMSL, _bandsPosition.altitudeAMSL) || moved(position.terrainAMSL, _bandsPosition.terrainAMSL)) {
        _bands = bands;
        _bandsPosition = position;
        emit verticalBandsChanged();
    }
    return update.alerts;
}

QSet<QString> GeoZoneVehicleMonitor::insideZoneKeys() const
{
    QSet<QString> keys;
    for (const Nearby &nearby : _nearby) {
        if (nearby.inside) {
            keys.insert(nearby.zone->key());
        }
    }
    return keys;
}

void GeoZoneVehicleMonitor::reset()
{
    _alerts.reset();
    const bool hadNearby = !_nearby.isEmpty();
    const bool hadBands = !_bands.isEmpty() || _bandsPosition.coordinate.isValid();
    const bool hadEntry = _entry.has_value();
    _nearby.clear();
    _bands.clear();
    _bandsPosition = GeoZone::Position();
    _entry.reset();
    if (hadNearby) {
        emit nearbyChanged();
    }
    if (hadBands) {
        emit verticalBandsChanged();
    }
    if (hadEntry) {
        emit predictionChanged();
    }
}

void GeoZoneVehicleMonitor::forgetZones(const QString &sourceKey)
{
    if (_entry && (_entry->zone->sourceKey() == sourceKey)) {
        _entry.reset();
        emit predictionChanged();
    }
    if (_bands.removeIf([&sourceKey](const GeoZoneVerticalBand &band) { return band.zone->sourceKey() == sourceKey; }) > 0) {
        emit verticalBandsChanged();
    }
    if (_nearby.removeIf([&sourceKey](const Nearby &nearby) { return nearby.zone->sourceKey() == sourceKey; }) > 0) {
        emit nearbyChanged();
    }
}

GeoZone::Position GeoZoneVehicleMonitor::vehiclePosition(Vehicle *vehicle, const QGeoCoordinate &coordinate, TerrainSource *terrainSource)
{
    if (!vehicle) {
        if (terrainSource) {
            *terrainSource = TerrainSource::Unknown;
        }
        return GeoZone::Position{ coordinate };
    }
    return vehiclePosition(coordinate, vehicle->altitudeAMSL()->rawValue().toDouble(), vehicle->altitudeAboveTerr()->rawValue().toDouble(),
                           vehicle->armed(), terrainSource);
}

GeoZone::Position GeoZoneVehicleMonitor::vehiclePosition(const QGeoCoordinate &coordinate, double altitudeAMSL, double heightAboveTerrain, bool armed, TerrainSource *terrainSource)
{
    GeoZone::Position position{ coordinate };
    TerrainSource source = TerrainSource::Unknown;
    if (!qIsNaN(altitudeAMSL)) {
        position.altitudeAMSL = altitudeAMSL;

        // Without terrain data limits above ground can't be excluded. The height above home is not used instead
        // since it misses zones over rising ground. A disarmed vehicle is on the ground. The vehicle doesn't query
        // the terrain again while it hovers, so tiles loaded since its last query are looked up.
        if (!qIsNaN(heightAboveTerrain)) {
            position.terrainAMSL = altitudeAMSL - heightAboveTerrain;
            source = TerrainSource::Vehicle;
        } else if (!armed) {
            position.terrainAMSL = altitudeAMSL;
            source = TerrainSource::Ground;
        } else {
            position.terrainAMSL = GeoZoneTerrain::elevationAt(coordinate);
            source = qIsNaN(position.terrainAMSL) ? TerrainSource::Unknown : TerrainSource::Database;
        }
    }
    if (terrainSource) {
        *terrainSource = source;
    }
    return position;
}
