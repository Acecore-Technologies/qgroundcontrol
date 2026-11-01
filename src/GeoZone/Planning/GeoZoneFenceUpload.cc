/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFenceUpload.h"
#include "GeoFenceController.h"
#include "GeoFenceManager.h"
#include "GeoZoneManager.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include <QtCore/QTimer>

namespace {

QString _countText(const GeoZoneFenceUpload::FenceSnapshot &fence)
{
    return GeoZoneFenceUpload::tr("%1 with %2 and %3", "Polygons with points and circles of a fence")
        .arg(GeoZoneFenceUpload::tr("%n polygon(s)", nullptr, static_cast<int>(fence.polygons.count())),
             GeoZoneFenceUpload::tr("%n point(s)", nullptr, fence.pointCount()),
             GeoZoneFenceUpload::tr("%n circle(s)", nullptr, static_cast<int>(fence.circles.count())));
}

} // namespace

GeoZoneFenceUpload::GeoZoneFenceUpload(QObject *parent)
    : QObject(parent)
{
}

bool GeoZoneFenceUpload::canTransition(State from, State to)
{
    const bool fromBusy = (from == Sending) || (from == Verifying);
    switch (to) {
    case Idle:
    case Sending:
        return !fromBusy;
    case Verifying:
        return from == Sending;
    case Verified:
    case Mismatch:
        return from == Verifying;
    case Failed:
        return true;
    case Aborted:
        return fromBusy;
    }
    return false;
}

bool GeoZoneFenceUpload::_transition(State state, const QString &message)
{
    if (!canTransition(_state, state)) {
        qCWarning(GeoZoneManagerLog) << "fence upload can't go from" << _state << "to" << state;
        return false;
    }
    if ((state != Sending) && (state != Verifying)) {
        if (_uploadConnections) {
            // Possibly from one of its connections
            (void) QObject::disconnect(nullptr, nullptr, _uploadConnections, nullptr);
            _uploadConnections->deleteLater();
            _uploadConnections = nullptr;
        }
        _vehicle = nullptr;
        _manager = nullptr;
    }
    _state = state;
    _message = message;
    qCDebug(GeoZoneManagerLog) << "fence upload" << state << message;
    emit stateChanged();
    return true;
}

Vehicle *GeoZoneFenceUpload::_planVehicle() const
{
    if (!_geoFenceController || !_geoFenceController->masterController()) {
        return nullptr;
    }
    return _geoFenceController->masterController()->managerVehicle();
}

void GeoZoneFenceUpload::setGeoFenceController(GeoFenceController *geoFenceController)
{
    if (geoFenceController == _geoFenceController) {
        return;
    }
    if (busy()) {
        abort(tr("The plan changed during the upload."));
    }
    _geoFenceController = geoFenceController;
    _connectController();
    if (_state != Idle) {
        (void) _transition(Idle, QString());
    }
    emit geoFenceControllerChanged();
    emit canUploadChanged();
}

void GeoZoneFenceUpload::_connectController()
{
    delete _controllerConnections;
    _controllerConnections = new QObject(this);
    if (!_geoFenceController || !_geoFenceController->masterController()) {
        return;
    }
    PlanMasterController *const masterController = _geoFenceController->masterController();
    const auto changed = [this]() { emit canUploadChanged(); };
    (void) connect(masterController, &PlanMasterController::managerVehicleChanged, _controllerConnections, changed);
    (void) connect(masterController, &PlanMasterController::offlineChanged, _controllerConnections, changed);
    (void) connect(_geoFenceController, &GeoFenceController::supportedChanged, _controllerConnections, changed);
}

bool GeoZoneFenceUpload::canUpload() const
{
    const Vehicle *const vehicle = _planVehicle();
    return vehicle && !vehicle->isOfflineEditingVehicle() && !_geoFenceController->masterController()->offline() && _geoFenceController->supported();
}

