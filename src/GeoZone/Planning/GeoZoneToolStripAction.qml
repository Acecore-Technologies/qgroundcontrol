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

/// Geozones action of the Plan view's tool strip, which opens the Geozones panel of GeoZonePlanViewOverlay. Also shown
/// without zones: the panel is where the zones of the map area are downloaded, zones are drawn and the sources managed.
ToolStripAction {
    required property var overlay   ///< GeoZonePlanViewOverlay

    text:               qsTr("Geozones")
    iconSource:         "qrc:/qml/QGroundControl/GeoZone/GeoZone.svg"
    dropPanelComponent: overlay.dropPanelComponent
}
