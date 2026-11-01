/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePlanData.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneManager.h"
#include "GeoZonePlanning.h"
#include "GeoZoneMissionCheck.h"
#include "PlanMasterController.h"

#include <QtCore/QJsonArray>

namespace GeoZonePlanData {

namespace {

constexpr const char *plannedStartKey = "plannedStart";
constexpr const char *authorisationsKey = "authorisations";

} // namespace

void save(PlanMasterController *planController, QJsonObject &planJson, GeoZoneManager *manager)
{
    manager = manager ? manager : GeoZoneManager::instance();
    QJsonObject json;
    if (manager->planning()->plannedStart().isValid()) {
        json.insert(plannedStartKey, manager->planning()->plannedStart().toUTC().toString(Qt::ISODate));
    }

    QJsonArray authorisations;
    if ((manager->planning()->authorisations()->count() > 0) && planController) {
        GeoZoneMissionCheck check(manager);
        check.setMissionController(planController->missionController());
        check.update();
        for (const GeoZone *zone : check.authorisedZones()) {
            if (const GeoZoneAuthorisation *const authorisation = manager->planning()->authorisations()->find(zone)) {
                authorisations.append(authorisation->toJson());
            }
        }
    }
    if (!authorisations.isEmpty()) {
        json.insert(authorisationsKey, authorisations);
    }

    if (!json.isEmpty()) {
        planJson.insert(jsonKey, json);
    }
}

void load(const QJsonObject &planJson, GeoZoneManager *manager)
{
    manager = manager ? manager : GeoZoneManager::instance();
    const QJsonObject json = planJson.value(jsonKey).toObject();
    const QDateTime plannedStart = QDateTime::fromString(json.value(plannedStartKey).toString(), Qt::ISODate);
    if (plannedStart.isValid()) {
        manager->planning()->setPlannedStart(plannedStart);
    }

    QList<GeoZoneAuthorisation> authorisations;
    for (const QJsonValue &value : json.value(authorisationsKey).toArray()) {
        if (const auto authorisation = GeoZoneAuthorisation::fromJson(value.toObject())) {
            authorisations.append(*authorisation);
        }
    }
    // Permits are the operator's, so those of a plan from elsewhere are only added when accepted
    manager->planning()->authorisations()->offer(authorisations);
}

} // namespace GeoZonePlanData
