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

/// The zones the operator drew, with buttons to draw, edit and remove them, see GeoZoneOperatorZones. The map shows
/// the zone edited with GeoZoneOperatorZoneEditor.
ColumnLayout {
    id:         section
    spacing:    ScreenTools.defaultFontPixelHeight / 4

    property var map: null  ///< Map whose view the new zones are drawn in, without it none can be drawn

    signal drawingStarted   ///< A zone is drawn or edited on the map

    readonly property var _zones: GeoZoneManager.operatorZones
    property string       _error    ///< Why the last change of the zones failed, empty when it didn't

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    Connections {
        target: section._zones
        // While a zone is edited its form shows why it can't be saved
        function onFailed(message) {
            if (!section._zones.editing) {
                section._error = message
            }
        }
        function onZonesChanged() { section._error = "" }
        function onEditingChanged() { section._error = "" }
    }

    QGCLabel {
        Layout.fillWidth:   true
        text:               qsTr("My zones")
    }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        font.pointSize:     ScreenTools.smallFontPointSize
        text:               section._zones.zones.length === 0 ? qsTr("Draw zones of your own: they are shown, checked and alerted like the others.") :
                                                                qsTr("Shown, checked and alerted like the other zones, and kept as the source \"My zones\".")
    }

    // The zones in the file which couldn't be read, and why the last zone couldn't be removed
    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        textFormat:         Text.PlainText
        color:              qgcPal.warningText
        text:               section._zones.loadError
        visible:            text !== ""
    }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        textFormat:         Text.PlainText
        color:              qgcPal.warningText
        text:               section._error
        visible:            text !== ""
    }

    Repeater {
        model: section._zones.zones

        RowLayout {
            id:                 zoneRow
            Layout.fillWidth:   true

            required property var   modelData
            required property int   index

            Rectangle {
                Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight * 0.75
                Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 0.75
                color:                  GeoZoneStyle.restrictionColor(zoneRow.modelData.restriction)
                radius:                 zoneRow.modelData.circle ? height / 2 : 0
            }

            QGCLabel {
                Layout.fillWidth:   true
                textFormat:         Text.PlainText
                text:               zoneRow.modelData.name
                elide:              Text.ElideRight
            }

            QGCButton {
                text:       qsTr("Edit")
                enabled:    !section._zones.editing
                onClicked: {
                    section._zones.startEdit(zoneRow.index)
                    section.drawingStarted()
                }
            }

            QGCButton {
                text:       qsTr("Remove")
                enabled:    !section._zones.editing
                onClicked: {
                    section._error = ""
                    section._zones.removeZone(zoneRow.index)
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Draw Polygon")
            enabled:            !section._zones.editing && !!section.map
            onClicked: {
                section._zones.startPolygon(section.map.visibleRegion.boundingGeoRectangle())
                section.drawingStarted()
            }
        }

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Draw Circle")
            enabled:            !section._zones.editing && !!section.map
            onClicked: {
                section._zones.startCircle(section.map.visibleRegion.boundingGeoRectangle())
                section.drawingStarted()
            }
        }
    }
}
