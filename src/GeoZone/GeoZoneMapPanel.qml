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
    signal zoneSelected     ///< A zone picked in the search is shown on the map

    readonly property var   _geoZoneManager:    QGroundControl.geoZoneManager
    readonly property real  _textWidth:         ScreenTools.defaultFontPixelWidth * 32

    QGCCheckBoxSlider {
        Layout.fillWidth:   true
        text:               qsTr("Show geozones")
        checked:            root._geoZoneManager.visible
        onClicked:          root._geoZoneManager.visible = checked
    }

    GeoZoneLegend {
        showCrossedZones: root.showCrossedZones
    }

    QGCLabel {
        Layout.preferredWidth:  root._textWidth
        text:                   qsTr("Click a zone on the map for its details, when not adding items.")
        wrapMode:               Text.WordWrap
        font.pointSize:         ScreenTools.smallFontPointSize
    }

    GeoZoneSearch {
        Layout.preferredWidth:  root._textWidth
        onZoneSelected:         root.zoneSelected()
    }

    GeoZoneSummary {
        Layout.preferredWidth:  root._textWidth
        hiddenZoneCount:        root.hiddenZoneCount
    }

    QGCButton {
        Layout.fillWidth:   true
        text:               qsTr("Manage Geozones...")
        onClicked:          root.manageRequested()
    }
}
