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
import QGroundControl.ScreenTools
import QGroundControl.GeoZone

/// Turns geozone sources on or off. The zones of sources turned off are left out of the maps, checks and alerts.
/// Shown with several sources, or when one is off. Many sources scroll, long names are elided with a tool tip.
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 4
    visible:    _geoZoneManager.sources.count > 1 || _geoZoneManager.disabledSourceCount > 0

    property real maxListHeight: ScreenTools.defaultFontPixelHeight * 12   ///< The list scrolls beyond

    readonly property var _geoZoneManager: GeoZoneManager

    QGCLabel { text: qsTr("Sources (%1)").arg(root._geoZoneManager.sources.count) }

    QGCListView {
        id:                     sourceList
        Layout.fillWidth:       true
        Layout.preferredHeight: Math.min(contentHeight, root.maxListHeight)
        implicitWidth:          ScreenTools.defaultFontPixelWidth * 30
        spacing:                ScreenTools.defaultFontPixelHeight / 4
        model:                  root._geoZoneManager.sources

        delegate: RowLayout {
            id:         sourceRow
            // Room for the scroll indicator
            width:      ListView.view.width - ScreenTools.defaultFontPixelWidth
            spacing:    ScreenTools.defaultFontPixelWidth

            required property var object

            QGCCheckBox {
                checked:    sourceRow.object.enabled
                onClicked:  sourceRow.object.enabled = checked
            }

            QGCLabel {
                id:                 sourceLabel
                Layout.fillWidth:   true
                elide:              Text.ElideRight
                text:               qsTr("%1 (%2)").arg(sourceRow.object.name).arg(sourceRow.object.zoneCount)
                textFormat:         Text.PlainText
                opacity:            sourceRow.object.enabled ? 1 : 0.5

                MouseArea {
                    id:                 labelMouseArea
                    anchors.fill:       parent
                    hoverEnabled:       true
                    onClicked:          sourceRow.object.enabled = !sourceRow.object.enabled

                    ToolTip.visible:    containsMouse && sourceLabel.truncated
                    ToolTip.text:       sourceLabel.text
                    ToolTip.delay:      500
                }
            }
        }
    }
}
