/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneConformance.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneGeoid.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMissionReader.h"
#include "GeoZonePlanning.h"
#include "GeoZoneVehicleMonitor.h"
#include "MissionController.h"
#include "MultiVehicleManager.h"
#include "PlanMasterController.h"
#include "Vehicle.h"

#include <QtCore/QPointF>
#include <QtCore/QtMath>
#include <QtCore/qapplicationstatic.h>

#include <algorithm>
#include <cmath>
#include <limits>

Q_APPLICATION_STATIC(GeoZoneConformance, _geoZoneConformance);

namespace {

constexpr int volumesDelayMSecs = 1000;
constexpr double earthRadius = 6371000;

GeoZoneConformance *applicationInstance = nullptr;

QPointer<PlanMasterController> &flyViewPlan()
{
    static QPointer<PlanMasterController> plan;
    return plan;
}

bool outside(GeoZoneConformance::State state)
{
    return (state != GeoZoneConformance::Off) && (state != GeoZoneConformance::Waiting) && (state != GeoZoneConformance::Conforming);
}

/// @return How far outside, like "40 m above it", empty when inside
QString detailText(const GeoZoneConformance::Result &result)
{
    const long meters = std::isfinite(result.distance) ? std::lround(result.distance) : -1;
    switch (result.state) {
    case GeoZoneConformance::OutsideHorizontally:
        return (meters >= 0) ? GeoZoneConformance::tr("%1 m beside it").arg(meters) : GeoZoneConformance::tr("beside it");
    case GeoZoneConformance::Above:
        return GeoZoneConformance::tr("%1 m above it").arg(meters);
    case GeoZoneConformance::Below:
        return GeoZoneConformance::tr("%1 m below it").arg(meters);
    case GeoZoneConformance::OutsideTime:
        return GeoZoneConformance::tr("outside its time window");
    case GeoZoneConformance::Off:
    case GeoZoneConformance::Waiting:
    case GeoZoneConformance::Conforming:
        break;
    }
    return QString();
}

} // namespace

GeoZoneConformance::GeoZoneConformance(QObject *parent)
    : GeoZoneConformance(GeoZoneManager::instance(), GeoZoneMonitor::instance(), parent)
{
    _application = true;
    applicationInstance = this;
    _evaluateTimer.setInterval(GeoZoneMonitor::evaluateIntervalMSecs);
    (void) connect(&_evaluateTimer, &QTimer::timeout, this, &GeoZoneConformance::_evaluateActiveVehicle);
    setPlanController(flyViewPlan());
}

