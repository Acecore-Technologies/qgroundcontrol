/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/
import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette

/// Geozone warning in the confirmation of a guided action, GuidedActionConfirm.qml: the zones a go to, orbit, altitude
/// change or takeoff would enter, following the value slider, those a return to launch or a landing would enter, those
/// of the mission flown from the item set as current or resumed at, and the check of the mission on the vehicle when
/// starting it. Empty and hidden for other actions or without zones.
QGCLabel {
    id:                     control
    Layout.fillWidth:       true
    horizontalAlignment:    Text.AlignHCenter
    wrapMode:               Text.WordWrap
    textFormat:             Text.PlainText
    color:                  qgcPal.warningText
    text:                   confirmDialog.visible ? warning(confirmDialog.action, confirmDialog.actionData) : ""
    visible:                text !== ""

    required property var confirmDialog     ///< GuidedActionConfirm, with the action, its data and the guided controller

    readonly property var _guidedController:    confirmDialog.guidedController
    readonly property var _missionController:   _guidedController.missionController
    readonly property var _vehicle:             QGroundControl.multiVehicleManager.activeVehicle

    property GeoZoneMissionCheck _vehicleMissionCheck: GeoZoneMissionCheck {
        missionController:  control._missionController
        // The fence on the vehicle is checked for excluding the prohibited zones of the mission
        geoFenceController: control._missionController ? control._missionController.masterController.geoFenceController : null
        flightNow:          true
    }

    QGCPalette { id: qgcPal }

    /// @return Warning about the geozones the action would enter, empty when there are none. Bound to the slider value.
    function warning(actionCode, actionData) {
        const controller = _guidedController
        const check = GeoZoneMonitor.guidedCheck
        if (!_vehicle || !GeoZoneManager.zoneCount) {
            return ""
        }
        const sliderMeters = QGroundControl.unitsConversion.appSettingsVerticalDistanceUnitsToMeters(confirmDialog.guidedValueSlider.getOutputValue())
        switch (actionCode) {
        case controller.actionGoto:
            return check.gotoWarning(_vehicle, actionData)
        case controller.actionOrbit:
            return check.orbitWarning(_vehicle, controller.orbitMapCircle.center, controller.orbitMapCircle.radius(), _vehicle.homePosition.altitude + sliderMeters)
        case controller.actionChangeAlt:
            return check.altitudeWarning(_vehicle, sliderMeters)
        case controller.actionTakeoff:
            return _vehicle.guidedTakeoffSupported ? check.altitudeWarning(_vehicle, sliderMeters) : ""
        case controller.actionRTL:
            // Smart RTL retraces the path flown
            return (confirmDialog.optionText !== "" && confirmDialog.optionChecked) ? "" : check.returnWarning(_vehicle, _missionController)
        case controller.actionLand:
            return check.landWarning(_vehicle)
        case controller.actionSetWaypoint:
            return check.missionWarning(_vehicle, _missionController, actionData, false)
        case controller.actionResumeMission:
        case controller.actionResumeMissionUploadFail:
            return _missionController ? check.missionWarning(_vehicle, _missionController, _missionController.resumeMissionIndex, true) : ""
        case controller.actionStartMission:
        case controller.actionContinueMission:
            return [ _vehicleMissionCheck.warningText, _vehicleMissionCheck.fenceText ].filter(text => text !== "").join("\n")
        default:
            return ""
        }
    }
}
