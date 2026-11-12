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
#include "GeoZoneEnvironment.h"
#include "GeoZoneFenceTags.h"
#include "GeoZoneManager.h"
#include "GeoZoneSourceRepository.h"
#include "PlanMasterController.h"
#include "QGCApplication.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QTimer>

#include <algorithm>
#include <cmath>

namespace {

// Keys of the file of recorded fences and of its records
constexpr int recordsVersion = 1;
constexpr const char *versionKey = "version";
constexpr const char *vehiclesKey = "vehicles";
constexpr const char *uidKey = "uid";
constexpr const char *systemIdKey = "systemId";
constexpr const char *timeKey = "time";
constexpr const char *verifiedKey = "verified";
constexpr const char *geoZoneFencesKey = "geoZoneFences";
constexpr const char *sourcesKey = "sources";
constexpr const char *notFencedKey = "notFenced";
constexpr const char *countKey = "count";
constexpr const char *zonesKey = "zones";
constexpr const char *nameKey = "name";
constexpr const char *reasonKey = "reason";
constexpr const char *fenceKey = "fence";
constexpr const char *polygonsKey = "polygons";
constexpr const char *circlesKey = "circles";
constexpr const char *inclusionKey = "inclusion";
constexpr const char *verticesKey = "vertices";
constexpr const char *centerKey = "center";
constexpr const char *radiusKey = "radius";
constexpr const char *returnPointKey = "returnPoint";

QJsonArray _coordinateJson(const QGeoCoordinate &coordinate)
{
    return QJsonArray{ coordinate.latitude(), coordinate.longitude() };
}

/// @return The coordinate of a [latitude, longitude] array, invalid for anything else
QGeoCoordinate _coordinateFromJson(const QJsonValue &json)
{
    const QJsonArray values = json.toArray();
    if ((values.count() < 2) || !values[0].isDouble() || !values[1].isDouble()) {
        return QGeoCoordinate();
    }
    return QGeoCoordinate(values[0].toDouble(), values[1].toDouble());
}

QJsonObject _fenceJson(const GeoZoneFenceUpload::FenceSnapshot &fence)
{
    QJsonArray polygons;
    for (const GeoZoneFenceUpload::FenceSnapshot::Polygon &polygon : fence.polygons) {
        QJsonArray vertices;
        for (const QGeoCoordinate &vertex : polygon.vertices) {
            vertices.append(_coordinateJson(vertex));
        }
        polygons.append(QJsonObject{ { inclusionKey, polygon.inclusion }, { verticesKey, vertices } });
    }
    QJsonArray circles;
    for (const GeoZoneFenceUpload::FenceSnapshot::Circle &circle : fence.circles) {
        circles.append(QJsonObject{ { inclusionKey, circle.inclusion }, { centerKey, _coordinateJson(circle.circle.center()) }, { radiusKey, circle.circle.radius() } });
    }
    QJsonObject json{ { polygonsKey, polygons }, { circlesKey, circles } };
    if (fence.returnPoint.isValid()) {
        json.insert(returnPointKey, _coordinateJson(fence.returnPoint));
    }
    return json;
}

/// @return The fence, nothing when a vertex, center or radius can't be read: a fence read in part would compare with
///         the vehicle's as another fence
std::optional<GeoZoneFenceUpload::FenceSnapshot> _fenceFromJson(const QJsonObject &json)
{
    GeoZoneFenceUpload::FenceSnapshot fence;
    for (const QJsonValue &value : json.value(polygonsKey).toArray()) {
        GeoZoneFenceUpload::FenceSnapshot::Polygon polygon;
        polygon.inclusion = value[inclusionKey].toBool();
        for (const QJsonValue &vertex : value[verticesKey].toArray()) {
            polygon.vertices.append(_coordinateFromJson(vertex));
            if (!polygon.vertices.constLast().isValid()) {
                return std::nullopt;
            }
        }
        fence.polygons.append(polygon);
    }
    for (const QJsonValue &value : json.value(circlesKey).toArray()) {
        const QGeoCoordinate center = _coordinateFromJson(value[centerKey]);
        const double radius = value[radiusKey].toDouble(qQNaN());
        if (!center.isValid() || !std::isfinite(radius)) {
            return std::nullopt;
        }
        fence.circles.append({ value[inclusionKey].toBool(), QGeoCircle(center, radius) });
    }
    if (json.contains(returnPointKey)) {
        fence.returnPoint = _coordinateFromJson(json.value(returnPointKey));
        if (!fence.returnPoint.isValid()) {
            return std::nullopt;
        }
    }
    return fence;
}

/// Fence recorded for a vehicle, which is told from the others by the unique id it reports, or by its system id
/// while one of them has none. Vehicles taken for each other only share a record while they store the same fence.
struct Record {
    quint64 uid = 0;
    int systemId = 0;
    GeoZoneFenceUpload::Verification verification;
    const Vehicle *vehicle = nullptr;   ///< It was made for, while that is connected. Only compared, never used.

