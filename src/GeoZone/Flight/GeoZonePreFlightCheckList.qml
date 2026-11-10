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
import QGroundControl.ScreenTools
import QGroundControl.Vehicle

/// The pre-flight checklist of the vehicle type, see PreFlightCheckList.qml, followed by the automatic geozone checks of
/// GeoZonePreflightCheck. QGCOptions::preFlightChecklistUrl points to it. The checklist passes when both pass.
ColumnLayout {
    id:         root
    spacing:    0.8 * ScreenTools.defaultFontPixelWidth

    readonly property bool allChecksPassed: standardChecklist.item ? (standardChecklist.item.allChecksPassed && geoZoneGroup.passed) : false

    // The vehicle's state follows this checklist rather than the standard one alone
    function _updateCheckListState() {
        if (globals.activeVehicle) {
            globals.activeVehicle.checkListState = allChecksPassed ? Vehicle.CheckListPassed : Vehicle.CheckListFailed
        }
    }

    /// Called by the geozone group when it passes or fails, like PreFlightCheckList.groupPassedChanged
    function groupPassedChanged(index, passed) {
        _updateCheckListState()
    }

    onAllChecksPassedChanged: _updateCheckListState()

    Loader {
        id:                 standardChecklist
        Layout.fillWidth:   true
        source:             "qrc:/qml/QGroundControl/FlightDisplay/PreFlightCheckList.qml"
    }

    Connections {
        target:                         standardChecklist.item
        function onAllChecksPassedChanged() { root._updateCheckListState() }
    }

    PreFlightCheckGroup {
        id:                 geoZoneGroup
        name:               qsTr("Geozones")
        Layout.fillWidth:   true

        property GeoZonePreflightCheck check: GeoZonePreflightCheck {
            planController: globals.planMasterControllerFlyView
        }

        GeoZonePreFlightCheckButton {
            name:   qsTr("Geozone data")
            item:   geoZoneGroup.check.dataItem
        }

        GeoZonePreFlightCheckButton {
            name:   qsTr("Mission")
            item:   geoZoneGroup.check.missionItem
        }

        GeoZonePreFlightCheckButton {
            name:   qsTr("Fence")
            item:   geoZoneGroup.check.fenceItem
        }

        GeoZonePreFlightCheckButton {
            name:   qsTr("QNH")
            item:   geoZoneGroup.check.qnhItem
        }

        GeoZonePreFlightCheckButton {
            name:   qsTr("Remote ID")
            item:   geoZoneGroup.check.remoteIdItem
        }

        GeoZonePreFlightCheckButton {
            name:   qsTr("Take-off position")
            item:   geoZoneGroup.check.takeoffItem
        }

        GeoZonePreFlightCheckButton {
            name:   qsTr("Geo-awareness")
            item:   geoZoneGroup.check.awarenessItem
        }
    }
}
