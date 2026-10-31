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

/// Geozones legend and display options for a map tool strip drop panel
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 2

    property int    hiddenZoneCount:    0       ///< Smallest zones in the map view left out to limit the number of map items
    property bool   showCrossedZones:   false   ///< See GeoZoneLegend

    signal manageRequested

    readonly property var   _geoZoneManager:    QGroundControl.geoZoneManager
    readonly property real  _textWidth:         ScreenTools.defaultFontPixelWidth * 32

    QGCCheckBoxSlider {
        Layout.fillWidth:   true
        text:               qsTr("Show geozones")
        checked:            _geoZoneManager.visible
        onClicked:          _geoZoneManager.visible = checked
    }

    GeoZoneLegend {
        showCrossedZones: root.showCrossedZones
    }

    QGCLabel {
        text:           qsTr("Click a zone name on the map for details.")
        font.pointSize: ScreenTools.smallFontPointSize
    }

    GeoZoneSummary {
        Layout.preferredWidth:  _textWidth
        hiddenZoneCount:        root.hiddenZoneCount
    }

    QGCButton {
        Layout.fillWidth:   true
        text:               qsTr("Manage Geozones...")
        onClicked:          root.manageRequested()
    }
}
