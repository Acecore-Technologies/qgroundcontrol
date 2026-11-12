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

import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Finds loaded geozones by name or identifier, ignoring case and accents (see GeoZoneCatalog::search). Picking a
/// result shows it on the visible map with its details.
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 4

    property int maxResults: 8

    signal zoneSelected     ///< A result was picked, the zone is shown on the map

    readonly property var _geoZoneManager: GeoZoneManager

    GeoZoneSearchModel {
        id:         searchModel
        text:       searchField.text
        maxResults: root.maxResults
    }

    QGCTextField {
        id:                 searchField
        Layout.fillWidth:   true
        placeholderText:    qsTr("Find a geozone by name or identifier")
        enabled:            root._geoZoneManager.zoneCount > 0
    }

    QGCLabel {
        text:       qsTr("No geozones found")
        visible:    searchField.text.trim() !== "" && searchModel.count === 0 && !searchModel.pending
    }

    ListView {
        Layout.fillWidth:       true
        Layout.preferredHeight: contentHeight
        interactive:            false
        model:                  searchModel
        visible:                count > 0

        delegate: Rectangle {
            id:         resultRow
            width:      ListView.view.width
            height:     Math.max(resultLayout.implicitHeight + ScreenTools.defaultFontPixelHeight / 4, ScreenTools.implicitButtonHeight)
            color:      resultMouseArea.containsMouse ? qgcPal.buttonHighlight : "transparent"
            radius:     ScreenTools.defaultFontPixelHeight / 4

            required property var       zone
            required property string    displayName
            required property string    identifier
            required property int       restriction
            required property string    restrictionText
            required property int       sourceCount

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
                    color:                  GeoZoneStyle.restrictionColor(resultRow.restriction)
                }

                ColumnLayout {
                    Layout.fillWidth:   true
                    spacing:            0

                    QGCLabel {
                        Layout.fillWidth:   true
                        text:               resultRow.displayName
                        textFormat:         Text.PlainText
                        elide:              Text.ElideRight
                    }

                    QGCLabel {
                        Layout.fillWidth:   true
                        text:               resultRow.identifier + " - " + resultRow.restrictionText +
                                                (resultRow.sourceCount > 1 ? " · " + qsTr("%n source(s)", "", resultRow.sourceCount) : "")
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
                    root._geoZoneManager.showZone(resultRow.zone)
                    root.zoneSelected()
                }
            }
        }
    }

    QGCLabel {
        text:           qsTr("More zones match, refine the search")
        visible:        searchModel.hasMore
        font.pointSize: ScreenTools.smallFontPointSize
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }
}
