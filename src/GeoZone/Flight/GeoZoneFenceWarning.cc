/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFenceWarning.h"
#include "Fact.h"
#include "FactMetaData.h"
#include "GeoFenceManager.h"
#include "GeoZoneMonitor.h"
#include "MissionManager.h"
#include "ParameterManager.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "RallyPointManager.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QLocale>
#include <QtCore/QTimer>

#include <cmath>
#include <memory>

namespace {

/// Of the default component, invalid when the vehicle doesn't have it or its parameters aren't loaded
QVariant _parameter(Vehicle *vehicle, const QString &name)
{
    ParameterManager *const parameters = vehicle->parameterManager();
    if (!parameters || !parameters->parametersReady() || !parameters->parameterExists(ParameterManager::defaultComponentId, name)) {
        return QVariant();
    }
    return parameters->getParameter(ParameterManager::defaultComponentId, name)->rawValue();
}

/// Parameters the config is made of, followed for changes
const QStringList &_fenceParameters()
{
    static const QStringList names = {
        QStringLiteral("GF_ACTION"), QStringLiteral("GF_MAX_HOR_DIST"), QStringLiteral("GF_MAX_VER_DIST"),
        QStringLiteral("FENCE_ENABLE"), QStringLiteral("FENCE_AUTOENABLE"), QStringLiteral("FENCE_TYPE"), QStringLiteral("FENCE_ACTION"),
        QStringLiteral("FENCE_RADIUS"), QStringLiteral("FENCE_ALT_MAX"), QStringLiteral("FENCE_ALT_MIN"), QStringLiteral("FENCE_MARGIN"),
        QStringLiteral("FENCE_MARGIN_XY"), QStringLiteral("FENCE_OPTIONS"), QStringLiteral("FENCE_ALT_MAX_TP"), QStringLiteral("FENCE_ALT_MIN_TP"),
    };
    return names;
}

// Fences of ArduPilot's FENCE_TYPE
constexpr int kArduPilotPolygonsBit = 4;
constexpr int kArduPilotCircleBit = 2;
constexpr int kArduPilotAltMaxBit = 1;
constexpr int kArduPilotAltMinBit = 8;

} // namespace

GeoZoneFenceWarning::GeoZoneFenceWarning(Vehicle *vehicle, const Clock &clock, QObject *parent)
    : QObject(parent)
    , _vehicle(vehicle)
    , _clock(clock)
{
    if (!_clock) {
        auto timer = std::make_shared<QElapsedTimer>();
        timer->start();
        _clock = [timer]() { return timer->elapsed(); };
    }
    _modeTimer.setSingleShot(true);
    _modeTimer.setInterval(modeSwitchTimeoutMSecs);
    (void) connect(&_modeTimer, &QTimer::timeout, this, [this]() { _modeSwitched(true); });
    _fenceCommandTimer.setSingleShot(true);
    _fenceCommandTimer.setInterval(fenceCommandTimeoutMSecs);
    (void) connect(&_fenceCommandTimer, &QTimer::timeout, this, &GeoZoneFenceWarning::_fenceCommandTimedOut);
    _connectVehicle();
    _updateConfig();
}

