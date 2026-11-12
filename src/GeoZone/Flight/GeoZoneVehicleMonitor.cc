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
#include "GeoZoneFenceWarning.h"
#include "GeoZoneInfo.h"
#include "GeoZoneMonitor.h"
#include "GeoZoneNearbyModel.h"
#include "GeoZoneTerrain.h"
#include "GeoZoneText.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

#include <QtCore/QVariantMap>

#include <algorithm>
#include <cmath>

GeoZoneVehicleMonitor::GeoZoneVehicleMonitor(Vehicle *vehicle, int vehicleId, int key, const Clock &clock, QObject *parent)
    : QObject(parent)
    , _vehicle(vehicle)
    , _followsVehicle(vehicle != nullptr)
    , _vehicleId(vehicleId)
    , _key(key)
    , _clock(clock)
    , _nearbyModel(new GeoZoneNearbyModel(this))
{
    (void) connect(this, &GeoZoneVehicleMonitor::nearbyChanged, this, [this]() { _nearbyModel->setItems(_nearby); });
    (void) connect(this, &GeoZoneVehicleMonitor::nearbyChanged, this, &GeoZoneVehicleMonitor::keptZonesChanged);
    (void) connect(this, &GeoZoneVehicleMonitor::predictionChanged, this, &GeoZoneVehicleMonitor::keptZonesChanged);
    if (vehicle) {
        _fenceWarning = new GeoZoneFenceWarning(vehicle, clock, this);
        // The breach of a vehicle which doesn't report its own, like PX4
        (void) connect(_fenceWarning, &GeoZoneFenceWarning::outsideChanged, this, [this]() {
            if (!_fenceStatusKnown) {
                _deriveFenceStatus();
            }
        });
        (void) connect(vehicle, &Vehicle::mavlinkMessageReceived, this, [this](const mavlink_message_t &message) {
            handleMessage(message, _clock());
        });
        (void) connect(vehicle, &Vehicle::armedChanged, this, &GeoZoneVehicleMonitor::armedChanged);
    }
}

Vehicle *GeoZoneVehicleMonitor::vehicle() const
{
    return _vehicle;
}

void GeoZoneVehicleMonitor::_positionReceived(qint64 msecs)
{
    if (_positionMSecs >= 0) {
        _positionIntervals.append((std::max)(qint64{ 0 }, msecs - _positionMSecs));
        if (_positionIntervals.count() > positionIntervalCount) {
            _positionIntervals.removeFirst();
        }
    }
    _positionMSecs = msecs;
}

void GeoZoneVehicleMonitor::_setFenceStatus(bool known, bool breached, int breachType, int breachCount)
{
    const bool changed = (known != _fenceStatusKnown) || (breached != _fenceBreached) || (breachType != _fenceBreachType) || (breachCount != _fenceBreachCount);
    const bool started = breached && !_fenceBreached;
    _fenceStatusKnown = known;
    _fenceBreached = breached;
    _fenceBreachType = breachType;
    _fenceBreachCount = breachCount;
    if (_fenceWarning) {
        _fenceWarning->setBreachReported(known && breached);
    }
    if (started) {
        emit fenceBreachStarted(fenceBreachText());
    }
    if (changed) {
        emit fenceStatusChanged();
    }
}

void GeoZoneVehicleMonitor::_deriveFenceStatus()
{
    int breachType = FENCE_BREACH_NONE;
    const std::optional<GeoZoneFencePrediction::Boundary> boundary = _fenceWarning ? _fenceWarning->outsideBoundary() : std::nullopt;
    if (boundary) {
        switch (*boundary) {
        case GeoZoneFencePrediction::Boundary::MaxAltitude:
            breachType = FENCE_BREACH_MAXALT;
            break;
        case GeoZoneFencePrediction::Boundary::MinAltitude:
            breachType = FENCE_BREACH_MINALT;
            break;
        case GeoZoneFencePrediction::Boundary::Polygon:
        case GeoZoneFencePrediction::Boundary::Circle:
        case GeoZoneFencePrediction::Boundary::MaxDistance:
            breachType = FENCE_BREACH_BOUNDARY;
            break;
        }
    }
    _setFenceStatus(false, boundary.has_value(), breachType, 0);
}

qint64 GeoZoneVehicleMonitor::_usualPositionInterval() const
{
    if (_positionIntervals.isEmpty()) {
        return 0;
    }
    // The median of the last ones, the lower of the two in the middle: neither one message after an outage nor two
    // messages arriving together change it
    QList<qint64> sorted = _positionIntervals;
    std::sort(sorted.begin(), sorted.end());
    return sorted[(sorted.count() - 1) / 2];
}

