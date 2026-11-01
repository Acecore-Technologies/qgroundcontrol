/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Finds loaded geozones by name or identifier. Picking a result shows it on the visible map with its details.
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 4

    property int maxResults: 8

    signal zoneSelected     ///< A result was picked, the zone is shown on the map

    readonly property var _geoZoneManager: GeoZoneManager

    property var _results: []

    function _search() {
        // One more than shown, to tell that there are more matches
        _results = _geoZoneManager.searchZones(searchField.text, maxResults + 1)
    }

    QGCTextField {
        id:                 searchField
        Layout.fillWidth:   true
        placeholderText:    qsTr("Find a geozone by name or identifier")
        enabled:            root._geoZoneManager.zoneCount > 0
        onTextChanged:      searchTimer.restart()
    }

    Timer {
        id:             searchTimer
        interval:       300
        onTriggered:    root._search()
    }

    // The zones change when sources are reloaded
    Connections {
        target:                     root._geoZoneManager
        function onZonesChanged()   { root._search() }
    }

    QGCLabel {
        text:       qsTr("No geozones found")
        visible:    searchField.text.trim() !== "" && root._results.length === 0 && !searchTimer.running
    }

    Repeater {
        model: root._results.slice(0, root.maxResults)

        Rectangle {
            id:                     resultRow
            Layout.fillWidth:       true
            Layout.preferredHeight: resultLayout.implicitHeight + ScreenTools.defaultFontPixelHeight / 4
            color:                  resultMouseArea.containsMouse ? qgcPal.buttonHighlight : "transparent"
            radius:                 ScreenTools.defaultFontPixelHeight / 4

            required property var modelData

            RowLayout {
                id:                     resultLayout
                anchors.left:           parent.left
                anchors.right:          parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins:        ScreenTools.defaultFontPixelWidth / 2
                spacing:                ScreenTools.defaultFontPixelWidth

                Rectangle {
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight * 0.75
                    Layout.preferredHeight: Layout.preferredWidth
                    radius:                 width / 2
                    color:                  GeoZoneStyle.restrictionColor(resultRow.modelData.restriction)
                }

                ColumnLayout {
                    Layout.fillWidth:   true
                    spacing:            0

                    QGCLabel {
                        Layout.fillWidth:   true
                        text:               resultRow.modelData.displayName
                        textFormat:         Text.PlainText
                        elide:              Text.ElideRight
                    }

                    QGCLabel {
                        Layout.fillWidth:   true
                        text:               resultRow.modelData.identifier + " - " + resultRow.modelData.restrictionText
                        textFormat:         Text.PlainText
                        elide:              Text.ElideRight
                        font.pointSize:     ScreenTools.smallFontPointSize
                    }
                }
            }

            MouseArea {
                id:             resultMouseArea
                anchors.fill:   parent
                hoverEnabled:   true
                onClicked: {
                    root._geoZoneManager.showZone(resultRow.modelData)
                    root.zoneSelected()
                }
            }
        }
    }

    QGCLabel {
        text:           qsTr("More zones match, refine the search")
        visible:        root._results.length > root.maxResults
        font.pointSize: ScreenTools.smallFontPointSize
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }
}