void GeoZoneFenceWarning::_connectVehicle()
{
    Vehicle *const vehicle = _vehicle;
    if (!vehicle) {
        return;
    }
    const auto update = [this]() { _updateConfig(); };
    // While the fence is unknown it is read back once the transfers of a plan are done, after their completion was
    // handled, which may start the next transfer
    const auto reload = [this](bool inProgress) {
        if (!inProgress && _fenceUnknown) {
            QTimer::singleShot(0, this, &GeoZoneFenceWarning::_reloadFence);
        }
    };
    if (GeoFenceManager *const fenceManager = vehicle->geoFenceManager()) {
        // A download ends with loadComplete whether or not it failed, after reporting its errors
        (void) connect(fenceManager, &GeoFenceManager::inProgressChanged, this, [this, reload](bool inProgress) {
            if (inProgress) {
                _fenceTransferError = false;
            }
            reload(inProgress);
        });
        (void) connect(fenceManager, &GeoFenceManager::error, this, [this]() { _fenceTransferError = true; });
        (void) connect(fenceManager, &GeoFenceManager::loadComplete, this, [this]() { _fenceTransferred(_fenceTransferError); });
        (void) connect(fenceManager, &GeoFenceManager::sendComplete, this, [this](bool error) { _fenceTransferred(error); });
        (void) connect(fenceManager, &GeoFenceManager::removeAllComplete, this, [this](bool error) { _fenceTransferred(error); });
    }
    if (MissionManager *const missionManager = vehicle->missionManager()) {
        (void) connect(missionManager, &MissionManager::inProgressChanged, this, reload);
    }
    if (RallyPointManager *const rallyPointManager = vehicle->rallyPointManager()) {
        (void) connect(rallyPointManager, &RallyPointManager::inProgressChanged, this, reload);
    }
    (void) connect(vehicle, &Vehicle::flightModeChanged, this, [this]() { _modeSwitched(false); });
    (void) connect(vehicle, &Vehicle::mavCommandResult, this, [this](int, int, int command, int ackResult, int failureCode) {
        _commandResult(command, ackResult, failureCode);
    });
    (void) connect(vehicle, &Vehicle::homePositionChanged, this, update);
    (void) connect(vehicle, &Vehicle::armedChanged, this, update);
    (void) connect(vehicle, &Vehicle::flightModesChanged, this, update);
    (void) connect(vehicle, &Vehicle::vtolInFwdFlightChanged, this, update);
    if (ParameterManager *const parameters = vehicle->parameterManager()) {
        (void) connect(parameters, &ParameterManager::parametersReadyChanged, this, update);
    }
}

void GeoZoneFenceWarning::_fenceTransferred(bool failed)
{
    if (!failed) {
        _fenceReloads = 0;
    }
    if (failed != _fenceUnknown) {
        _fenceUnknown = failed;
        qCInfo(GeoZoneMonitorLog) << (failed ? "The fence transfer failed, the fence on the vehicle is unknown" : "The fence on the vehicle is known again");
        emit fenceUnknownChanged();
    }
    _updateConfig();
    if (failed) {
        // After the others handled the failure, like the plan upload going on with the rally points
        QTimer::singleShot(0, this, &GeoZoneFenceWarning::_reloadFence);
    }
}

void GeoZoneFenceWarning::_reloadFence()
{
    Vehicle *const vehicle = _vehicle;
    if (!_fenceUnknown || !vehicle || (_fenceReloads >= maxFenceReloads)) {
        return;
    }
    GeoFenceManager *const fenceManager = vehicle->geoFenceManager();
    const MissionManager *const missionManager = vehicle->missionManager();
    const RallyPointManager *const rallyPointManager = vehicle->rallyPointManager();
    // Autopilots transfer one plan type at a time
    if (!fenceManager || fenceManager->inProgress() || (missionManager && missionManager->inProgress()) || (rallyPointManager && rallyPointManager->inProgress())) {
        return;
    }
    _fenceReloads++;
    qCInfo(GeoZoneMonitorLog) << "Reading the fence back from the vehicle, attempt" << _fenceReloads;
    fenceManager->loadFromVehicle();
}

GeoZoneFenceWarning::StoredFence GeoZoneFenceWarning::storedFence(Vehicle *vehicle)
{
    StoredFence stored;
    GeoFenceManager *const fenceManager = vehicle ? vehicle->geoFenceManager() : nullptr;
    if (!fenceManager) {
        return stored;
    }
    for (const QGCFencePolygon &polygon : fenceManager->polygons()) {
        stored.polygons.append({ polygon.coordinateList(), polygon.inclusion() });
    }
    for (const QGCFenceCircle &circle : fenceManager->circles()) {
        stored.circles.append({ QGeoCircle(circle.center(), const_cast<QGCFenceCircle&>(circle).radius()->rawValue().toDouble()), circle.inclusion() });
    }
    return stored;
}

GeoZoneFenceWarning::Config GeoZoneFenceWarning::vehicleConfig(Vehicle *vehicle)
{
    return vehicleConfig(vehicle, storedFence(vehicle));
}