void GeoZoneFenceUpload::start()
{
    if (!_geoFenceController || busy()) {
        return;
    }
    PlanMasterController *const masterController = _geoFenceController->masterController();
    Vehicle *const vehicle = _planVehicle();
    if (!vehicle || vehicle->isOfflineEditingVehicle() || masterController->offline()) {
        (void) _transition(Failed, tr("No vehicle is connected."));
        return;
    }
    if (!_geoFenceController->supported()) {
        (void) _transition(Failed, tr("The vehicle doesn't support fences."));
        return;
    }
    if (_geoFenceController->syncInProgress()) {
        (void) _transition(Failed, tr("The vehicle is transferring a plan, try again when it is done."));
        return;
    }

    _vehicle = vehicle;
    _manager = vehicle->geoFenceManager();
    _planFile = masterController->currentPlanFile();
    _sentFence = snapshot(_geoFenceController);
    _error.clear();
    _ackResult = MAV_MISSION_ACCEPTED;
    (void) _transition(Sending, tr("Uploading the fence…"));

    _uploadConnections = new QObject(this);
    // The fence manager reports a rejected upload without the reason the vehicle gave
    (void) connect(vehicle, &Vehicle::mavlinkMessageReceived, _uploadConnections, [this](const mavlink_message_t &message) {
        if (message.msgid != MAVLINK_MSG_ID_MISSION_ACK) {
            return;
        }
        mavlink_mission_ack_t ack;
        mavlink_msg_mission_ack_decode(&message, &ack);
        if ((ack.mission_type == MAV_MISSION_TYPE_FENCE) && (ack.type != MAV_MISSION_ACCEPTED)) {
            _ackResult = ack.type;
        }
    });
    (void) connect(_manager, &GeoFenceManager::error, _uploadConnections, [this](int, const QString &message) { _error = message; });
    (void) connect(_manager, &GeoFenceManager::sendComplete, _uploadConnections, [this](bool error) { _sent(error); });
    (void) connect(_manager, &GeoFenceManager::loadComplete, _uploadConnections, [this]() { _readBack(); });
    (void) connect(vehicle, &QObject::destroyed, _uploadConnections, [this]() { _vehicleLost(); });
    (void) connect(masterController, &PlanMasterController::managerVehicleChanged, _uploadConnections, [this]() { _vehicleLost(); });
    (void) connect(masterController, &PlanMasterController::currentPlanFileChanged, _uploadConnections, [this, masterController]() {
        if (masterController->currentPlanFile() != _planFile) {
            abort(tr("The plan file changed during the upload, upload the fence again."));
        }
    });
    // Edits of the fence. Sending clears the dirty state of the fences, so that the first edit of each sets it again.
    const auto edited = [this]() { _checkFence(); };
    (void) connect(_geoFenceController, &QObject::destroyed, _uploadConnections, [this]() { abort(tr("The plan was closed during the upload.")); });
    (void) connect(_geoFenceController, &GeoFenceController::dirtyChanged, _uploadConnections, edited);
    (void) connect(_geoFenceController, &GeoFenceController::breachReturnPointChanged, _uploadConnections, edited);
    for (QmlObjectListModel *const fences : { _geoFenceController->polygons(), _geoFenceController->circles() }) {
        (void) connect(fences, &QmlObjectListModel::countChanged, _uploadConnections, edited);
        (void) connect(fences, &QmlObjectListModel::dirtyChanged, _uploadConnections, edited);
    }

    _geoFenceController->sendToVehicle();
}

void GeoZoneFenceUpload::abort(const QString &reason)
{
    if (busy()) {
        (void) _transition(Aborted, reason);
    }
}

void GeoZoneFenceUpload::_checkFence()
{
    if (busy() && (!_geoFenceController || (snapshot(_geoFenceController) != _sentFence))) {
        abort(tr("The plan's fence changed during the upload, upload it again."));
    }
}

void GeoZoneFenceUpload::_vehicleLost()
{
    if (busy() && (!_vehicle || !_manager || (_planVehicle() != _vehicle))) {
        (void) _transition(Failed, tr("The vehicle disconnected."));
    }
}

QString GeoZoneFenceUpload::_rejectionText(const QString &errorMessage) const
{
    const int points = _sentFence.pointCount() + _sentFence.circles.count() + (_sentFence.returnPoint.isValid() ? 1 : 0);
    switch (_ackResult) {
    case MAV_MISSION_NO_SPACE:
        return tr("The vehicle has no room for this fence of %n point(s). Leave out zones or allow more simplification.", nullptr, points);
    case MAV_MISSION_ERROR:
        if (_vehicle && _vehicle->apmFirmware()) {
            return tr("The vehicle rejected the fence. ArduPilot does when the fence exceeds its storage, leave out zones or allow more simplification.");
        }
        break;
    case MAV_MISSION_DENIED:
        return tr("The vehicle doesn't accept fence changes now.");
    default:
        break;
    }
    return errorMessage.isEmpty() ? tr("The fence upload failed.") : tr("The fence upload failed: %1").arg(errorMessage);
}

void GeoZoneFenceUpload::_sent(bool error)
{
    if (_state != Sending) {
        return;
    }
    if (error) {
        (void) _transition(Failed, _rejectionText(_error));
        return;
    }
    // Edits while sending leave the fence dirty, they aren't on the vehicle
    _checkFence();
    if (_state != Sending) {
        return;
    }
    // Like the plan upload once it is done, since clearing the fence's dirty state while sending leaves it set again
    // by the fences which were dirty
    _geoFenceController->setDirty(false);
    (void) _transition(Verifying, tr("Reading the fence back from the vehicle…"));
    _error.clear();
    // After the fence manager has finished the upload transaction
    QTimer::singleShot(0, _uploadConnections, [this]() {
        if (_state != Verifying) {
            return;
        }
        if (!_manager) {
            _vehicleLost();
            return;
        }
        _manager->loadFromVehicle();
    });
}

