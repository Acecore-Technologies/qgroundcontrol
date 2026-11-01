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
import QGroundControl.Palette
import QGroundControl.ScreenTools
import QGroundControl.GeoZone

/// Number of loaded geozones, zones left out of the map and outdated data warning
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 4

    property int hiddenZoneCount: 0 ///< Smallest zones in the map view left out to limit the number of map items

    readonly property var _geoZoneManager:      GeoZoneManager
    readonly property int _activeSourceCount:   _geoZoneManager.sources.count - _geoZoneManager.disabledSourceCount

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        text:               _geoZoneManager.busy ? _geoZoneManager.progressText :
                                (_activeSourceCount === 1 ? qsTr("%1 zones from 1 source.").arg(_geoZoneManager.zoneCount) :
                                                            qsTr("%1 zones from %2 sources.").arg(_geoZoneManager.zoneCount).arg(_activeSourceCount)) +
                                (_geoZoneManager.disabledSourceCount > 0 ? " " + qsTr("%n source(s) turned off.", "", _geoZoneManager.disabledSourceCount) : "")
    }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        text:               qsTr("%1 of the smallest zones in view are not shown. Zoom in to see them.").arg(root.hiddenZoneCount)
        visible:            _geoZoneManager.display.visible && root.hiddenZoneCount > 0
    }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        color:              qgcPal.warningText
        text:               _geoZoneManager.validityWarning
        visible:            text !== ""
    }
}
