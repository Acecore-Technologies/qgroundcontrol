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
/// GeoZoneFenceWarning): what its autopilot does then and where the vehicle reaches the fence
ColumnLayout {
    id:         control
    spacing:    ScreenTools.defaultFontPixelHeight * 0.25
    visible:    _shown

    property color textColor: "white"

    readonly property var   _monitor:       GeoZoneMonitor.activeVehicle
    readonly property var   _warning:       _monitor ? _monitor.fenceWarning : null
    readonly property bool  _breached:      !!_monitor && _monitor.fenceBreached
    readonly property bool  _shown:         !!_warning && (_warning.active || _breached)

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        textFormat:         Text.PlainText
        font.bold:          true
        color:              qgcPal.colorRed
        text:               !control._warning ? "" :
                                (control._breached ? qsTr("GEOFENCE breached, the vehicle takes its fence action: %1").arg(control._warning.actionText)
                                                   : qsTr("GEOFENCE ahead, the vehicle will take its fence action: %1").arg(control._warning.actionText))
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
}