GeoZoneFenceWarning::Config GeoZoneFenceWarning::vehicleConfig(Vehicle *vehicle, const StoredFence &stored)
{
    Config config;
    if (!vehicle || vehicle->isOfflineEditingVehicle()) {
        return config;
    }
    if (vehicle->fixedWing() || (vehicle->vtol() && vehicle->vtolInFwdFlight())) {
        config.vehicleClass = VehicleClass::Plane;
    } else if (vehicle->rover()) {
        config.vehicleClass = VehicleClass::Rover;
    } else if (vehicle->sub()) {
        config.vehicleClass = VehicleClass::Sub;
    }
    GeoZoneFencePrediction::Fence &fence = config.fence;
    fence.home = vehicle->homePosition();
    const auto addShapes = [&stored, &fence]() {
        fence.polygons = stored.polygons;
        fence.circles = stored.circles;
    };
    const auto positive = [vehicle](const QString &name) {
        bool ok = false;
        const double value = _parameter(vehicle, name).toDouble(&ok);
        return (ok && (value > 0)) ? value : qQNaN();
    };

    if (vehicle->px4Firmware()) {
        config.firmware = Firmware::PX4;
        const QVariant action = _parameter(vehicle, QStringLiteral("GF_ACTION"));
        config.action = action.isValid() ? action.toInt() : -1;
        // PX4 checks its geofence unless the action is None
        config.enabled = config.action > 0;
        addShapes();
        fence.inclusionUnion = true;
        fence.maxDistance = positive(QStringLiteral("GF_MAX_HOR_DIST"));
        fence.maxAltitude = positive(QStringLiteral("GF_MAX_VER_DIST"));
    } else if (vehicle->apmFirmware()) {
        config.firmware = Firmware::ArduPilot;
        const QVariant action = _parameter(vehicle, QStringLiteral("FENCE_ACTION"));
        config.action = action.isValid() ? action.toInt() : -1;
        const QVariant enable = _parameter(vehicle, QStringLiteral("FENCE_ENABLE"));
        const int autoEnable = _parameter(vehicle, QStringLiteral("FENCE_AUTOENABLE")).toInt();
        config.enabled = enable.isValid() && ((enable.toInt() == 1) || ((autoEnable > 0) && vehicle->armed()));
        const int type = _parameter(vehicle, QStringLiteral("FENCE_TYPE")).toInt();
        if (type & kArduPilotPolygonsBit) {
            addShapes();
        }
        if (type & kArduPilotCircleBit) {
            fence.maxDistance = positive(QStringLiteral("FENCE_RADIUS"));
        }
        // Altitudes above home, or above sea level or terrain with the frame parameters of newer versions
        const auto altitudeLimit = [vehicle, &fence](const QString &name, const QString &frameName) {
            bool ok = false;
            const double limit = _parameter(vehicle, name).toDouble(&ok);
            if (!ok) {
                return qQNaN();
            }
            const QVariant frame = _parameter(vehicle, frameName);
            if (!frame.isValid() || (frame.toInt() == 1) || (frame.toInt() == 2)) {
                return limit;
            }
            if ((frame.toInt() == 0) && fence.home.isValid() && !std::isnan(fence.home.altitude())) {
                return limit - fence.home.altitude();
            }
            return qQNaN();
        };
        if (type & kArduPilotAltMaxBit) {
            fence.maxAltitude = altitudeLimit(QStringLiteral("FENCE_ALT_MAX"), QStringLiteral("FENCE_ALT_MAX_TP"));
        }
        if (type & kArduPilotAltMinBit) {
            fence.minAltitude = altitudeLimit(QStringLiteral("FENCE_ALT_MIN"), QStringLiteral("FENCE_ALT_MIN_TP"));
        }
        const double margin = (std::max)(_parameter(vehicle, QStringLiteral("FENCE_MARGIN")).toDouble(), 0.0);
        const double horizontalMargin = _parameter(vehicle, QStringLiteral("FENCE_MARGIN_XY")).toDouble();
        fence.verticalMargin = margin;
        fence.horizontalMargin = (horizontalMargin > 0) ? horizontalMargin : margin;
        const int options = _parameter(vehicle, QStringLiteral("FENCE_OPTIONS")).toInt();
        config.modeChangeBlocked = (options & 1) != 0;
        fence.inclusionUnion = (options & 2) != 0;
    }
    if (!fence.home.isValid()) {
        fence.maxDistance = qQNaN();
    }
    config.takeControlMode = takeControlMode(config.firmware, config.vehicleClass, vehicle->flightModes());
    return config;
}

void GeoZoneFenceWarning::setConfig(const Config &config)
{
    _fixedConfig = true;
    _config = config;
    emit configChanged();
    if (!enabled()) {
        _setBreach(std::nullopt);
        _setOutside(std::nullopt, -1);
    }
}

