/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Fence part of GeoZoneAlertPopup, shown while the active vehicle is about to breach its own fence or breaches it (see
/// GeoZoneFenceWarning): what its autopilot does then, where the vehicle reaches the fence and how the pilot overrides
/// the fence action, with Take Control switching to the flight mode the pilot flies in and, on ArduPilot, Disable Fence
/// after a confirmation
ColumnLayout {
    id:         control
    spacing:    ScreenTools.defaultFontPixelHeight * 0.25
    visible:    _shown

    property color textColor: "white"

    readonly property var   _monitor:       GeoZoneMonitor.activeVehicle
    readonly property var   _warning:       _monitor ? _monitor.fenceWarning : null
    readonly property bool  _breached:      !!_monitor && _monitor.fenceBreached
    readonly property bool  _shown:         !!_warning && (_warning.active || _breached || _warning.fenceDisabled)

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        textFormat:         Text.PlainText
        font.bold:          true
        color:              qgcPal.colorRed
        text:               !control._warning ? "" :
                                (control._warning.fenceDisabled ? qsTr("GEOFENCE disabled by the pilot, the vehicle doesn't act on breaches") :
                                    (control._breached ? qsTr("GEOFENCE breached, the vehicle takes its fence action: %1").arg(control._warning.actionText)
                                                       : qsTr("GEOFENCE ahead, the vehicle will take its fence action: %1").arg(control._warning.actionText)))
    }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        textFormat:         Text.PlainText
        color:              control.textColor
        font.pointSize:     ScreenTools.smallFontPointSize
        text:               control._warning ? qsTr("Reaches the fence at %1, marked on the map.").arg(control._warning.intersectionText) : ""
        visible:            !!control._warning && control._warning.active
    }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        textFormat:         Text.PlainText
        color:              control.textColor
        font.pointSize:     ScreenTools.smallFontPointSize
        text:               control._warning ? qsTr("To override: %1").arg(control._warning.overrideText) : ""
    }

    Flow {
        Layout.fillWidth:   true
        spacing:            ScreenTools.defaultFontPixelWidth

        // Shown disabled while the vehicle wouldn't switch, which the text above explains
        QGCButton {
            text:       control._warning ? qsTr("Take Control (%1)").arg(control._warning.takeControlMode) : ""
            visible:    !!control._warning && control._warning.takeControlMode !== ""
            enabled:    !!control._warning && control._warning.canTakeControl
            onClicked:  control._warning.takeControl()
        }

        QGCButton {
            text:       qsTr("Disable Fence")
            visible:    !!control._warning && control._warning.canDisableFence && !control._warning.fenceDisabled
            onClicked: {
                // The fence of the vehicle asked about, also when another one is active once the pilot answers
                const warning = control._warning
                mainWindow.showMessageDialog(qsTr("Disable Fence"),
                                             qsTr("Disable the vehicle's fence? It then no longer acts on any fence breach until the fence is enabled again."),
                                             Dialog.Ok | Dialog.Cancel,
                                             function() {
                                                 if (warning) {
                                                     warning.disableFence()
                                                 }
                                             })
            }
        }

        // Disabled by the pilot, who enables it again once in control
        QGCButton {
            text:       qsTr("Enable Fence")
            visible:    !!control._warning && control._warning.fenceDisabled
            onClicked:  control._warning.enableFence()
        }
    }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        textFormat:         Text.PlainText
        color:              control.textColor
        font.pointSize:     ScreenTools.smallFontPointSize
        text:               control._warning ? control._warning.commandText : ""
        visible:            text !== ""
    }
}