GeoZoneConformance::GeoZoneConformance(GeoZoneManager *manager, GeoZoneMonitor *monitor, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _monitor(monitor)
{
    _volumesTimer.setSingleShot(true);
    _volumesTimer.setInterval(volumesDelayMSecs);
    (void) connect(&_volumesTimer, &QTimer::timeout, this, &GeoZoneConformance::updateVolumes);
    (void) connect(_manager->planning(), &GeoZonePlanning::plannedStartChanged, this, &GeoZoneConformance::_scheduleVolumes);
}

GeoZoneConformance::~GeoZoneConformance()
{
    if (applicationInstance == this) {
        applicationInstance = nullptr;
    }
}

GeoZoneConformance *GeoZoneConformance::instance()
{
    return _geoZoneConformance();
}

void GeoZoneConformance::followFlyViewPlan(PlanMasterController *planController)
{
    flyViewPlan() = planController;
    if (applicationInstance) {
        applicationInstance->setPlanController(planController);
    }
}

PlanMasterController *GeoZoneConformance::planController() const
{
    return _planController;
}

void GeoZoneConformance::setPlanController(PlanMasterController *planController)
{
    if (planController == _planController) {
        return;
    }
    _planController = planController;
    _connectPlan();
    _scheduleVolumes();
}

void GeoZoneConformance::_connectPlan()
{
    delete _planConnections;
    _planConnections = new QObject(this);
    if (!_planController) {
        return;
    }
    MissionController *const missionController = _planController->missionController();
    (void) connect(missionController, &MissionController::visualItemsChanged, _planConnections, [this]() { _scheduleVolumes(); });
    (void) connect(missionController, &MissionController::plannedHomePositionChanged, _planConnections, [this]() { _scheduleVolumes(); });
    (void) connect(_planController, &PlanMasterController::managerVehicleChanged, _planConnections, [this]() { _scheduleVolumes(); });
}

void GeoZoneConformance::_scheduleVolumes()
{
    if (!_fixedVolumes) {
        _volumesTimer.start();
    }
}

void GeoZoneConformance::setVolumes(const QList<Volume> &volumes)
{
    _fixedVolumes = true;
    _volumesTimer.stop();
    _volumes = volumes;
    emit volumesChanged();
    _updateEvaluateTimer();
}

void GeoZoneConformance::updateVolumes()
{
    _volumesTimer.stop();
    if (_fixedVolumes) {
        return;
    }
    QList<Volume> volumes;
    MissionController *const missionController = _planController ? _planController->missionController() : nullptr;
    const auto [start, end] = GeoZoneMissionCheck::plannedFlightTimes(_manager, missionController);
    if (missionController && start.isValid()) {
        // Like the export of the operational intent
        const GeoZoneMissionPath path = GeoZoneMissionReader::path(missionController);
        QString errorString;
        if (const std::optional<QList<Volume>> built = GeoZoneOperationalIntent::buildVolumes(path.evaluatorPaths(start, end), GeoZoneOperationalIntent::Parameters(), errorString)) {
            volumes = *built;
        } else {
            qCDebug(GeoZoneMonitorLog) << "No operational intent to monitor:" << errorString;
        }
    }
    _volumes = volumes;
    emit volumesChanged();
    if (_volumes.isEmpty()) {
        _setResult(Result());
    }
    _updateEvaluateTimer();
}

void GeoZoneConformance::_updateEvaluateTimer()
{
    if (!_application) {
        return;
    }
    if (_volumes.isEmpty()) {
        _evaluateTimer.stop();
        _setResult(Result());
    } else if (!_evaluateTimer.isActive()) {
        _evaluateTimer.start();
        _evaluateActiveVehicle();
    }
}

void GeoZoneConformance::_evaluateActiveVehicle()
{
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    if (!vehicle) {
        _setResult(Result{ _volumes.isEmpty() ? Off : Waiting, qQNaN() });
        return;
    }
    const QGeoCoordinate coordinate = vehicle->coordinate();
    evaluate(GeoZoneVehicleMonitor::vehiclePosition(vehicle, coordinate), vehicle->flying() && coordinate.isValid(), vehicle->id());
}

void GeoZoneConformance::evaluate(const GeoZone::Position &position, bool flying, int vehicleId)
{
    Result result;
    if (!_volumes.isEmpty()) {
        result = flying ? check(_volumes, position, GeoZoneEnvironment::currentDateTimeUtc()) : Result{ Waiting, qQNaN() };
    }
    // Once when the vehicle leaves the volumes, again after it conformed
    if (outside(result.state) && !outside(_result.state) && _monitor) {
        (void) _monitor->raiseVehicleAlert(GeoZoneMonitor::AlertEvent::NonConformance, vehicleId, position, result.distance, stateCode(result.state), detailText(result));
    }
    _setResult(result);
}

void GeoZoneConformance::_setResult(const Result &result)
{
    const auto meters = [](double distance) { return std::isfinite(distance) ? std::lround(distance) : -1L; };
    if ((result.state == _result.state) && (meters(result.distance) == meters(_result.distance))) {
        return;
    }
    _result = result;
    emit stateChanged();
}

GeoZoneConformance::Result GeoZoneConformance::check(const QList<Volume> &volumes, const GeoZone::Position &position, const QDateTime &time)
{
    return check(volumes, position, time, GeoZoneGeometry::geoidHeight(position.coordinate));
}

GeoZoneConformance::Result GeoZoneConformance::check(const QList<Volume> &volumes, const GeoZone::Position &position, const QDateTime &time, double geoidHeight)
{
    if (volumes.isEmpty()) {
        return Result();
    }
    QList<const Volume*> current;
    for (const Volume &volume : volumes) {
        if ((time >= volume.timeStart) && (time <= volume.timeEnd)) {
            current.append(&volume);
        }
    }
    if (current.isEmpty()) {
        return Result{ OutsideTime, qQNaN() };
    }

    QList<const Volume*> over;
    double beside = std::numeric_limits<double>::infinity();
    for (const Volume *volume : std::as_const(current)) {
        const double distance = horizontalDistance(*volume, position.coordinate);
        if (distance <= 0) {
            over.append(volume);
        } else {
            beside = (std::min)(beside, distance);
        }
    }
    if (over.isEmpty()) {
        return Result{ OutsideHorizontally, beside };
    }
    // The volumes are above the WGS84 ellipsoid. Without the altitude of the vehicle, or the height of the geoid to
    // compare it with, the vehicle can't be told to be above or below them.
    const double altitude = position.altitudeAMSL + geoidHeight;
    if (!std::isfinite(altitude)) {
        return Result{ Conforming, 0 };
    }
    double above = std::numeric_limits<double>::infinity();
    double below = std::numeric_limits<double>::infinity();
    for (const Volume *volume : std::as_const(over)) {
        if ((altitude >= volume->altitudeLower) && (altitude <= volume->altitudeUpper)) {
            return Result{ Conforming, 0 };
        }
        if (altitude > volume->altitudeUpper) {
            above = (std::min)(above, altitude - volume->altitudeUpper);
        } else {
            below = (std::min)(below, volume->altitudeLower - altitude);
        }
    }
    return (above <= below) ? Result{ Above, above } : Result{ Below, below };
}

double GeoZoneConformance::horizontalDistance(const Volume &volume, const QGeoCoordinate &coordinate)
{
    if (volume.isCircle()) {
        return (std::max)(volume.circleCenter.distanceTo(coordinate) - volume.circleRadius, 0.0);
    }
    if (volume.contains(coordinate) || volume.outline.isEmpty()) {
        return 0;
    }
    // In a plane tangent at the coordinate, precise enough near the volume
    const double cosLatitude = std::cos(qDegreesToRadians(coordinate.latitude()));
    const auto project = [&coordinate, cosLatitude](const QGeoCoordinate &vertex) {
        double longitude = vertex.longitude() - coordinate.longitude();
        longitude -= 360 * std::round(longitude / 360);
        return QPointF(qDegreesToRadians(longitude) * earthRadius * cosLatitude, qDegreesToRadians(vertex.latitude() - coordinate.latitude()) * earthRadius);
    };
    double nearest = std::numeric_limits<double>::infinity();
    for (qsizetype i = 0; i < volume.outline.count(); i++) {
        const QPointF a = project(volume.outline[i]);
        const QPointF b = project(volume.outline[(i + 1) % volume.outline.count()]);
        const QPointF edge = b - a;
        const double lengthSquared = QPointF::dotProduct(edge, edge);
        const double fraction = (lengthSquared > 0) ? std::clamp(-QPointF::dotProduct(a, edge) / lengthSquared, 0.0, 1.0) : 0.0;
        const QPointF closest = a + (edge * fraction);
        nearest = (std::min)(nearest, std::hypot(closest.x(), closest.y()));
    }
    return nearest;
}

QString GeoZoneConformance::stateCode(State state)
{
    switch (state) {
    case OutsideHorizontally:
        return QStringLiteral("horizontal");
    case Above:
        return QStringLiteral("above");
    case Below:
        return QStringLiteral("below");
    case OutsideTime:
        return QStringLiteral("time");
    case Off:
    case Waiting:
    case Conforming:
        break;
    }
    return QString();
}

QString GeoZoneConformance::stateText(const Result &result)
{
    switch (result.state) {
    case Off:
        return tr("No planned volume: set the planned start of the mission");
    case Waiting:
        return tr("Planned volume checked in flight");
    case Conforming:
        return tr("Inside planned volume");
    case OutsideHorizontally:
    case Above:
    case Below:
    case OutsideTime:
        break;
    }
    return tr("Outside planned volume: %1").arg(detailText(result));
}
