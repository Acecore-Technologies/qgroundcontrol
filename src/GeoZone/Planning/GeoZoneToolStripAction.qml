/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick

import QGroundControl.Controls
import QGroundControl.GeoZone

/// Geozones action of the Plan view's tool strip, which opens the Geozones panel of GeoZonePlanViewOverlay
ToolStripAction {
    required property var overlay   ///< GeoZonePlanViewOverlay

    text:               qsTr("Geozones")
    iconSource:         "qrc:/qml/QGroundControl/GeoZone/GeoZone.svg"
    visible:            GeoZoneManager.zoneCount > 0
    dropPanelComponent: overlay.dropPanelComponent
}
