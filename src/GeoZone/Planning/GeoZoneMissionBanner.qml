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

/// Plan View banner warning when the mission enters restricted geozones or the geozone data changed since the plan was
/// saved, centered between leftLimit and rightLimit. Exports the geozone briefing of the mission, see exportBriefing().
Rectangle {
    id:             control
    x:              leftLimit + ((_availableWidth - width) / 2)
    width:          Math.min(Math.max(warningLabel.implicitWidth, contingencyLabel.implicitWidth, infoLabel.implicitWidth, outdatedLabel.implicitWidth, auditLabel.implicitWidth, plannedLabel.implicitWidth, fenceLabel.implicitWidth, pendingLabel.implicitWidth, buttonRow.implicitWidth, hintLabel.implicitWidth), _availableWidth - (_margin * 2)) + (_margin * 2)
    height:         column.height + (_margin * 2)
    radius:         ScreenTools.defaultFontPixelWidth / 2
    color:          qgcPal.window
    border.color:   (missionCheck.warningText !== "" || _outdatedText !== "" || missionCheck.auditText !== "") ? qgcPal.warningText : qgcPal.text
    z:              QGroundControl.zOrderWidgets
    visible:        missionCheck.warningText !== "" || missionCheck.contingencyText !== "" || missionCheck.infoText !== "" || _outdatedText !== "" || missionCheck.fenceText !== "" || _pendingAuthorisations.length > 0 || missionCheck.auditText !== ""

    required property var   missionController
    required property var   map                 ///< Fitted to the crossed zones when the banner is clicked
    property var            geoFenceController: null    ///< Fence expected to exclude the prohibited zones, which can be added to it
    property real           leftLimit:  0
    property real           rightLimit: parent ? parent.width : 0

    readonly property alias missionCheck:   check
    readonly property var   crossedZones:   check.crossedZones

    // Outdated data, like an old list of temporary restrictions, can miss zones the mission enters
    readonly property string _outdatedText: GeoZoneManager.validityWarning !== "" ?
                                                qsTr("Geozone data may be outdated: %1").arg(GeoZoneManager.validityWarning.replace(/\n/g, "; ")) : ""

    readonly property var  _pendingAuthorisations: GeoZoneManager.planning.authorisations.pendingAuthorisations
    readonly property real _margin:         ScreenTools.defaultFontPixelWidth
    readonly property real _availableWidth: Math.max(rightLimit - leftLimit, ScreenTools.defaultFontPixelWidth * 20)

    /// Asks for confirmation before uploading a mission which enters prohibited zones without authorisation, or whose
    /// geozone data changed since the plan was saved, listing why, then calls uploadFunction unless cancelled
    function confirmUpload(uploadFunction) {
        // The banner updates after changes settle, so check the current plan
        check.update()
        const reasons = check.uploadReasons()
        if (reasons.length === 0) {
            uploadFunction()
            return
        }
        mainWindow.showMessageDialog(qsTr("Plan Upload"),
                                     qsTr("Confirm the upload of this plan:\n\n%1\n\nClick 'Ok' to upload the Plan anyway.")
                                        .arg(reasons.map(reason => "\u2022 " + control._plainText(reason)).join("\n\n")),
                                     Dialog.Ok | Dialog.Cancel,
                                     uploadFunction)
    }

    /// Asks for a file and exports the geozone briefing of the mission to it, as PDF, web page or JSON by its extension
    function exportBriefing() {
        briefingDialog.folder = briefingExport.defaultFolder()
        briefingDialog.fileName = briefingExport.defaultFileName()
        briefingDialog.openForSave()
    }

    /// Markup in names from the files is defused for the message dialog, which detects rich text
    function _plainText(text) {
        return text.replace(/</g, "\u2039").replace(/>/g, "\u203A")
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    GeoZoneBriefingExport {
        id:                 briefingExport
        missionController:  control.missionController
    }

    QGCFileDialog {
        id:             briefingDialog
        title:          qsTr("Export Geozone Briefing")
        nameFilters:    briefingExport.nameFilters
        defaultSuffix:  "pdf"

        onAcceptedForSave: (file) => {
            close()
            if (briefingExport.exportBriefing(file)) {
                mainWindow.showMessageDialog(qsTr("Export Geozone Briefing"), qsTr("Briefing saved to %1").arg(control._plainText(file)))
            } else {
                mainWindow.showMessageDialog(qsTr("Export Geozone Briefing"), control._plainText(briefingExport.errorString))
            }
        }
    }

    GeoZoneMissionCheck {
        id:                 check
        missionController:  control.missionController
        geoFenceController: control.geoFenceController
    }

    Component {
        id: routeDialogComponent

        GeoZoneRouteDialog { }
    }

    GeoZoneFenceImporter {
        id:                 fenceImporter
        geoFenceController: control.geoFenceController
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

        // Returns from along the mission after a failsafe or a fence breach, less likely than the planned path
        QGCLabel {
            id:                     contingencyLabel
            width:                  parent.width
            wrapMode:               Text.WordWrap
            horizontalAlignment:    Text.AlignHCenter
            text:                   check.contingencyText
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

        // The zones or sources changed since the plan was saved
        QGCLabel {
            id:                     auditLabel
            width:                  parent.width
            wrapMode:               Text.WordWrap
            horizontalAlignment:    Text.AlignHCenter
            color:                  qgcPal.warningText
            text:                   check.auditText
            textFormat:             Text.PlainText
            visible:                text !== ""
        }

        Row {
            anchors.horizontalCenter:   parent.horizontalCenter
            spacing:                    control._margin
            visible:                    auditLabel.visible

            QGCButton {
                text:       qsTr("Details")
                onClicked:  mainWindow.showMessageDialog(qsTr("Geozone Data Changes"), control._plainText(check.auditDetails.join("\n")))
            }

            QGCButton {
                text:       qsTr("Acknowledge")
                onClicked:  check.acknowledgeAudit()
            }
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
            text:                   qsTr("Checked for a flight starting %1 (local time)").arg(Qt.formatDateTime(GeoZoneManager.planning.plannedStart, "yyyy-MM-dd HH:mm"))
            visible:                !isNaN(GeoZoneManager.planning.plannedStart.getTime())
        }

        QGCLabel {
            id:                     fenceLabel
            width:                  parent.width
            wrapMode:               Text.WordWrap
            horizontalAlignment:    Text.AlignHCenter
            textFormat:             Text.PlainText
            color:                  qgcPal.warningText
            text:                   check.fenceText
            visible:                text !== ""
        }

        // Authorisations of an opened plan are the operator's choice to add
        QGCLabel {
            id:                     pendingLabel
            width:                  parent.width
            wrapMode:               Text.WordWrap
            horizontalAlignment:    Text.AlignHCenter
            textFormat:             Text.PlainText
            text:                   qsTr("The plan includes authorisations: %1. Add them to your authorisations?")
                                        .arg(control._pendingAuthorisations.map(authorisation => authorisation.permit !== "" ?
                                                qsTr("%1 (permit %2, %3)").arg(authorisation.zoneName).arg(authorisation.permit).arg(authorisation.statusText) :
                                                qsTr("%1 (%2)").arg(authorisation.zoneName).arg(authorisation.statusText)).join(", "))
            visible:                control._pendingAuthorisations.length > 0
        }

        Row {
            anchors.horizontalCenter:   parent.horizontalCenter
            spacing:                    control._margin
            visible:                    pendingLabel.visible

            QGCButton {
                text:       qsTr("Add")
                onClicked:  GeoZoneManager.planning.authorisations.acceptPending()
            }

            QGCButton {
                text:       qsTr("Ignore")
                onClicked:  GeoZoneManager.planning.authorisations.dismissPending()
            }
        }

        QGCButton {
            anchors.horizontalCenter:   parent.horizontalCenter
            text:                       qsTr("Add Exclusion Fences")
            visible:                    fenceLabel.visible
            onClicked:                  fenceImporter.addZones(check.unfencedZones)
        }

        Row {
            id:                         buttonRow
            anchors.horizontalCenter:   parent.horizontalCenter
            spacing:                    control._margin

            QGCButton {
                text:       qsTr("Route Around Zones...")
                visible:    check.warningText !== ""
                onClicked:  routeDialogComponent.createObject(mainWindow, { missionController: control.missionController }).open()
            }

            QGCButton {
                text:       qsTr("Export Briefing...")
                onClicked:  control.exportBriefing()
            }
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

    // Below the column, so its button gets its clicks
    QGCMouseArea {
        anchors.fill:   parent
        z:              -1
        onClicked: {
            GeoZoneManager.display.visible = true
            if (check.crossedZonesBoundingBox.isValid) {
                map.fitViewportToGeoShape(check.crossedZonesBoundingBox, ScreenTools.defaultFontPixelHeight * 2)
            }
        }
    }
}