    bool isOf(quint64 otherUid, int otherSystemId) const
    {
        return ((uid != 0) && (otherUid != 0)) ? (uid == otherUid) : (systemId == otherSystemId);
    }
};

/// The recorded fences, the most recent first, read from their file when first used and written when they change
class Records
{
public:
    void setFileName(const QString &fileName)
    {
        _fileName = fileName;
        _records.clear();
        _loaded = false;
    }

    const Record *find(quint64 uid, int systemId)
    {
        _load();
        const auto it = std::find_if(_records.cbegin(), _records.cend(), [uid, systemId](const Record &record) { return record.isOf(uid, systemId); });
        return (it != _records.cend()) ? &*it : nullptr;
    }

    void set(const Record &record)
    {
        _load();
        (void) _records.removeIf([&record](const Record &other) { return other.isOf(record.uid, record.systemId); });
        _records.prepend(record);
        if (_records.count() > GeoZoneFenceUpload::maxRecordedVehicles) {
            _records.resize(GeoZoneFenceUpload::maxRecordedVehicles);
        }
        _save();
    }

    void remove(quint64 uid, int systemId)
    {
        _load();
        if (_records.removeIf([uid, systemId](const Record &record) { return record.isOf(uid, systemId); }) > 0) {
            _save();
        }
    }

    /// The vehicle is gone: its address may be that of the next one
    void forget(const Vehicle *vehicle)
    {
        for (Record &record : _records) {
            if (record.vehicle == vehicle) {
                record.vehicle = nullptr;
            }
        }
    }

private:
    QString _path() const
    {
        return _fileName.isEmpty() ? QDir(GeoZoneManager::instance()->repository()->storageDir()).filePath(QStringLiteral("fences/vehicle-fences.json")) : _fileName;
    }

    void _load()
    {
        if (_loaded) {
            return;
        }
        _loaded = true;
        QFile file(_path());
        if (!file.open(QIODevice::ReadOnly)) {
            return;
        }
        const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
        // A file of a later version, or one which isn't of recorded fences, has no record this version can rely on:
        // the fences then read as not verified
        if (json.value(versionKey).toInt() != recordsVersion) {
            qCWarning(GeoZoneManagerLog) << "recorded vehicle fences not read, version" << json.value(versionKey) << file.fileName();
            return;
        }
        for (const QJsonValue &value : json.value(vehiclesKey).toArray()) {
            const QJsonObject entry = value.toObject();
            const std::optional<GeoZoneFenceUpload::Verification> verification = GeoZoneFenceUpload::Verification::fromJson(entry);
            if (verification) {
                _records.append(Record{ entry.value(uidKey).toString().toULongLong(nullptr, 16), entry.value(systemIdKey).toInt(), *verification, nullptr });
            }
        }
    }

    void _save() const
    {
        QJsonArray vehicles;
        for (const Record &record : _records) {
            QJsonObject entry = record.verification.toJson();
            entry.insert(uidKey, QString::number(record.uid, 16));
            entry.insert(systemIdKey, record.systemId);
            vehicles.append(entry);
        }
        const QString path = _path();
        QSaveFile file(path);
        if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly) ||
            (file.write(QJsonDocument(QJsonObject{ { versionKey, recordsVersion }, { vehiclesKey, vehicles } }).toJson(QJsonDocument::Compact)) < 0) || !file.commit()) {
            // Kept in memory for this session
            qCWarning(GeoZoneManagerLog) << "recorded vehicle fences not saved" << path << file.errorString();
        }
    }

    QString _fileName;
    QList<Record> _records;
    bool _loaded = false;
};

Records &records()
{
    static Records instance;
    return instance;
}

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
        // Possibly from one of its connections
        _uploadConnections.disconnect();
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
    _connectPlanVehicle();
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
    (void) connect(masterController, &PlanMasterController::managerVehicleChanged, _controllerConnections, [this]() { _connectPlanVehicle(); });
    (void) connect(masterController, &PlanMasterController::offlineChanged, _controllerConnections, changed);
    (void) connect(_geoFenceController, &GeoFenceController::supportedChanged, _controllerConnections, changed);
}