void GeoZoneFenceWarning::_updateConfig()
{
    Vehicle *const vehicle = _vehicle;
    if (_fixedConfig || !vehicle) {
        return;
    }
    // The fence parameters change while the vehicle flies, like the fence enabled from an RC switch
    delete _parameterConnections;
    _parameterConnections = new QObject(this);
    ParameterManager *const parameters = vehicle->parameterManager();
    if (parameters && parameters->parametersReady()) {
        for (const QString &name : _fenceParameters()) {
            if (parameters->parameterExists(ParameterManager::defaultComponentId, name)) {
                (void) connect(parameters->getParameter(ParameterManager::defaultComponentId, name), &Fact::rawValueChanged,
                               _parameterConnections, [this]() { QTimer::singleShot(0, this, [this]() { _updateConfig(); }); });
            }
        }
    }
    // While the fence is unknown the vehicle may still enforce the one it had
    if (!_fenceUnknown) {
        _storedFence = storedFence(vehicle);
    }
    _config = vehicleConfig(vehicle, _storedFence);
    emit configChanged();
    if (!enabled()) {
        _setBreach(std::nullopt);
        _setOutside(std::nullopt, -1);
    }
}

void GeoZoneFenceWarning::handleMessage(const mavlink_message_t &message, qint64 msecs)
{
    if (message.msgid != MAVLINK_MSG_ID_GLOBAL_POSITION_INT) {
        return;
    }
    mavlink_global_position_int_t globalPosition{};
    mavlink_msg_global_position_int_decode(&message, &globalPosition);
    const QGeoCoordinate coordinate(globalPosition.lat / 1e7, globalPosition.lon / 1e7);
    const double altitude = globalPosition.relative_alt / 1000.0;
    // Positions without a velocity, like those of simulators, give it over the last seconds
    GeoZone::Position position{ coordinate };
    position.altitudeAMSL = altitude;
    const Velocity estimated = _estimator.add(msecs, position);
    if ((_evaluatedMSecs >= 0) && ((msecs - _evaluatedMSecs) < minEvaluateIntervalMSecs)) {
        return;
    }
    _evaluatedMSecs = msecs;
    const bool reported = (globalPosition.vx != 0) || (globalPosition.vy != 0) || (globalPosition.vz != 0);
    _evaluate(coordinate, altitude, reported ? Velocity{ globalPosition.vx / 100.0, globalPosition.vy / 100.0, -globalPosition.vz / 100.0, true } : estimated, msecs);
}

void GeoZoneFenceWarning::evaluate(const QGeoCoordinate &coordinate, double altitude, const Velocity &velocity)
{
    _evaluate(coordinate, altitude, velocity, _clock());
}

void GeoZoneFenceWarning::_evaluate(const QGeoCoordinate &coordinate, double altitude, const Velocity &velocity, qint64 msecs)
{
    _position = coordinate;
    if (!enabled() || !coordinate.isValid()) {
        _setBreach(std::nullopt);
        _setOutside(std::nullopt, -1);
        return;
    }
    GeoZoneFencePrediction::Fence fence = _config.fence;
    // ArduPilot enables its floor once the vehicle climbed above it
    if (!std::isnan(altitude) && (altitude < fence.minAltitude)) {
        fence.minAltitude = qQNaN();
    }
    // Past the fence the vehicle holds, returns or lands, so this doesn't wait for a velocity
    _setOutside(GeoZoneFencePrediction::breachedBoundary(fence, coordinate, altitude), msecs);
    const bool moving = velocity.valid && ((velocity.groundSpeed() >= GeoZoneAlertTracker::minPredictionSpeed) ||
                                           (std::abs(velocity.up) >= GeoZoneAlertTracker::minPredictionSpeed));
    if (!moving) {
        _setBreach(std::nullopt);
        return;
    }
    // Ends a little after it would start, so that it doesn't flicker
    _setBreach(GeoZoneFencePrediction::predict(fence, coordinate, altitude, velocity, active() ? (_warningTime + endDelay) : _warningTime));
}

void GeoZoneFenceWarning::_setOutside(const std::optional<GeoZoneFencePrediction::Boundary> &boundary, qint64 msecs)
{
    if (boundary) {
        _insideMSecs = -1;
        if (boundary != _outside) {
            _outside = boundary;
            emit outsideChanged();
        }
        return;
    }
    if (!_outside) {
        return;
    }
    if ((msecs >= 0) && (_insideMSecs < 0)) {
        _insideMSecs = msecs;
    }
    if ((msecs < 0) || ((msecs - _insideMSecs) >= outsideEndDelayMSecs)) {
        _outside.reset();
        _insideMSecs = -1;
        emit outsideChanged();
    }
}