qint64 GeoZoneVehicleMonitor::positionTimeout() const
{
    return std::clamp(positionTimeoutIntervals * _usualPositionInterval(), qint64{ positionTimeoutMSecs }, qint64{ maxPositionTimeoutMSecs });
}

void GeoZoneVehicleMonitor::handleMessage(const mavlink_message_t &message, qint64 msecs)
{
    if (_fenceWarning) {
        _fenceWarning->handleMessage(message, msecs);
    }
    if (_fenceStatusKnown && (message.msgid != MAVLINK_MSG_ID_FENCE_STATUS) && ((msecs - _fenceStatusMSecs) > fenceStatusTimeoutMSecs)) {
        // The vehicle no longer reports its fence, like ArduPilot with its fence disabled
        _deriveFenceStatus();
    }
    _thresholds.handleMessage(message, msecs);
    switch (message.msgid) {
    // The position is as old as the last message the Vehicle took its coordinate from, see Vehicle::_handleGpsRawInt,
    // _handleGlobalPositionInt and _handleHighLatency: other messages leave the coordinate where it was
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t globalPosition{};
        mavlink_msg_global_position_int_decode(&message, &globalPosition);
        // ArduPilot sends 0/0 without a position, which the Vehicle ignores
        if ((globalPosition.lat == 0) && (globalPosition.lon == 0)) {
            break;
        }
        _globalPositionReceived = true;
        _positionReceived(msecs);
        // Ground velocity in centimeters per second, down positive
        _reported = Velocity{ globalPosition.vx / 100.0, globalPosition.vy / 100.0, -globalPosition.vz / 100.0, true };
        _velocityMSecs = msecs;
        break;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
        mavlink_gps_raw_int_t gps{};
        mavlink_msg_gps_raw_int_decode(&message, &gps);
        // The coordinate of the Vehicle follows the receiver only until the first GLOBAL_POSITION_INT
        if ((gps.fix_type >= GPS_FIX_TYPE_3D_FIX) && !_globalPositionReceived) {
            _positionReceived(msecs);
        }
        _gnss.known = true;
        _gnss.fixType = gps.fix_type;
        _gnss.satellites = (gps.satellites_visible == UINT8_MAX) ? -1 : gps.satellites_visible;
        _gnss.hdop = (gps.eph == UINT16_MAX) ? qQNaN() : (gps.eph / 100.0);
        // Millimeters, 0 from receivers which don't report it
        _gnss.horizontalAccuracy = (gps.h_acc == 0) ? qQNaN() : (gps.h_acc / 1000.0);
        _gnssMSecs = msecs;
        break;
    }
    case MAVLINK_MSG_ID_HIGH_LATENCY:
    case MAVLINK_MSG_ID_HIGH_LATENCY2:
        _positionReceived(msecs);
        break;
    case MAVLINK_MSG_ID_COMMAND_ACK: {
        // The vehicle answers an arm command also when its pre-arm checks refuse it. The acknowledgement doesn't tell
        // whether the command armed or disarmed: an accepted one is that of a disarm command reaching a vehicle
        // already on the ground, or of an arm command, whose attempt is alerted when the vehicle reports being armed.
        mavlink_command_ack_t ack{};
        mavlink_msg_command_ack_decode(&message, &ack);
        const bool refused = (ack.result != MAV_RESULT_ACCEPTED) && (ack.result != MAV_RESULT_IN_PROGRESS);
        if ((ack.command == MAV_CMD_COMPONENT_ARM_DISARM) && refused && onGround()) {
            emit armingAttempted();
        }
        break;
    }
    case MAVLINK_MSG_ID_FENCE_STATUS: {
        mavlink_fence_status_t fenceStatus{};
        mavlink_msg_fence_status_decode(&message, &fenceStatus);
        // What the vehicle reports replaces what its position tells
        _fenceStatusMSecs = msecs;
        _setFenceStatus(true, fenceStatus.breach_status != 0, fenceStatus.breach_type, fenceStatus.breach_count);
        break;
    }
    default:
        break;
    }
}

std::optional<bool> GeoZoneVehicleMonitor::armed() const
{
    if (_followsVehicle) {
        const Vehicle *const vehicle = _vehicle;
        if (!vehicle || vehicle->isOfflineEditingVehicle()) {
            return std::nullopt;
        }
        return vehicle->armed();
    }
    return _armed;
}

