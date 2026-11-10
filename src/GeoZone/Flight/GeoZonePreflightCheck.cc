/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePreflightCheck.h"
#include "Fact.h"
#include "GeoFenceController.h"
#include "GeoFenceManager.h"
#include "GeoZone.h"
#include "GeoZoneCatalog.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneIntendedLevel.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionReader.h"
#include "GeoZoneMonitor.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZonePlanning.h"
#include "GeoZonePolicy.h"
#include "GeoZoneSettings.h"
#include "GeoZoneSourceRepository.h"
#include "GeoZoneVehicleMonitor.h"
#include "GeoZoneVertical.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "RemoteIDManager.h"
#include "RemoteIDSettings.h"
#include "SettingsManager.h"
#include "Vehicle.h"

#include <QtCore/QtMath>

#include <algorithm>
#include <cmath>

namespace {

constexpr int updateDelayMSecs = 250;

QString names(const QList<const GeoZone*> &zones)
{
    return GeoZonePathEvaluator::zoneNames(zones, GeoZoneMissionCheck::maxListedZones, false);
}

QString timeText(const QDateTime &time)
{
    return time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace

QVariantMap GeoZonePreflightCheck::Item::toMap() const
{
    return QVariantMap{ { QStringLiteral("state"), static_cast<int>(state) }, { QStringLiteral("text"), text } };
}

GeoZonePreflightCheck::GeoZonePreflightCheck(QObject *parent)
    : GeoZonePreflightCheck(nullptr, parent)
{
    setMonitor(GeoZoneMonitor::instance());
}

GeoZonePreflightCheck::GeoZonePreflightCheck(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _missionCheck(new GeoZoneMissionCheck(_manager, this))
{
    _updateTimer.setSingleShot(true);
    _updateTimer.setInterval(updateDelayMSecs);
    (void) connect(&_updateTimer, &QTimer::timeout, this, &GeoZonePreflightCheck::update);

    (void) connect(_missionCheck, &GeoZoneMissionCheck::zonesChanged, this, &GeoZonePreflightCheck::_scheduleUpdate);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZonePreflightCheck::_scheduleUpdate);
    (void) connect(_manager->repository(), &GeoZoneSourceRepository::validityWarningChanged, this, &GeoZonePreflightCheck::_scheduleUpdate);
    (void) connect(GeoZoneSettings::instance()->qnhHPa(), &Fact::rawValueChanged, this, &GeoZonePreflightCheck::_scheduleUpdate);
    RemoteIDSettings *const remoteIdSettings = SettingsManager::instance()->remoteIDSettings();
    for (Fact *const fact : { remoteIdSettings->operatorID(), remoteIdSettings->operatorIDValid(), remoteIdSettings->region() }) {
        (void) connect(fact, &Fact::rawValueChanged, this, &GeoZonePreflightCheck::_scheduleUpdate);
    }
    update();
}

PlanMasterController *GeoZonePreflightCheck::planController() const
{
    return _planController;
}

void GeoZonePreflightCheck::setPlanController(PlanMasterController *planController)
{
    if (planController == _planController) {
        return;
    }
    if (_planController) {
        (void) disconnect(_planController, nullptr, this, nullptr);
    }
    _planController = planController;
    _connectPlan();
    emit planControllerChanged();
}

void GeoZonePreflightCheck::_connectPlan()
{
    _missionCheck->setMissionController(_planController ? _planController->missionController() : nullptr);
    _missionCheck->setGeoFenceController(_planController ? _planController->geoFenceController() : nullptr);
    if (_planController) {
        (void) connect(_planController, &PlanMasterController::managerVehicleChanged, this, &GeoZonePreflightCheck::_connectVehicle);
        (void) connect(_planController->missionController(), &MissionController::visualItemsChanged, this, &GeoZonePreflightCheck::_scheduleUpdate);
    }
    _connectVehicle();
}

void GeoZonePreflightCheck::_connectVehicle()
{
    delete _vehicleConnections;
    _vehicleConnections = new QObject(this);
    _vehicle = _planController ? _planController->managerVehicle() : nullptr;
    if (_vehicle) {
        if (RemoteIDManager *const remoteId = _vehicle->remoteIDManager()) {
            for (const auto signal : { &RemoteIDManager::availableChanged, &RemoteIDManager::armStatusGoodChanged, &RemoteIDManager::armStatusErrorChanged,
                                       &RemoteIDManager::commsGoodChanged, &RemoteIDManager::operatorIDGoodChanged }) {
                (void) connect(remoteId, signal, _vehicleConnections, [this]() { _scheduleUpdate(); });
            }
        }
        // An upload records the fence it verified when the fence is read back
        if (GeoFenceManager *const fenceManager = _vehicle->geoFenceManager()) {
            (void) connect(fenceManager, &GeoFenceManager::loadComplete, _vehicleConnections, [this]() { _scheduleUpdate(); });
            (void) connect(fenceManager, &GeoFenceManager::sendComplete, _vehicleConnections, [this]() { _scheduleUpdate(); });
        }
    }
    _connectMonitor();
}

void GeoZonePreflightCheck::setMonitor(GeoZoneMonitor *monitor)
{
    if (monitor == _monitor) {
        return;
    }
    if (_monitor) {
        (void) disconnect(_monitor, nullptr, this, nullptr);
    }
    _monitor = monitor;
    if (_monitor) {
        (void) connect(_monitor, &GeoZoneMonitor::activeMonitorChanged, this, &GeoZonePreflightCheck::_connectMonitor);
        (void) connect(_monitor, &GeoZoneMonitor::monitoringChanged, this, &GeoZonePreflightCheck::_connectMonitor);
    }
    _connectMonitor();
}

GeoZoneVehicleMonitor *GeoZonePreflightCheck::_vehicleMonitor() const
{
    return (_monitor && _vehicle) ? _monitor->vehicleMonitor(_vehicle) : nullptr;
}

void GeoZonePreflightCheck::_connectMonitor()
{
    delete _monitorConnections;
    _monitorConnections = new QObject(this);
    if (GeoZoneVehicleMonitor *const monitor = _vehicleMonitor()) {
        for (const auto signal : { &GeoZoneVehicleMonitor::groundStatusChanged, &GeoZoneVehicleMonitor::armedChanged, &GeoZoneVehicleMonitor::positionUnavailableChanged }) {
            (void) connect(monitor, signal, _monitorConnections, [this]() { _scheduleUpdate(); });
        }
    }
    _scheduleUpdate();
}

void GeoZonePreflightCheck::_scheduleUpdate()
{
    if (!_updateTimer.isActive()) {
        _updateTimer.start();
    }
}

void GeoZonePreflightCheck::update()
{
    _updateTimer.stop();
    const Item data = dataItem(_manager->zoneCount(), _manager->repository()->validityWarning(), _manager->repository()->dataInvalid());

    Item mission{ Checking, tr("Checking the mission\u2026") };
    Item fence = mission;
    Item qnh = mission;
    Item remoteId = mission;
    MissionController *const missionController = _planController ? _planController->missionController() : nullptr;
    // The mission check may not have checked the current mission yet
    if (missionController && !_missionCheck->settled()) {
        _updateTimer.start();
    } else {
        const QList<GeoZoneMissionCheck::Entry> &entries = _missionCheck->entries();
        const GeoZoneMissionPath path = GeoZoneMissionReader::path(missionController);
        const int itemCount = path.paths().isEmpty() ? 0 : (missionController ? (std::max)(missionController->visualItems()->count() - 1, 0) : 0);
        mission = missionItem(itemCount, entries, _manager->planning()->plannedStart());

        FenceInput fenceInput;
        if (_vehicle && !_vehicle->isOfflineEditingVehicle()) {
            if (GeoFenceManager *const fenceManager = _vehicle->geoFenceManager()) {
                fenceInput.vehicleFence = GeoZoneFenceUpload::snapshot(fenceManager->polygons(), fenceManager->circles(), fenceManager->breachReturnPoint());
            }
            fenceInput.verification = GeoZoneFenceUpload::verification(_vehicle);
        }
        for (const GeoZoneSource *const source : _manager->repository()->sourceList()) {
            fenceInput.sources.append(GeoZoneSourceVersion::fromSource(source));
        }
        fenceInput.unfenced = GeoZoneMissionCheck::zones(entries, GeoZoneMissionCheck::Unfenced);
        fence = fenceItem(fenceInput);

        qnh = qnhItem(flightLevelZonesNear(_manager->catalog(), path), GeoZoneSettings::instance()->qnhHPa()->rawValue().toDouble());
        remoteId = remoteIdItem(GeoZoneMissionCheck::zones(entries, GeoZoneMissionCheck::USpace), GeoZoneRemoteId::status(_vehicle));
    }

    const Item takeoff = takeoffItem(_vehicleMonitor());

    if ((data == _data) && (mission == _mission) && (fence == _fence) && (qnh == _qnh) && (remoteId == _remoteId) && (takeoff == _takeoff)) {
        return;
    }
    _data = data;
    _mission = mission;
    _fence = fence;
    _qnh = qnh;
    _remoteId = remoteId;
    _takeoff = takeoff;
    emit itemsChanged();
}

GeoZonePreflightCheck::Item GeoZonePreflightCheck::dataItem(int zoneCount, const QString &validityWarning, bool dataInvalid)
{
    if (zoneCount <= 0) {
        return { Warning, tr("No geozone data is loaded. Add sources in the Geozones settings.") };
    }
    if (!validityWarning.isEmpty()) {
        const QString sources = QString(validityWarning).replace(QLatin1Char('\n'), QStringLiteral("; "));
        return { Warning, dataInvalid ? tr("Expired geozone data, its zones are still checked: %1").arg(sources)
                                      : tr("Geozone data may be outdated: %1").arg(sources) };
    }
    return { Passed, tr("No geozone data is outdated.") };
}

GeoZonePreflightCheck::Item GeoZonePreflightCheck::missionItem(int itemCount, const QList<GeoZoneMissionCheck::Entry> &entries, const QDateTime &plannedStart)
{
    if (itemCount <= 0) {
        return { NotApplicable, tr("No mission on the vehicle.") };
    }
    const QList<const GeoZone*> crossed = GeoZoneMissionCheck::zones(entries, GeoZoneMissionCheck::Crossed);
    if (!crossed.isEmpty()) {
        // Prohibited zones can't be accepted here, only authorised in the Plan View
        const bool prohibited = std::any_of(crossed.cbegin(), crossed.cend(), [](const GeoZone *zone) { return zone->restriction() == GeoZone::Prohibited; });
        return { prohibited ? Failed : Warning, GeoZoneMissionCheck::warningText(entries) };
    }
    const QString when = plannedStart.isValid() ? tr("for the planned start %1").arg(timeText(plannedStart)) : tr("at any time, without a planned start");
    const QList<const GeoZone*> authorised = GeoZoneMissionCheck::zones(entries, GeoZoneMissionCheck::Authorised);
    if (!authorised.isEmpty()) {
        return { Passed, tr("Enters only restricted geozones authorised %1: %2.").arg(when, names(authorised)) };
    }
    return { Passed, tr("Enters no restricted geozone %1.").arg(when) };
}

GeoZonePreflightCheck::Item GeoZonePreflightCheck::fenceItem(const FenceInput &input)
{
    QStringList problems;
    if (!input.unfenced.isEmpty()) {
        problems.append(tr("The vehicle's fence doesn't exclude the prohibited geozones the mission enters: %1.").arg(names(input.unfenced)));
    }
    const bool vehicleFence = !input.vehicleFence.polygons.isEmpty() || !input.vehicleFence.circles.isEmpty();
    if (input.verification) {
        const GeoZoneFenceUpload::Verification &verification = *input.verification;
        if (!GeoZoneFenceUpload::compareFences(verification.fence, input.vehicleFence).isEmpty()) {
            problems.append(tr("The vehicle's fence changed since it was verified at %1.").arg(timeText(verification.time)));
        } else if (verification.geoZoneFences > 0) {
            QStringList changed;
            for (const GeoZoneSourceVersion &used : verification.sources) {
                const auto current = std::find_if(input.sources.cbegin(), input.sources.cend(), [&used](const GeoZoneSourceVersion &source) { return source.key == used.key; });
                if ((current == input.sources.cend()) || !current->sameData(used)) {
                    changed.append(used.name.isEmpty() ? used.key : used.name);
                }
            }
            if (!changed.isEmpty()) {
                problems.append(tr("Its fences from geozones were made from data which changed since: %1. Add them again in the Plan View and upload the fence.")
                                    .arg(changed.join(QStringLiteral(", "))));
            }
        }
    } else if (vehicleFence) {
        problems.append(tr("The vehicle's fence wasn't verified. Upload it from the fence import of the Plan View, which reads it back."));
    }
    if (!problems.isEmpty()) {
        return { Warning, problems.join(QLatin1Char(' ')) };
    }
    if (input.verification) {
        return { Passed, (input.verification->geoZoneFences > 0) ? tr("Verified at %1, made from the current geozone data.").arg(timeText(input.verification->time)) :
                                                                   tr("Verified at %1.").arg(timeText(input.verification->time)) };
    }
    return { NotApplicable, tr("No fence on the vehicle.") };
}

GeoZonePreflightCheck::Item GeoZonePreflightCheck::qnhItem(const QList<const GeoZone*> &flightLevelZones, double qnhHPa)
{
    if (flightLevelZones.isEmpty()) {
        return { NotApplicable, tr("No flight level limits near the mission altitudes.") };
    }
    if (qFuzzyCompare(qnhHPa, GeoZoneEnvironment::kStandardPressureHPa)) {
        return { Warning, tr("Flight level limits of %1 are near the mission altitudes and converted with the standard pressure. Set the local QNH in the Geozones settings.")
                              .arg(names(flightLevelZones)) };
    }
    return { Passed, tr("The flight level limits of %1 are converted with the QNH %2 hPa.").arg(names(flightLevelZones), QString::number(qnhHPa, 'f', 1)) };
}

GeoZonePreflightCheck::Item GeoZonePreflightCheck::remoteIdItem(const QList<const GeoZone*> &uSpace, const GeoZoneRemoteId::Status &status)
{
    if (uSpace.isEmpty()) {
        return { NotApplicable, tr("The mission doesn't enter U-space airspace.") };
    }
    const QString problem = GeoZoneRemoteId::problem(status);
    if (!problem.isEmpty()) {
        return { Warning, tr("The mission enters U-space airspace %1, which requires Remote ID. %2").arg(names(uSpace), problem) };
    }
    return { Passed, tr("Remote ID is ready for U-space airspace %1.").arg(names(uSpace)) };
}

GeoZonePreflightCheck::Item GeoZonePreflightCheck::takeoffItem(const GeoZoneVehicleMonitor *monitor)
{
    if (!monitor || !monitor->armed().has_value()) {
        return { NotApplicable, tr("No vehicle connected.") };
    }
    if (!monitor->onGround()) {
        return { NotApplicable, tr("The vehicle is armed.") };
    }
    if (monitor->positionUnavailable() || !monitor->position().coordinate.isValid()) {
        return { Warning, tr("The vehicle sends no current position, so it can't be checked against the geozones.") };
    }
    const QString warning = monitor->groundWarning();
    if (warning.isEmpty()) {
        return { Passed, tr("The vehicle isn't inside a restricted geozone.") };
    }
    switch (monitor->groundLevel()) {
    case GeoZoneIntendedLevel::Warning:
        return { Failed, warning + QLatin1Char('.') };
    case GeoZoneIntendedLevel::Caution:
        return { Warning, warning + QLatin1Char('.') };
    default:
        break;
    }
    return { Passed, warning + QLatin1Char('.') };
}

QList<const GeoZone*> GeoZonePreflightCheck::flightLevelZonesNear(const GeoZoneCatalog &catalog, const GeoZoneMissionPath &path)
{
    double lowest = qQNaN();
    double highest = qQNaN();
    for (const GeoZoneMissionPath::Path &missionPath : path.paths()) {
        for (const GeoZone::Position &position : missionPath.positions) {
            if (!std::isnan(position.altitudeAMSL)) {
                lowest = std::isnan(lowest) ? position.altitudeAMSL : (std::min)(lowest, position.altitudeAMSL);
                highest = std::isnan(highest) ? position.altitudeAMSL : (std::max)(highest, position.altitudeAMSL);
            }
        }
    }
    const GeoZoneBox box = path.box();
    if (!box.isValid()) {
        return {};
    }
    const double latitudeMargin = qRadiansToDegrees(qnhHorizontalMargin / 6371000.0);
    const double longitudeMargin = latitudeMargin / (std::max)(std::cos(qDegreesToRadians((std::max)(std::abs(box.south()), std::abs(box.north())))), 0.01);
    const GeoZoneBox area(box.south() - latitudeMargin, box.west() - longitudeMargin, box.north() + latitudeMargin, box.east() + longitudeMargin);
    const GeoZone::Position center{ box.center(), qQNaN(), qQNaN() };

    QList<const GeoZone*> zones;
    for (const GeoZone *zone : catalog.zonesInArea(area, GeoZonePolicy::alerts())) {
        const QList<GeoZone::Volume> &volumes = zone->data().volumes;
        const bool near = std::any_of(volumes.cbegin(), volumes.cend(), [&](const GeoZone::Volume &volume) {
            for (const bool upper : { false, true }) {
                if ((upper ? volume.upperReference : volume.lowerReference) != GeoZone::VerticalReference::FlightLevel) {
                    continue;
                }
                // Any limit may be near unknown altitudes
                const double limit = GeoZoneVertical::limitAMSL(volume, upper, center);
                if (std::isnan(highest) || (!std::isnan(limit) && (limit >= (lowest - qnhVerticalMargin)) && (limit <= (highest + qnhVerticalMargin)))) {
                    return true;
                }
            }
            return false;
        });
        if (near) {
            zones.append(zone);
        }
    }
    return zones;
}
