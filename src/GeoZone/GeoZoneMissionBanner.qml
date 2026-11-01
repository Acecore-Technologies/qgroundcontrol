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

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Plan View banner warning when the mission enters restricted geozones, centered between leftLimit and rightLimit
Rectangle {
    id:             control
    x:              leftLimit + ((_availableWidth - width) / 2)
    width:          Math.min(Math.max(warningLabel.implicitWidth, infoLabel.implicitWidth, outdatedLabel.implicitWidth, plannedLabel.implicitWidth, hintLabel.implicitWidth), _availableWidth - (_margin * 2)) + (_margin * 2)
    height:         column.height + (_margin * 2)
    radius:         ScreenTools.defaultFontPixelWidth / 2
    color:          qgcPal.window
    border.color:   (missionCheck.warningText !== "" || _outdatedText !== "") ? qgcPal.warningText : qgcPal.text
    z:              QGroundControl.zOrderWidgets
    visible:        missionCheck.warningText !== "" || missionCheck.infoText !== "" || _outdatedText !== ""

    required property var   missionController
    required property var   map                 ///< Fitted to the crossed zones when the banner is clicked
    property real           leftLimit:  0
    property real           rightLimit: parent ? parent.width : 0

    readonly property alias missionCheck:   check
    readonly property var   crossedZones:   check.crossedZones

    // Outdated data, like an old list of temporary restrictions, can miss zones the mission enters
    readonly property string _outdatedText: QGroundControl.geoZoneManager.validityWarning !== "" ?
                                                qsTr("Geozone data may be outdated: %1").arg(QGroundControl.geoZoneManager.validityWarning.replace(/\n/g, "; ")) : ""

    readonly property real _margin:         ScreenTools.defaultFontPixelWidth
    readonly property real _availableWidth: Math.max(rightLimit - leftLimit, ScreenTools.defaultFontPixelWidth * 20)

    /// Asks for confirmation before uploading a mission which enters prohibited zones, then calls uploadFunction
    function confirmUpload(uploadFunction) {
        // The banner updates after changes settle, so check the current plan
        check.update()
        var prohibitedZones = check.crossedZones.filter(zone => zone.restriction === GeoZone.Prohibited)
        if (prohibitedZones.length === 0) {
            uploadFunction()
            return
        }
        // The message dialog detects rich text, so markup in names from the files is defused
        var names = prohibitedZones.slice(0, 5).map(zone => zone.displayName.replace(/</g, "\u2039").replace(/>/g, "\u203A")).join(", ")
        if (prohibitedZones.length > 5) {
            names += qsTr(" and %1 more").arg(prohibitedZones.length - 5)
        }
        mainWindow.showMessageDialog(qsTr("Plan Upload"),
                                     qsTr("The mission path enters prohibited geozones: %1\n\nClick 'Ok' to upload the Plan anyway.").arg(names),
                                     Dialog.Ok | Dialog.Cancel,
                                     uploadFunction)
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    GeoZoneMissionCheck {
        id:                 check
        missionController:  control.missionController
    }

    Column {
        id:                 column
        anchors.centerIn:   parent
        width:              parent.width - (_margin * 2)

        QGCLabel {
            id:                     warningLabel
            width:                  parent.width
            wrapMode:               Text.WordWrap
            horizontalAlignment:    Text.AlignHCenter
            color:                  qgcPal.warningText
            text:                   check.warningText
            textFormat:             Text.PlainText
            visible:                text !== ""
        }

        QGCLabel {
            id:                     outdatedLabel
            width:                  parent.width
            wrapMode:               Text.WordWrap
            horizontalAlignment:    Text.AlignHCenter
            color:                  qgcPal.warningText
            text:                   control._outdatedText
            textFormat:             Text.PlainText
            visible:                text !== ""
        }

        QGCLabel {
            id:                     infoLabel
            width:                  parent.width
            wrapMode:               Text.WordWrap
            horizontalAlignment:    Text.AlignHCenter
            text:                   check.infoText
            textFormat:             Text.PlainText
            visible:                text !== ""
        }

        QGCLabel {
            id:                     plannedLabel
            width:                  parent.width
            wrapMode:               Text.WordWrap
            horizontalAlignment:    Text.AlignHCenter
            font.pointSize:         ScreenTools.smallFontPointSize
            text:                   qsTr("Checked for a flight starting %1 (local time)").arg(Qt.formatDateTime(QGroundControl.geoZoneManager.plannedStart, "yyyy-MM-dd HH:mm"))
            visible:                !isNaN(QGroundControl.geoZoneManager.plannedStart.getTime())
        }

        QGCLabel {
            id:                     hintLabel
            width:                  parent.width
            horizontalAlignment:    Text.AlignHCenter
            font.pointSize:         ScreenTools.smallFontPointSize
            text:                   qsTr("Click to show the zones")
            visible:                check.warningText !== ""
        }
    }

    QGCMouseArea {
        anchors.fill:   parent
        onClicked: {
            QGroundControl.geoZoneManager.visible = true
            if (check.crossedZonesBoundingBox.isValid) {
                map.fitViewportToGeoShape(check.crossedZonesBoundingBox, ScreenTools.defaultFontPixelHeight * 2)
            }
        }
    }
}
