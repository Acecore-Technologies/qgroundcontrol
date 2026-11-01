/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/
import QtQuick

import QGroundControl
import QGroundControl.GeoZone

/// Geozone warnings for the confirmation of the guided actions of a GuidedActionsController: the zones a go to,
/// orbit, altitude change or takeoff would enter, and the check of the mission on the vehicle when starting it
QtObject {
    id: root

    required property var guidedController  ///< GuidedActionsController, for its action codes, slider and orbit circle
    property var missionController          ///< Mission on the vehicle
    property var geoFenceController         ///< Fence on the vehicle, checked for excluding the prohibited zones of the mission

    readonly property var _vehicle: QGroundControl.multiVehicleManager.activeVehicle

    property GeoZoneMissionCheck _vehicleMissionCheck: GeoZoneMissionCheck {
        missionController:  root.missionController
        geoFenceController: root.geoFenceController
        flightNow:          true
    }

    /// @return Warning about the geozones the action would enter, empty when there are none. Bound to the slider value.
    function warning(actionCode, actionData) {
        const controller = guidedController
        const check = GeoZoneMonitor.guidedCheck
        if (!_vehicle || !GeoZoneManager.zoneCount) {
            return ""
        }
        const sliderMeters = QGroundControl.unitsConversion.appSettingsVerticalDistanceUnitsToMeters(controller.guidedValueSlider.value)
        switch (actionCode) {
        case controller.actionGoto:
            return check.gotoWarning(_vehicle, actionData)
        case controller.actionOrbit:
            return check.orbitWarning(_vehicle, controller.orbitMapCircle.center, controller.orbitMapCircle.radius(), _vehicle.homePosition.altitude + sliderMeters)
        case controller.actionChangeAlt:
            return check.altitudeWarning(_vehicle, sliderMeters)
        case controller.actionTakeoff:
            return _vehicle.guidedTakeoffSupported ? check.altitudeWarning(_vehicle, sliderMeters) : ""
        case controller.actionStartMission:
        case controller.actionContinueMission:
            return [ _vehicleMissionCheck.warningText, _vehicleMissionCheck.fenceText ].filter(text => text !== "").join("\n")
        default:
            return ""
        }
    }

    /// @return true if going to the coordinate would enter geozones, so it must be confirmed
    function gotoEntersZones(coordinate) {
        return warning(guidedController.actionGoto, coordinate) !== ""
    }
}