void GeoZoneFenceWarning::_setBreach(const std::optional<GeoZoneFencePrediction::Breach> &breach)
{
    const bool wasActive = active();
    if (!breach && !wasActive) {
        return;
    }
    _breach = breach;
    if (!wasActive && breach) {
        qCDebug(GeoZoneMonitorLog) << "Fence breach predicted:" << text();
        emit warningStarted(text());
    }
    emit warningChanged();
    if (wasActive && !breach) {
        emit warningEnded();
    }
}

QString GeoZoneFenceWarning::warningText(const QString &action, bool manoeuvre, double time, double distance)
{
    const QString units = FactMetaData::appSettingsHorizontalDistanceUnitsString();
    const QString distanceText = QStringLiteral("%1 %2").arg(qRound(FactMetaData::metersToAppSettingsHorizontalDistanceUnits(distance).toDouble())).arg(units);
    if (!manoeuvre) {
        return tr("Geofence in %1 s, %2 ahead: %3").arg(std::lround(time)).arg(distanceText, action.toLower());
    }
    return tr("Geofence: %1 in %2 s, %3 ahead").arg(action).arg(std::lround(time)).arg(distanceText);
}

QString GeoZoneFenceWarning::text() const
{
    return _breach ? warningText(actionText(), isManoeuvre(_config.firmware, _config.action), _breach->time, _breach->distance) : QString();
}

int GeoZoneFenceWarning::timeToFence() const
{
    return _breach ? static_cast<int>(std::lround(_breach->time)) : -1;
}

QString GeoZoneFenceWarning::intersectionText() const
{
    if (!_breach) {
        return QString();
    }
    const QString coordinate = QStringLiteral("%1, %2").arg(QLocale().toString(_breach->coordinate.latitude(), 'f', 6), QLocale().toString(_breach->coordinate.longitude(), 'f', 6));
    if (std::isnan(_breach->altitude)) {
        return coordinate;
    }
    return tr("%1 at %2 %3 above home").arg(coordinate)
        .arg(qRound(FactMetaData::metersToAppSettingsVerticalDistanceUnits(_breach->altitude).toDouble()))
        .arg(FactMetaData::appSettingsVerticalDistanceUnitsString());
}

QVariantList GeoZoneFenceWarning::path() const
{
    if (!_breach || !_position.isValid()) {
        return QVariantList();
    }
    return QVariantList{ QVariant::fromValue(_position), QVariant::fromValue(_breach->coordinate) };
}

bool GeoZoneFenceWarning::isManoeuvre(Firmware firmware, int action)
{
    switch (firmware) {
    case Firmware::PX4:
        return action >= 2;
    case Firmware::ArduPilot:
        return action > 0;
    case Firmware::Other:
        break;
    }
    return false;
}

QString GeoZoneFenceWarning::actionText(Firmware firmware, VehicleClass vehicleClass, int action)
{
    if (action < 0) {
        return tr("Fence action unknown");
    }
    if (firmware == Firmware::PX4) {
        switch (action) {
        case 0:
            return tr("No action");
        case 1:
            return tr("Warning only, no manoeuvre");
        case 2:
            return tr("Hold");
        case 3:
            return tr("Return");
        case 4:
            return tr("Flight termination");
        case 5:
            return tr("Land");
        default:
            break;
        }
    } else if (firmware == Firmware::ArduPilot) {
        if (action == 0) {
            return tr("Report only, no manoeuvre");
        }
        switch (vehicleClass) {
        case VehicleClass::Copter:
            switch (action) {
            case 1:
                return tr("RTL or land");
            case 2:
                return tr("Land");
            case 3:
                return tr("SmartRTL, RTL or land");
            case 4:
                return tr("Brake or land");
            case 5:
                return tr("SmartRTL or land");
            default:
                break;
            }
            break;
        case VehicleClass::Plane:
            switch (action) {
            case 1:
                return tr("RTL");
            case 6:
                return tr("Guided to the return point");
            case 7:
                return tr("Guided to the return point, pilot throttle");
            case 8:
                return tr("Autoland or RTL");
            default:
                break;
            }
            break;
        case VehicleClass::Rover:
            switch (action) {
            case 1:
                return tr("RTL or hold");
            case 2:
                return tr("Hold");
            case 3:
                return tr("SmartRTL, RTL or hold");
            case 4:
                return tr("SmartRTL or hold");
            case 5:
                return tr("Terminate");
            case 6:
                return tr("Loiter or hold");
            default:
                break;
            }
            break;
        case VehicleClass::Sub:
            if (action == 1) {
                return tr("RTL or land");
            }
            break;
        }
    }
    return tr("Fence action %1").arg(action);
}