void GeoZoneFenceUpload::_connectPlanVehicle()
{
    delete _planVehicleConnections;
    _planVehicleConnections = new QObject(this);
    Vehicle *const vehicle = _planVehicle();
    if (!vehicle || vehicle->isOfflineEditingVehicle()) {
        return;
    }
    // Plan uploads send the fence without this object. What they sent is recorded, so that the pre-flight checklist
    // names the zones not fenced after them too.
    const QPointer<Vehicle> sentTo = vehicle;
    (void) connect(vehicle->geoFenceManager(), &GeoFenceManager::sendComplete, _planVehicleConnections, [this, sentTo](bool error) { _planFenceSent(sentTo, error); });
    (void) connect(vehicle->geoFenceManager(), &GeoFenceManager::removeAllComplete, _planVehicleConnections, [sentTo](bool error) {
        if (!error) {
            clearVerification(sentTo);
        }
    });
}

GeoZoneFenceUpload::Verification GeoZoneFenceUpload::_record(const FenceSnapshot &fence) const
{
    Verification record;
    record.fence = fence;
    record.time = GeoZoneEnvironment::currentDateTimeUtc();
    for (QmlObjectListModel *const fences : { _geoFenceController->polygons(), _geoFenceController->circles() }) {
        for (int i = 0; i < fences->count(); i++) {
            record.geoZoneFences += GeoZoneFenceTag::isGeoZone(fences->get(i)) ? 1 : 0;
        }
    }
    // The data the fences from geozones were made of
    for (const GeoZoneSource *const source : GeoZoneManager::instance()->repository()->sourceList()) {
        record.sources.append(GeoZoneSourceVersion::fromSource(source));
    }
    record.notFenced = GeoZoneFenceTags::notFenced(_geoFenceController, &record.notFencedCount);
    return record;
}

void GeoZoneFenceUpload::_planFenceSent(Vehicle *vehicle, bool error)
{
    // An upload of this object records its fence once it is read back
    if (error || !vehicle || !_geoFenceController || busy()) {
        return;
    }
    GeoFenceManager *const manager = vehicle->geoFenceManager();
    const FenceSnapshot sent = snapshot(manager->polygons(), manager->circles(), manager->breachReturnPoint());
    // Of another plan, or edited since: the zones this plan doesn't fence aren't those of the fence sent
    if (!compareFences(snapshot(_geoFenceController), sent).isEmpty()) {
        return;
    }
    Verification record = _record(sent);
    record.verified = false;
    // Each importer of the plan follows the upload
    const std::optional<Verification> previous = verification(vehicle);
    if (previous && !previous->verified && (previous->fence == record.fence) && (previous->notFenced == record.notFenced) &&
        (previous->notFencedCount == record.notFencedCount) && (previous->geoZoneFences == record.geoZoneFences) && (previous->sources == record.sources)) {
        return;
    }
    setVerification(vehicle, record);
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
    // With the zones the plan doesn't fence now: reading back may replace the fences of an empty plan
    _sentRecord = _record(_sentFence);
    _error.clear();
    _ackResult = MAV_MISSION_ACCEPTED;
    _rejectedForRoom = false;
    _uploadNumber++;

    _uploadConnections.disconnect();
    // The fence manager reports a rejected upload without the reason the vehicle gave
    _uploadConnections << connect(vehicle, &Vehicle::mavlinkMessageReceived, this, [this](const mavlink_message_t &message) {
        if (message.msgid != MAVLINK_MSG_ID_MISSION_ACK) {
            return;
        }
        mavlink_mission_ack_t ack;
        mavlink_msg_mission_ack_decode(&message, &ack);
        if ((ack.mission_type == MAV_MISSION_TYPE_FENCE) && (ack.type != MAV_MISSION_ACCEPTED)) {
            _ackResult = ack.type;
        }
    });
    _uploadConnections << connect(_manager, &GeoFenceManager::error, this, [this](int, const QString &message) { _error = message; });
    _uploadConnections << connect(_manager, &GeoFenceManager::sendComplete, this, [this](bool error) { _sent(error); });
    _uploadConnections << connect(_manager, &GeoFenceManager::loadComplete, this, [this]() { _readBack(); });
    _uploadConnections << connect(vehicle, &QObject::destroyed, this, [this]() { _vehicleLost(); });
    _uploadConnections << connect(masterController, &PlanMasterController::managerVehicleChanged, this, [this]() { _vehicleLost(); });
    _uploadConnections << connect(masterController, &PlanMasterController::currentPlanFileChanged, this, [this, masterController]() {
        if (masterController->currentPlanFile() != _planFile) {
            abort(tr("The plan file changed during the upload, upload the fence again."));
        }
    });
    // Edits of the fence. Sending clears the dirty state of the fences, so that the first edit of each sets it again.
    const auto edited = [this]() { _checkFence(); };
    _uploadConnections << connect(_geoFenceController, &QObject::destroyed, this, [this]() { abort(tr("The plan was closed during the upload.")); });
    _uploadConnections << connect(_geoFenceController, &GeoFenceController::dirtyChanged, this, edited);
    _uploadConnections << connect(_geoFenceController, &GeoFenceController::breachReturnPointChanged, this, edited);
    for (QmlObjectListModel *const fences : { _geoFenceController->polygons(), _geoFenceController->circles() }) {
        _uploadConnections << connect(fences, &QmlObjectListModel::countChanged, this, edited);
        _uploadConnections << connect(fences, &QmlObjectListModel::dirtyChanged, this, edited);
    }

    // Once the upload is wired, so that what those told about it do right away is followed: an edit of the fence, or
    // aborting it, ends it before anything is sent
    if (!_transition(Sending, tr("Uploading the fence…"))) {
        return;
    }
    // An edit of a fence which is dirty already signals nothing
    _checkFence();
    if (_state != Sending) {
        return;
    }
    _geoFenceController->sendToVehicle();
}

