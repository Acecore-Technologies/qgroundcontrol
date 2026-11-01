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
/// so that a plan opened elsewhere is checked the same way
namespace GeoZonePlanData {

constexpr const char *jsonKey = "geoZones";

/// Adds the data to the JSON of a plan
/// @param manager GeoZoneManager::instance() when null
void save(PlanMasterController *planController, QJsonObject &planJson, GeoZoneManager *manager = nullptr);

/// Sets the planned start and offers the authorisations of a loaded plan, see GeoZoneAuthorisations::offer
void load(const QJsonObject &planJson, GeoZoneManager *manager = nullptr);

} // namespace GeoZonePlanData