QString GeoZoneFenceWarning::overrideText(const Config &config)
{
    switch (config.firmware) {
    case Firmware::PX4:
        switch (config.action) {
        case 0:
        case 1:
            return tr("The vehicle only warns, it takes no manoeuvre.");
        case 2:
            return tr("Switch the flight mode, with the RC mode switch or Take Control, to override the hold. Moving the sticks doesn't.");
        case 4:
            return tr("Flight termination can't be overridden.");
        default:
            return tr("Switch the flight mode, with the RC mode switch or Take Control, to override it. The vehicle acts again only after it leaves the breach and breaches the fence again.");
        }
    case Firmware::ArduPilot:
        if (config.action == 0) {
            return tr("The vehicle only reports the breach, it takes no manoeuvre.");
        }
        if (config.modeChangeBlocked) {
            return tr("The vehicle refuses flight mode changes until the breach clears (FENCE_OPTIONS): disable the fence, with its RC switch or Disable Fence, to take control.");
        }
        return tr("Switch the flight mode, with the RC mode switch or Take Control, to override it: the vehicle then ignores breaches for 10 s and acts again only on a new breach, "
                  "or 20 m beyond an altitude or circle fence. Disabling the fence, with its RC switch or Disable Fence, stops it altogether.");
    case Firmware::Other:
        break;
    }
    return tr("Switch the flight mode to override it.");
}

QString GeoZoneFenceWarning::takeControlMode(Firmware firmware, VehicleClass vehicleClass, const QStringList &flightModes)
{
    // The names of QGroundControl's firmware plugins, then those of autopilots reporting their modes (AVAILABLE_MODES),
    // like ArduPilot's "LOITER", compared without case
    QStringList candidates;
    if (firmware == Firmware::PX4) {
        switch (vehicleClass) {
        case VehicleClass::Copter:
        case VehicleClass::Plane:
            candidates = { QStringLiteral("Position"), QStringLiteral("Altitude"), QStringLiteral("Stabilized") };
            break;
        case VehicleClass::Rover:
        case VehicleClass::Sub:
            candidates = { QStringLiteral("Position"), QStringLiteral("Manual") };
            break;
        }
    } else if (firmware == Firmware::ArduPilot) {
        switch (vehicleClass) {
        case VehicleClass::Copter:
            candidates = { QStringLiteral("Loiter"), QStringLiteral("Position Hold"), QStringLiteral("PosHold"), QStringLiteral("QuadPlane Loiter"),
                           QStringLiteral("QLoiter"), QStringLiteral("QuadPlane Hover"), QStringLiteral("QHover"), QStringLiteral("Altitude Hold"),
                           QStringLiteral("AltHold") };
            break;
        case VehicleClass::Plane:
            candidates = { QStringLiteral("FBW A"), QStringLiteral("FBWA"), QStringLiteral("Manual") };
            break;
        case VehicleClass::Rover:
            candidates = { QStringLiteral("Manual") };
            break;
        case VehicleClass::Sub:
            candidates = { QStringLiteral("Position Hold"), QStringLiteral("PosHold"), QStringLiteral("Manual") };
            break;
        }
    }
    // Generic standard modes, like those of simulators
    candidates.append({ QStringLiteral("Position"), QStringLiteral("Manual") });
    for (const QString &candidate : std::as_const(candidates)) {
        for (const QString &mode : flightModes) {
            if (mode.compare(candidate, Qt::CaseInsensitive) == 0) {
                return mode;
            }
        }
    }
    return QString();
}

void GeoZoneFenceWarning::_setCommandText(const QString &text)
{
    if (text != _commandText) {
        _commandText = text;
        emit commandTextChanged();
    }
}