void GeoZoneFenceUpload::_readBack()
{
    if (_state != Verifying) {
        return;
    }
    if (!_error.isEmpty()) {
        (void) _transition(Failed, tr("The fence was sent, but reading it back failed: %1").arg(_error));
        return;
    }
    if (!_manager) {
        _vehicleLost();
        return;
    }
    // The vehicle stores the fence sent, which the plan no longer has
    _checkFence();
    if (_state != Verifying) {
        return;
    }
    const QString differences = compareFences(_sentFence, snapshot(_manager->polygons(), _manager->circles(), _manager->breachReturnPoint()));
    if (differences.isEmpty()) {
        (void) _transition(Verified, tr("Verified: the vehicle stores the fence of %1.").arg(_countText(_sentFence)));
    } else {
        (void) _transition(Mismatch, tr("The fence read back differs from the plan. %1").arg(differences));
    }
}

int GeoZoneFenceUpload::FenceSnapshot::pointCount() const
{
    int points = 0;
    for (const Polygon &polygon : polygons) {
        points += polygon.vertices.count();
    }
    return points;
}

GeoZoneFenceUpload::FenceSnapshot GeoZoneFenceUpload::snapshot(GeoFenceController *geoFenceController)
{
    FenceSnapshot fence;
    QmlObjectListModel *const polygons = geoFenceController->polygons();
    for (int i = 0; i < polygons->count(); i++) {
        const QGCFencePolygon *const polygon = polygons->value<QGCFencePolygon*>(i);
        fence.polygons.append({ polygon->inclusion(), polygon->coordinateList() });
    }
    QmlObjectListModel *const circles = geoFenceController->circles();
    for (int i = 0; i < circles->count(); i++) {
        QGCFenceCircle *const circle = circles->value<QGCFenceCircle*>(i);
        fence.circles.append({ circle->inclusion(), QGeoCircle(circle->center(), circle->radius()->rawValue().toDouble()) });
    }
    fence.returnPoint = geoFenceController->breachReturnPoint();
    return fence;
}

GeoZoneFenceUpload::FenceSnapshot GeoZoneFenceUpload::snapshot(const QList<QGCFencePolygon> &polygons, const QList<QGCFenceCircle> &circles, const QGeoCoordinate &returnPoint)
{
    FenceSnapshot fence;
    for (const QGCFencePolygon &polygon : polygons) {
        fence.polygons.append({ polygon.inclusion(), polygon.coordinateList() });
    }
    for (const QGCFenceCircle &circle : circles) {
        fence.circles.append({ circle.inclusion(), QGeoCircle(circle.center(), const_cast<QGCFenceCircle&>(circle).radius()->rawValue().toDouble()) });
    }
    fence.returnPoint = returnPoint;
    return fence;
}

QString GeoZoneFenceUpload::compareFences(const FenceSnapshot &sent, const FenceSnapshot &received)
{
    const auto near = [](const QGeoCoordinate &a, const QGeoCoordinate &b) {
        return a.isValid() && b.isValid() && (QGeoCoordinate(a.latitude(), a.longitude()).distanceTo(QGeoCoordinate(b.latitude(), b.longitude())) <= verifyTolerance);
    };
    QList<bool> polygonMatched(received.polygons.count(), false);
    QList<bool> circleMatched(received.circles.count(), false);
    int unmatchedPolygons = 0;
    for (const FenceSnapshot::Polygon &polygon : sent.polygons) {
        bool found = false;
        for (qsizetype i = 0; (i < received.polygons.count()) && !found; i++) {
            const FenceSnapshot::Polygon &other = received.polygons[i];
            if (polygonMatched[i] || (other.inclusion != polygon.inclusion) || (other.vertices.count() != polygon.vertices.count())) {
                continue;
            }
            found = true;
            for (qsizetype v = 0; (v < polygon.vertices.count()) && found; v++) {
                found = near(polygon.vertices[v], other.vertices[v]);
            }
            polygonMatched[i] = found;
        }
        unmatchedPolygons += found ? 0 : 1;
    }
    int unmatchedCircles = 0;
    for (const FenceSnapshot::Circle &circle : sent.circles) {
        bool found = false;
        for (qsizetype i = 0; (i < received.circles.count()) && !found; i++) {
            const FenceSnapshot::Circle &other = received.circles[i];
            found = !circleMatched[i] && (other.inclusion == circle.inclusion) && near(circle.circle.center(), other.circle.center()) &&
                    (qAbs(circle.circle.radius() - other.circle.radius()) <= verifyTolerance);
            circleMatched[i] = found;
        }
        unmatchedCircles += found ? 0 : 1;
    }

    QStringList differences;
    if ((sent.polygons.count() != received.polygons.count()) || (sent.circles.count() != received.circles.count())) {
        differences.append(tr("The vehicle has %1 instead of %2.").arg(_countText(received), _countText(sent)));
    } else if ((unmatchedPolygons > 0) || (unmatchedCircles > 0)) {
        differences.append(tr("Fences differing from the plan: %1.").arg(unmatchedPolygons + unmatchedCircles));
    }
    if (sent.returnPoint.isValid() != received.returnPoint.isValid()) {
        differences.append(sent.returnPoint.isValid() ? tr("The vehicle has no breach return point.") : tr("The vehicle has a breach return point the plan doesn't."));
    } else if (sent.returnPoint.isValid() && !near(sent.returnPoint, received.returnPoint)) {
        differences.append(tr("The breach return point on the vehicle differs from the plan."));
    }
    return differences.join(QLatin1Char(' '));
}
