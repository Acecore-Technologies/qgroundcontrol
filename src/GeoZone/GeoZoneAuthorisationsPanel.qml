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

/// Zones the operator is authorised to fly in, see GeoZoneAuthorisations
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 4

    readonly property var _authorisations: GeoZoneManager.authorisations

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        text:               qsTr("No authorisations. Add them from the details of a zone on the map.")
        visible:            root._authorisations.count === 0
    }

    Repeater {
        model: root._authorisations

        RowLayout {
            id:                 authorisationRow
            Layout.fillWidth:   true
            spacing:            ScreenTools.defaultFontPixelWidth

            required property string zoneKey
            required property string zoneName
            required property string permit
            required property string statusText

            ColumnLayout {
                Layout.fillWidth:   true
                spacing:            0

                QGCLabel {
                    Layout.fillWidth:   true
                    elide:              Text.ElideRight
                    text:               authorisationRow.zoneName !== "" ? authorisationRow.zoneName : authorisationRow.zoneKey
                    textFormat:         Text.PlainText
                }

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text:               authorisationRow.permit !== "" ? qsTr("%1, permit %2").arg(authorisationRow.statusText).arg(authorisationRow.permit) : authorisationRow.statusText
                    textFormat:         Text.PlainText
                }
            }

            QGCButton {
                text:       qsTr("Remove")
                onClicked:  root._authorisations.revoke(authorisationRow.zoneKey)
            }
        }
    }
}
