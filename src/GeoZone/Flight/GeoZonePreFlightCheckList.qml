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
/// GeoZonePreflightCheck. QGCOptions::preFlightChecklistUrl points to it. The checklist passes when both pass, and the
/// geozone checks are reset with those of the vehicle type: by its reset button and for another active vehicle.
ColumnLayout {
    id:         root
    spacing:    0.8 * ScreenTools.defaultFontPixelWidth

    readonly property bool allChecksPassed: standardChecklist.item ? (standardChecklist.item.allChecksPassed && geoZoneGroup.passed) : false

    // The only place the vehicle's state is set: the embedded checklist of the vehicle type leaves it to this one
    onAllChecksPassedChanged: {
        if (globals.activeVehicle) {
            globals.activeVehicle.checkListState = allChecksPassed ? Vehicle.CheckListPassed : Vehicle.CheckListFailed
        }
    }

    /// Called by the geozone group when it passes or fails, like PreFlightCheckList.groupPassedChanged
    function groupPassedChanged(index, passed) {
    }

    Loader {
        id:                     standardChecklist
        Layout.fillWidth:       true
        Component.onCompleted:  setSource("qrc:/qml/QGroundControl/FlightDisplay/PreFlightCheckList.qml", { embedded: true })
    }

    Connections {
        target:                     standardChecklist.item
        function onGroupsReset()    { geoZoneGroup.reset() }
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

        GeoZonePreFlightCheckButton {
            name:   qsTr("Permits")
            item:   geoZoneGroup.check.permitItem
        }

        // The zones of the permits, acknowledged here for this flight. The group counts the children which haven't
        // passed, so the list passes.
        Column {
            width:      40 * ScreenTools.defaultFontPixelWidth
            spacing:    ScreenTools.defaultFontPixelHeight / 4
            visible:    geoZoneGroup.check.permitZones.length > 0

            property bool passed: true
            property bool failed: false
            function reset() { }

            Repeater {
                model: geoZoneGroup.check.permitZones

                RowLayout {
                    id:         permitRow
                    width:      parent.width
                    spacing:    ScreenTools.defaultFontPixelWidth

                    required property var modelData

                    QGCLabel {
                        Layout.fillWidth:   true
                        wrapMode:           Text.WordWrap
                        textFormat:         Text.PlainText
                        text:               (permitRow.modelData.atVehicle ? qsTr("%1 (%2), at the vehicle") : qsTr("%1 (%2), on the mission"))
                                                .arg(permitRow.modelData.name).arg(permitRow.modelData.restrictionText)
                    }

                    QGCButton {
                        text:       qsTr("I Have Permission")
                        visible:    !permitRow.modelData.permitted
                        onClicked:  GeoZoneMonitor.permits.acknowledge(permitRow.modelData.zone, GeoZonePermits.Checklist)
                    }

                    QGCLabel {
                        text:       qsTr("Acknowledged")
                        visible:    permitRow.modelData.permitted
                    }
                }
            }
        }
    }
}
