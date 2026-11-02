/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneOperationalIntentExport.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMissionPath.h"
#include "GeoZonePlanning.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "Vehicle.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>

GeoZoneOperationalIntentExport::GeoZoneOperationalIntentExport(GeoZoneManager *manager, QObject *parent)
    : GeoZoneMissionExport(manager, { tr("JSON data (*.json)") }, QStringLiteral("-intent.json"), QStringLiteral("operational-intent.json"), parent)
{
}

std::optional<GeoZoneOperationalIntent> GeoZoneOperationalIntentExport::intent(QString &errorString) const
{
    MissionController *const missionController = this->missionController();
    if (!missionController) {
        errorString = tr("No mission to export.");
        return std::nullopt;
    }
    const auto [start, end] = GeoZoneMissionCheck::plannedFlightTimes(_manager, missionController);
    if (!start.isValid()) {
        errorString = tr("Set the planned start of the flight in the Geozones panel first, the operational intent needs the flight times.");
        return std::nullopt;
    }

    GeoZoneOperationalIntent intent;
    intent.generatedAt = GeoZoneEnvironment::currentDateTimeUtc();
    const GeoZoneMissionPath path = GeoZoneMissionPath::fromMission(missionController);
    const QDateTime flightEnd = path.flightEnd(start, end);
    if (flightEnd.addSecs(intent.parameters.timeBuffer) <= intent.generatedAt) {
        errorString = tr("The planned flight is over, set a later planned start.");
        return std::nullopt;
    }
    const std::optional<QList<GeoZoneOperationalIntent::Volume>> volumes = GeoZoneOperationalIntent::buildVolumes(path.evaluatorPaths(start, end), intent.parameters,
                                                                                                                   errorString);
    if (!volumes) {
        return std::nullopt;
    }
    intent.volumes = *volumes;

    GeoZoneOperationalIntent::Flight &flight = intent.flight;
    PlanMasterController *const masterController = missionController->masterController();
    flight.planFile = masterController ? masterController->currentPlanFile() : QString();
    flight.name = flight.planFile.isEmpty() ? tr("Untitled plan") : QFileInfo(flight.planFile).completeBaseName();
    if (Vehicle *const vehicle = masterController ? masterController->managerVehicle() : nullptr) {
        flight.vehicle = vehicle->vehicleTypeString();
    }
    flight.plannedStart = start;
    flight.plannedEnd = flightEnd;
    flight.distance = path.flightDistance();
    flight.repeatsIndefinitely = path.repeatsIndefinitely();
    if (flight.repeatsIndefinitely) {
        intent.notes.append(tr("The mission repeats without end, the volumes cover one pass of the repeated items."));
    }

    // The U-space airspaces where the flight needs the authorisation, and those already held for the zones it enters
    const GeoZoneMissionCheck::Evaluation evaluation = GeoZoneMissionCheck::evaluate(_manager->catalog(), path, start, end);
    const GeoZoneAuthorisations *const authorisations = _manager->planning()->authorisations();
    for (const GeoZone *zone : evaluation.zones) {
        if (evaluation.inactiveZones.contains(zone)) {
            continue;
        }
        if (zone->restriction() == GeoZone::USpace) {
            intent.uSpaceAirspaces.append(GeoZoneOperationalIntent::Airspace{ zone->key(), zone->identifier(), zone->country(), zone->name() });
        }
        if (const GeoZoneAuthorisation *const authorisation = authorisations->find(zone)) {
            intent.authorisations.append(*authorisation);
        }
    }
    return intent;
}

bool GeoZoneOperationalIntentExport::exportIntent(const QString &fileName)
{
    if (fileName.isEmpty()) {
        _setErrorString(tr("No file name"));
        return false;
    }
    const QString path = QFileInfo(fileName).suffix().isEmpty() ? (fileName + QStringLiteral(".json")) : fileName;
    QString errorString;
    const std::optional<GeoZoneOperationalIntent> operationalIntent = intent(errorString);
    if (!operationalIntent) {
        _setErrorString(errorString);
        return false;
    }
    const bool written = writeFile(path, QJsonDocument(operationalIntent->toJson()).toJson(QJsonDocument::Indented), errorString);
    _setErrorString(written ? QString() : errorString);
    return written;
}