void GeoZoneVehicleMonitor::setArmed(bool armed)
{
    if (_followsVehicle || (_armed == armed)) {
        return;
    }
    _armed = armed;
    emit armedChanged();
}

void GeoZoneVehicleMonitor::setGroundStatus(const QString &warning, int level)
{
    if ((warning == _groundWarning) && (level == _groundLevel)) {
        return;
    }
    _groundWarning = warning;
    _groundLevel = level;
    emit groundStatusChanged();
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

GeoZoneAwareness::Gnss GeoZoneVehicleMonitor::gnss(qint64 msecs) const
{
    if (_gnssMSecs < 0) {
        return GeoZoneAwareness::Gnss();
    }
    if ((msecs - _gnssMSecs) > positionTimeoutMSecs) {
        GeoZoneAwareness::Gnss unknown;
        unknown.stale = true;
        return unknown;
    }
    return _gnss;
}

bool GeoZoneVehicleMonitor::positionStale(qint64 msecs) const
{
    if (_positionMSecs < 0) {
        // Vehicles which never sent a position message, like those of tests, keep their coordinate
        return false;
    }
    return (msecs - _positionMSecs) > positionTimeout();
}

GeoZoneVehicleMonitor::Velocity GeoZoneVehicleMonitor::velocity(qint64 msecs, const GeoZone::Position &position)
{
    // A position is a sample of the estimate once. Evaluated again before the next message, the same position would
    // read as a vehicle standing still, on a slow link at most evaluations.
    if ((_positionMSecs < 0) || (_positionMSecs != _sampledMSecs)) {
        _sampledMSecs = _positionMSecs;
        _estimated = _estimator.add((_positionMSecs < 0) ? msecs : _positionMSecs, position);
        _estimatedMSecs = msecs;
    } else if ((msecs - _estimatedMSecs) > GeoZoneVelocityEstimator::windowMSecs) {
        _estimated = Velocity();
    }
    // The velocity sent with a position is as current as the position, so for longer on a slow link
    const qint64 maxAge = (std::max)(qint64{ reportedVelocityMaxAgeMSecs }, (_usualPositionInterval() * 3) / 2);
    const bool fresh = (_velocityMSecs >= 0) && ((msecs - _velocityMSecs) <= maxAge);
    if (!fresh) {
        return _estimated;
    }
    // Some vehicles and simulators report no velocity as zero while moving, which their positions show, or the ground
    // speed they report while there are too few positions: then the velocity isn't known
    const bool reportedZero = (_reported.north == 0) && (_reported.east == 0) && (_reported.up == 0);
    if (reportedZero) {
        const auto moving = [](double groundSpeed, double climb) {
            return (groundSpeed >= GeoZoneAlertTracker::minPredictionSpeed) || (std::abs(climb) >= GeoZoneAlertTracker::minPredictionSpeed);
        };
        if (_estimated.valid) {
            return moving(_estimated.groundSpeed(), _estimated.up) ? _estimated : _reported;
        }
        const GeoZoneVehicleThresholds::Speeds speeds = _thresholds.speedsWithoutVelocity(msecs, false, positionTimeout());
        if ((speeds.source == GeoZoneAlertThresholds::SpeedSource::Reported) && moving(speeds.ground, speeds.vertical)) {
            return Velocity();
        }
    }
    return _reported;
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
        _estimated = Velocity();
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
    _thresholds.setVehicleClass(GeoZoneVehicleThresholds::vehicleClass(vehicle->vehicleType()));
    if (_fenceWarning) {
        const GeoZoneAlertThresholds::Profile profile = GeoZoneAlertThresholds::profile(_thresholds.vehicleClass());
        _fenceWarning->setWarningTime(profile.pilotReactionTime + profile.fenceWarningMargin);
    }
    TerrainSource terrainSource = TerrainSource::Unknown;
    const GeoZone::Position position = vehiclePosition(vehicle, vehicle->coordinate(), &terrainSource);
    if (terrainSource != _terrainSource) {
        _terrainSource = terrainSource;
        emit terrainSourceChanged();
    }
    return evaluateAt(msecs, position, context);
}

QList<GeoZoneAlertTracker::Alert> GeoZoneVehicleMonitor::evaluateAt(qint64 msecs, const GeoZone::Position &position, const Context &context)
{
    // A flying vehicle whose velocity isn't known yet, or can't be estimated on a slow link, is not taken to hover
    const Velocity current = velocity(msecs, position);
    return _evaluate(position, current, context,
                     current.valid ? GeoZoneVehicleThresholds::Speeds() : _thresholds.speedsWithoutVelocity(msecs, onGround(), positionTimeout()));
}

QList<GeoZoneAlertTracker::Alert> GeoZoneVehicleMonitor::evaluate(const GeoZone::Position &position, const Velocity &velocity, const Context &context)
{
    return _evaluate(position, velocity, context, GeoZoneVehicleThresholds::Speeds());
}

QList<GeoZoneAlertTracker::Alert> GeoZoneVehicleMonitor::_evaluate(const GeoZone::Position &position, const Velocity &velocity, const Context &context,
                                                                   const GeoZoneVehicleThresholds::Speeds &speeds)
{
    if (!position.coordinate.isValid() || !context.catalog) {
        return {};
    }
    _position = position;

    // The thresholds at the current velocity, unless fixed
    const QVariantMap previousThresholds = alertThresholds();
    const GeoZoneAlertTracker::Limits limits = context.limits.value_or(_thresholds.update(_clock(), velocity, context.extras, speeds).limits());
    _fixedLimits = context.limits.has_value();
    if (alertThresholds() != previousThresholds) {
        emit alertThresholdsChanged();
    }

    // A zone which started or ended since the last evaluation applies, or no longer does, from this one
    if (context.refreshActive) {
        context.refreshActive(GeoZoneAlertTracker::searchArea(position, velocity, limits));
    }
    const GeoZoneAlertTracker::Update update = _alerts.update(*context.catalog, context.policy, position, velocity, limits);
    if (update.nearby != _nearby) {
        _nearby = update.nearby;
        emit nearbyChanged();
    }
    const bool hadEntry = _entry.has_value();
    _entry = update.firstEntry;
    if (hadEntry || _entry) {
        emit predictionChanged();
    }

    const QList<GeoZoneVerticalBand> bands = GeoZoneVerticalBand::at(*context.catalog, context.policy, position);
    const auto moved = [](double a, double b) { return (std::isnan(a) != std::isnan(b)) || (std::abs(a - b) >= bandsMoveMeters); };
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

QSet<QString> GeoZoneVehicleMonitor::nearbyZoneKeys() const
{
    QSet<QString> keys;
    for (const Nearby &nearby : _nearby) {
        keys.insert(nearby.zone->key());
    }
    return keys;
}

bool GeoZoneVehicleMonitor::inside() const
{
    return std::any_of(_nearby.cbegin(), _nearby.cend(), [](const Nearby &zone) { return zone.inside; });
}

QVariantList GeoZoneVehicleMonitor::keptZones() const
{
    QVariantList zones;
    for (const Nearby &nearby : _nearby) {
        zones.append(QVariant::fromValue(GeoZoneInfo(nearby.zone)));
    }
    if (const std::optional<Entry> predicted = entry()) {
        zones.append(QVariant::fromValue(GeoZoneInfo(predicted->zone)));
    }
    return zones;
}

QVariantList GeoZoneVehicleMonitor::verticalBands() const
{
    QVariantList bands;
    for (const GeoZoneVerticalBand &band : _bands) {
        bands.append(QVariantMap{
            { QStringLiteral("restriction"), static_cast<int>(band.zone->restriction()) },
            { QStringLiteral("severity"), GeoZone::restrictionSeverity(band.zone->restriction()) },
            { QStringLiteral("name"), band.zone->displayName() },
            { QStringLiteral("limitsText"), GeoZoneText::verticalLimits(band.zone->data()) },
            { QStringLiteral("floor"), band.floor },
            { QStringLiteral("ceiling"), band.ceiling },
        });
    }
    return bands;
}

QVariantList GeoZoneVehicleMonitor::predictedPath() const
{
    const std::optional<Entry> predicted = entry();
    return predicted ? QVariantList{ QVariant::fromValue(_position.coordinate), QVariant::fromValue(predicted->position.coordinate) } : QVariantList();
}

QGeoCoordinate GeoZoneVehicleMonitor::predictedEntry() const
{
    const std::optional<Entry> predicted = entry();
    return predicted ? predicted->position.coordinate : QGeoCoordinate();
}

int GeoZoneVehicleMonitor::predictedEntryTime() const
{
    const std::optional<Entry> predicted = entry();
    return predicted ? static_cast<int>(std::lround(predicted->time)) : -1;
}

QVariant GeoZoneVehicleMonitor::predictedEntryZone() const
{
    const std::optional<Entry> predicted = entry();
    return predicted ? QVariant::fromValue(GeoZoneInfo(predicted->zone)) : QVariant();
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
