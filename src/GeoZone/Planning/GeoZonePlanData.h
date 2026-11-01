/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonObject>

class GeoZoneManager;
class PlanMasterController;

/// Geozone data kept in plan files: the planned flight start and the authorisations of the zones the mission enters,
/// so that a plan opened elsewhere is checked the same way, the audit record of the geozone data the plan was checked
/// against, see GeoZonePlanAudit, and the names and origins of the fences, see GeoZoneFenceTags
namespace GeoZonePlanData {

constexpr const char *jsonKey = "geoZones";

/// Adds the data to the JSON of a plan, and makes its audit record that of the open plan
/// @param manager GeoZoneManager::instance() when null
void save(PlanMasterController *planController, QJsonObject &planJson, GeoZoneManager *manager = nullptr);

/// Sets the planned start and offers the authorisations of a loaded plan, see GeoZoneAuthorisations::offer, sets its
/// audit record, see GeoZonePlanning::planAudit, and the tags of its fences. Plans without a planned start or audit
/// record clear those of the previous plan.
/// @param planController Controller which loaded the plan, null to leave the fences
/// @param manager GeoZoneManager::instance() when null
void load(PlanMasterController *planController, const QJsonObject &planJson, GeoZoneManager *manager = nullptr);

/// Clears the planned start and the audit record of the open plan, like for a new plan
/// @param manager GeoZoneManager::instance() when null
void reset(GeoZoneManager *manager = nullptr);

/// Resets the data whenever the Plan View controller replaces its mission: a new plan (PlanMasterController::removeAll),
/// a plan downloaded from the vehicle, or a plan file loaded, whose data load then sets. Controllers of the Fly View
/// are ignored, since the vehicle's mission isn't the plan being edited.
/// @param manager GeoZoneManager::instance() when null
void follow(PlanMasterController *planController, GeoZoneManager *manager = nullptr);

} // namespace GeoZonePlanData
