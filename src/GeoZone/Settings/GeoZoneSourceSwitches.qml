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
import QGroundControl.ScreenTools
import QGroundControl.GeoZone

/// Turns geozone sources on or off. The zones of sources turned off are left out of the maps, checks and alerts.
/// Shown with several sources, or when one is off.
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 4
    visible:    _geoZoneManager.sources.count > 1 || _geoZoneManager.disabledSourceCount > 0

    readonly property var _geoZoneManager: GeoZoneManager

    QGCLabel { text: qsTr("Sources") }

    Repeater {
        model: root._geoZoneManager.sources

        RowLayout {
            id:                 sourceRow
            Layout.fillWidth:   true
            spacing:            ScreenTools.defaultFontPixelWidth

            required property var object

            QGCCheckBox {
                checked:    sourceRow.object.enabled
                onClicked:  sourceRow.object.enabled = checked
            }

            QGCLabel {
                Layout.fillWidth:   true
                elide:              Text.ElideRight
                text:               qsTr("%1 (%2)").arg(sourceRow.object.name).arg(sourceRow.object.zoneCount)
                textFormat:         Text.PlainText
                opacity:            sourceRow.object.enabled ? 1 : 0.5

                MouseArea {
                    anchors.fill:   parent
                    onClicked:      sourceRow.object.enabled = !sourceRow.object.enabled
                }
            }
        }
    }
}