void GeoZoneFenceWarning::takeControl()
{
    Vehicle *const vehicle = _vehicle;
    const QString mode = _config.takeControlMode;
    if (!vehicle || mode.isEmpty()) {
        _setCommandText(tr("No flight mode to take control in."));
        return;
    }
    qCInfo(GeoZoneMonitorLog) << "Pilot takes control from the fence action:" << mode;
    _pendingMode = mode;
    _setCommandText(tr("Switching to %1\u2026").arg(mode));
    // The vehicle reports the mode with its next heartbeat, or refuses it, like ArduPilot in fence recovery
    _modeTimer.start();
    vehicle->setFlightMode(mode);
}

void GeoZoneFenceWarning::_modeSwitched(bool timedOut)
{
    const Vehicle *const vehicle = _vehicle;
    if (_pendingMode.isEmpty() || !vehicle) {
        return;
    }
    const bool switched = vehicle->flightMode() == _pendingMode;
    if (!switched && !timedOut) {
        // Another mode, like the one of the fence action, on the way
        return;
    }
    const QString mode = _pendingMode;
    _pendingMode.clear();
    _modeTimer.stop();
    _setCommandText(switched ? tr("%1: you have control, the fence action is overridden.").arg(mode) : tr("The vehicle didn't switch to %1.").arg(mode));
}

void GeoZoneFenceWarning::disableFence()
{
    _sendFenceEnable(false);
}

void GeoZoneFenceWarning::enableFence()
{
    _sendFenceEnable(true);
}

void GeoZoneFenceWarning::_sendFenceEnable(bool enable)
{
    Vehicle *const vehicle = _vehicle;
    if (!vehicle || !canDisableFence()) {
        _setCommandText(tr("The vehicle can't disable its fence in flight."));
        return;
    }
    if (_pendingFenceEnable) {
        // One at a time, since the answer doesn't tell which of the two it is for
        return;
    }
    const VehicleLinkManager *const links = vehicle->vehicleLinkManager();
    if (!links || !links->primaryLink().lock()) {
        _setCommandText(tr("The vehicle isn't connected."));
        return;
    }
    qCInfo(GeoZoneMonitorLog) << "Pilot" << (enable ? "enables" : "disables") << "the fence";
    _pendingFenceEnable = enable;
    _unansweredFenceEnable.reset();
    _setCommandText(enable ? tr("Enabling the fence\u2026") : tr("Disabling the fence\u2026"));
    // Without a link the command gets no result, not even that it wasn't answered
    _fenceCommandTimer.start();
    vehicle->sendMavCommand(vehicle->defaultComponentId(), MAV_CMD_DO_FENCE_ENABLE, false /* showError */, enable ? 1.0f : 0.0f);
}

void GeoZoneFenceWarning::_commandResult(int command, int ackResult, int failureCode)
{
    const std::optional<bool> sent = _pendingFenceEnable ? _pendingFenceEnable : _unansweredFenceEnable;
    if ((command != MAV_CMD_DO_FENCE_ENABLE) || !sent) {
        return;
    }
    const bool enable = *sent;
    _pendingFenceEnable.reset();
    _unansweredFenceEnable.reset();
    _fenceCommandTimer.stop();
    QString text;
    if ((failureCode == Vehicle::MavCmdResultCommandResultOnly) && (ackResult == MAV_RESULT_ACCEPTED)) {
        text = enable ? tr("Fence enabled.") : tr("Fence disabled: the vehicle no longer acts on breaches until it is enabled again.");
        _setFenceDisabled(!enable);
    } else if ((failureCode == Vehicle::MavCmdResultCommandResultOnly) && (ackResult == MAV_RESULT_UNSUPPORTED)) {
        text = tr("The vehicle doesn't support enabling or disabling its fence.");
    } else if (failureCode == Vehicle::MavCmdResultCommandResultOnly) {
        text = enable ? tr("The vehicle refused to enable the fence.") : tr("The vehicle refused to disable the fence.");
    } else {
        text = tr("The vehicle didn't answer the fence command.");
    }
    _setCommandText(text);
}

void GeoZoneFenceWarning::_setFenceDisabled(bool disabled)
{
    if (disabled != _fenceDisabled) {
        _fenceDisabled = disabled;
        emit fenceDisabledChanged();
    }
}

void GeoZoneFenceWarning::_fenceCommandTimedOut()
{
    if (!_pendingFenceEnable) {
        return;
    }
    qCWarning(GeoZoneMonitorLog) << "No answer to the fence command";
    _unansweredFenceEnable = _pendingFenceEnable;
    _pendingFenceEnable.reset();
    _setCommandText(tr("The vehicle didn't answer the fence command."));
}
