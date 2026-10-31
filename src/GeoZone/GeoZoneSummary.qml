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

/// Number of loaded geozones, zones left out of the map and outdated data warning
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 4

    property int hiddenZoneCount: 0 ///< Smallest zones in the map view left out to limit the number of map items

    readonly property var _geoZoneManager: QGroundControl.geoZoneManager

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        text:               _geoZoneManager.busy ? _geoZoneManager.progressText :
                                (_geoZoneManager.sources.count === 1 ? qsTr("%1 zones from 1 source.").arg(_geoZoneManager.zones.count) :
                                                                      qsTr("%1 zones from %2 sources.").arg(_geoZoneManager.zones.count).arg(_geoZoneManager.sources.count))
    }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        text:               qsTr("%1 of the smallest zones in view are not shown. Zoom in to see them.").arg(root.hiddenZoneCount)
        visible:            _geoZoneManager.visible && root.hiddenZoneCount > 0
    }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        color:              qgcPal.warningText
        text:               _geoZoneManager.validityWarning
        visible:            text !== ""
    }
}
