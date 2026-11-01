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
#include "GeoZoneFenceTags.h"
#include "GeoZoneManager.h"
#include "GeoZonePlanAudit.h"
#include "GeoZonePlanning.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneSource.h"
#include "GeoZoneSourceRepository.h"
#include "PlanMasterController.h"

#include <QtCore/QJsonArray>

#include <algorithm>

namespace GeoZonePlanData {

namespace {

constexpr const char *plannedStartKey = "plannedStart";
constexpr const char *authorisationsKey = "authorisations";
constexpr const char *auditKey = "audit";

} // namespace

void save(PlanMasterController *planController, QJsonObject &planJson, GeoZoneManager *manager)
{
    manager = manager ? manager : GeoZoneManager::instance();
    if (planController) {
        GeoZoneFenceTags::save(planController->geoFenceController(), planJson);
    }

    QJsonObject json;
    if (manager->planning()->plannedStart().isValid()) {
        json.insert(plannedStartKey, manager->planning()->plannedStart().toUTC().toString(Qt::ISODate));
    }

    // Without geozone data there is nothing to audit
    const QList<const GeoZoneSource*> sources = manager->repository()->sourceList();
    const bool audit = std::any_of(sources.cbegin(), sources.cend(), [](const GeoZoneSource *source) { return source->enabled() && source->loaded(); });

    QJsonArray authorisations;
    if (planController && (audit || (manager->planning()->authorisations()->count() > 0))) {
        GeoZoneMissionCheck check(manager);
        check.setMissionController(planController->missionController());
        check.update();
        for (const GeoZone *zone : check.authorisedZones()) {
            if (const GeoZoneAuthorisation *const authorisation = manager->planning()->authorisations()->find(zone)) {
                authorisations.append(authorisation->toJson());
            }
        }
        if (audit) {
            const GeoZonePlanAudit::Record record = check.auditRecord();
            json.insert(auditKey, record.toJson());
            manager->planning()->setPlanAudit(record);
        }
    }
    if (!authorisations.isEmpty()) {
        json.insert(authorisationsKey, authorisations);
    }

    if (!json.isEmpty()) {
        planJson.insert(jsonKey, json);
    }
}

void load(PlanMasterController *planController, const QJsonObject &planJson, GeoZoneManager *manager)
{
    manager = manager ? manager : GeoZoneManager::instance();
    if (planController) {
        GeoZoneFenceTags::load(planController->geoFenceController(), planJson);
    }

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

    // Older plans don't have the data they were saved with
    manager->planning()->setPlanAudit(GeoZonePlanAudit::Record::fromJson(json.value(auditKey).toObject()));
}

} // namespace GeoZonePlanData