void GeoZoneFenceUpload::abort(const QString &reason)
{
    if (busy()) {
        (void) _transition(Aborted, reason);
    }
}

bool GeoZoneFenceUpload::detach()
{
    if (!busy() || !_geoFenceController) {
        return false;
    }
    // With the plan, which the upload ends with
    setParent(_geoFenceController);
    (void) connect(this, &GeoZoneFenceUpload::stateChanged, this, [this]() {
        if (busy()) {
            return;
        }
        emit finishedDetached(_state, _message);
        // Nobody shows the state any more. Names from geozone data aren't markup.
        QString text = _message;
        (void) text.replace(QLatin1Char('<'), QChar(0x2039)).replace(QLatin1Char('>'), QChar(0x203A));
        qgcApp()->showAppMessage(text, tr("Fence Upload"));
        deleteLater();
    });
    return true;
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
        return tr("The vehicle has no room for this fence of %n point(s). Retry with fewer zones, leave out zones or allow more simplification.", nullptr, points);
    case MAV_MISSION_ERROR:
        if (_vehicle && _vehicle->apmFirmware()) {
            return tr("The vehicle rejected the fence. ArduPilot does when the fence exceeds its storage: retry with fewer zones, leave out zones or allow more simplification.");
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
        _rejectedForRoom = (_ackResult == MAV_MISSION_NO_SPACE) || ((_ackResult == MAV_MISSION_ERROR) && _vehicle && _vehicle->apmFirmware());
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
    QTimer::singleShot(0, this, [this, upload = _uploadNumber]() {
        if ((upload != _uploadNumber) || (_state != Verifying)) {
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
        Verification verification = _sentRecord;
        verification.time = GeoZoneEnvironment::currentDateTimeUtc();
        setVerification(_vehicle, verification);
        QString message = tr("Verified: the vehicle stores the fence of %1.").arg(_countText(_sentFence));
        const QString notFenced = GeoZoneFenceTags::notFencedText(verification.notFenced, verification.notFencedCount);
        if (!notFenced.isEmpty()) {
            message += QLatin1Char(' ') + notFenced;
        }
        (void) _transition(Verified, message);
    } else {
        (void) _transition(Mismatch, tr("The fence read back differs from the plan. %1").arg(differences));
    }
}

QJsonObject GeoZoneFenceUpload::Verification::toJson() const
{
    QJsonArray sourceList;
    for (GeoZoneSourceVersion source : sources) {
        // Only what tells whether the data changed since
        source.provenance = QJsonObject();
        sourceList.append(source.toJson());
    }
    QJsonArray zones;
    for (const GeoZoneFenceTags::NotFenced &zone : notFenced) {
        zones.append(QJsonObject{ { nameKey, zone.name }, { reasonKey, zone.reason } });
    }
    return QJsonObject{
        { timeKey, time.toUTC().toString(Qt::ISODateWithMs) },
        { verifiedKey, verified },
        { geoZoneFencesKey, geoZoneFences },
        { fenceKey, _fenceJson(fence) },
        { sourcesKey, sourceList },
        { notFencedKey, QJsonObject{ { countKey, (std::max)(notFencedCount, static_cast<int>(notFenced.count())) }, { zonesKey, zones } } },
    };
}

std::optional<GeoZoneFenceUpload::Verification> GeoZoneFenceUpload::Verification::fromJson(const QJsonObject &json)
{
    const std::optional<FenceSnapshot> fence = _fenceFromJson(json.value(fenceKey).toObject());
    if (!fence || !json.value(fenceKey).isObject()) {
        return std::nullopt;
    }
    Verification verification;
    verification.fence = *fence;
    verification.time = QDateTime::fromString(json.value(timeKey).toString(), Qt::ISODateWithMs).toUTC();
    // Only a record which says so was read back
    verification.verified = json.value(verifiedKey).toBool(false);
    verification.geoZoneFences = (std::max)(json.value(geoZoneFencesKey).toInt(), 0);
    for (const QJsonValue &source : json.value(sourcesKey).toArray()) {
        verification.sources.append(GeoZoneSourceVersion::fromJson(source.toObject()));
    }
    const QJsonObject notFencedJson = json.value(notFencedKey).toObject();
    for (const QJsonValue &zone : notFencedJson.value(zonesKey).toArray()) {
        verification.notFenced.append(GeoZoneFenceTags::NotFenced{ zone[nameKey].toString(), zone[reasonKey].toString() });
    }
    verification.notFencedCount = (std::max)(notFencedJson.value(countKey).toInt(), static_cast<int>(verification.notFenced.count()));
    return verification;
}

std::optional<GeoZoneFenceUpload::Verification> GeoZoneFenceUpload::verification(const Vehicle *vehicle)
{
    if (!vehicle) {
        return std::nullopt;
    }
    const Record *const record = records().find(vehicle->vehicleUID(), vehicle->id());
    if (!record) {
        return std::nullopt;
    }
    Verification verification = record->verification;
    verification.identified = (record->vehicle == vehicle) || ((record->uid != 0) && (record->uid == vehicle->vehicleUID()));
    return verification;
}

void GeoZoneFenceUpload::setVerification(Vehicle *vehicle, const Verification &verification)
{
    if (!vehicle) {
        return;
    }
    static QSet<const Vehicle*> followed;
    if (!followed.contains(vehicle)) {
        followed.insert(vehicle);
        (void) connect(vehicle, &QObject::destroyed, vehicle, [vehicle]() {
            records().forget(vehicle);
            (void) followed.remove(vehicle);
        });
    }
    records().set(Record{ vehicle->vehicleUID(), vehicle->id(), verification, vehicle });
}

void GeoZoneFenceUpload::clearVerification(Vehicle *vehicle)
{
    if (vehicle) {
        records().remove(vehicle->vehicleUID(), vehicle->id());
    }
}

void GeoZoneFenceUpload::setRecordsFile(const QString &fileName)
{
    records().setFileName(fileName);
}

GeoZoneFences::Usage GeoZoneFenceUpload::sentUsage() const
{
    GeoZoneFences::Usage usage;
    for (const FenceSnapshot::Polygon &polygon : _sentFence.polygons) {
        usage.addPolygon(polygon.vertices.count());
    }
    for (qsizetype i = 0; i < _sentFence.circles.count(); i++) {
        usage.addCircle();
    }
    if (_sentFence.returnPoint.isValid()) {
        usage.addReturnPoint();
    }
    return usage;
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
    const auto isNear = [](const QGeoCoordinate &a, const QGeoCoordinate &b) {
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
                found = isNear(polygon.vertices[v], other.vertices[v]);
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
            // Each circle read back matches one circle sent: those matched stay matched, also for repeated circles
            if (circleMatched[i]) {
                continue;
            }
            found = (other.inclusion == circle.inclusion) && isNear(circle.circle.center(), other.circle.center()) &&
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
    } else if (sent.returnPoint.isValid() && !isNear(sent.returnPoint, received.returnPoint)) {
        differences.append(tr("The breach return point on the vehicle differs from the plan."));
    }
    return differences.join(QLatin1Char(' '));
}
