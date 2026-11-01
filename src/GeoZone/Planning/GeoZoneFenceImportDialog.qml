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
import QtQuick.Dialogs
import QtQuick.Layouts
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Fits loaded geozones into the vehicle's fence as exclusion fences, the zones of the planned flight first, and
/// uploads the fence to the vehicle and verifies it
QGCPopupDialog {
    id:                     control
    title:                  qsTr("Fences from Geozones")
    buttons:                Dialog.Apply | Dialog.Cancel
    acceptButtonEnabled:    !_uploading

    required property var geoFenceController
    required property var flightMap

    readonly property var   _geoZoneManager:    GeoZoneManager
    readonly property real  _margin:            ScreenTools.defaultFontPixelHeight / 2
    readonly property real  _contentWidth:      ScreenTools.defaultFontPixelWidth * 56
    readonly property var   _capacity:          importer.capacity
    readonly property var   _selected:          importer.choices.filter(choice => choice.selected)
    readonly property var   _leftOut:           importer.choices.filter(choice => !choice.selected)
    readonly property var   _upload:            importer.upload
    readonly property bool  _uploading:         _upload.busy

    // The dialog shows before the first selection is computed
    Component.onCompleted: selectionTimer.start()

    onAccepted: {
        if (selectionTimer.running) {
            selectionTimer.stop()
            _updateSelection()
        }
        importer.applySelection()
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    GeoZoneFenceImporter {
        id:                 importer
        geoFenceController: control.geoFenceController
    }

    // Selections are computed after changes settle since simplifying many zones takes a moment
    Timer {
        id:             selectionTimer
        interval:       300
        onTriggered:    control._updateSelection()
    }

    Connections {
        target:                     _geoZoneManager
        function onZonesChanged()   { selectionTimer.restart() }
    }

    function _horizontalDistanceText(meters) {
        const distance = QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnits(meters)
        return Math.round(distance) + " " + QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString
    }

    function _restrictions() {
        const restrictions = []
        if (prohibitedCheck.checked) {
            restrictions.push(GeoZone.Prohibited)
        }
        if (reqAuthorisationCheck.checked) {
            restrictions.push(GeoZone.ReqAuthorisation)
        }
        if (conditionalCheck.checked) {
            restrictions.push(GeoZone.Conditional)
        }
        return restrictions
    }

    function _updateSelection() {
        const area = mapViewOnlyCheck.checked ? flightMap.visibleRegion.boundingGeoRectangle() : QtPositioning.rectangle()
        importer.updateSelection(_restrictions(), area, activeCombo.hours[activeCombo.currentIndex], simplifyCombo.tolerances[simplifyCombo.currentIndex])
    }

    function _choiceDetail(choice) {
        const parts = [ choice.restrictionText ]
        if (choice.proximityText !== "") {
            parts.push(choice.proximityText)
        }
        if (choice.selected) {
            parts.push(qsTr("%n point(s)", "", choice.points))
            if (choice.simplifyTolerance > 0) {
                parts.push(qsTr("simplified %1").arg(_horizontalDistanceText(choice.simplifyTolerance)))
            }
        } else {
            parts.push(choice.reason)
        }
        return parts.join(" · ")
    }

    Component {
        id: choiceDelegate

        RowLayout {
            id:         choiceRow
            width:      ListView.view ? ListView.view.width : implicitWidth
            spacing:    ScreenTools.defaultFontPixelWidth

            required property var modelData

            QGCCheckBox {
                checked:    choiceRow.modelData.included
                enabled:    !control._uploading
                onClicked:  importer.setZoneIncluded(choiceRow.modelData.key, checked)
            }

            Rectangle {
                Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight * 0.75
                Layout.preferredHeight: Layout.preferredWidth
                radius:                 width / 2
                color:                  GeoZoneStyle.restrictionColor(choiceRow.modelData.restriction)
            }

            ColumnLayout {
                Layout.fillWidth:   true
                spacing:            0

                QGCLabel {
                    Layout.fillWidth:   true
                    text:               choiceRow.modelData.name
                    textFormat:         Text.PlainText
                    elide:              Text.ElideRight
                }

                QGCLabel {
                    Layout.fillWidth:   true
                    text:               control._choiceDetail(choiceRow.modelData)
                    textFormat:         Text.PlainText
                    elide:              Text.ElideRight
                    font.pointSize:     ScreenTools.smallFontPointSize
                    color:              choiceRow.modelData.selected ? qgcPal.text : qgcPal.warningText
                }
            }
        }
    }

    ColumnLayout {
        spacing: _margin

        QGCLabel {
            Layout.preferredWidth:  _contentWidth
            wrapMode:               Text.WordWrap
            text:                   qsTr("Replaces the fences added from geozones with exclusion fences for the zones which fit into the vehicle's fence, those the flight enters or passes near first. Altitude limits and polygon holes are not transferred to the fence.")
        }

        GridLayout {
            Layout.preferredWidth:  _contentWidth
            columns:                2
            columnSpacing:          _margin
            rowSpacing:             _margin / 2

            QGCLabel { text: qsTr("Zone types") }
            RowLayout {
                Layout.fillWidth:   true
                spacing:            _margin

                QGCCheckBox {
                    id:                 prohibitedCheck
                    text:               qsTr("Prohibited")
                    checked:            true
                    onClicked:          selectionTimer.restart()
                }
                QGCCheckBox {
                    id:                 reqAuthorisationCheck
                    text:               qsTr("Authorisation")
                    checked:            true
                    onClicked:          selectionTimer.restart()
                }
                QGCCheckBox {
                    id:                 conditionalCheck
                    text:               qsTr("Conditional")
                    checked:            false
                    onClicked:          selectionTimer.restart()
                }
            }

            QGCLabel { text: qsTr("Area") }
            QGCCheckBox {
                id:                 mapViewOnlyCheck
                Layout.fillWidth:   true
                text:               qsTr("Only zones in current map view")
                checked:            true
                onClicked:          selectionTimer.restart()
            }

            QGCLabel { text: qsTr("Active") }
            QGCComboBox {
                id:                 activeCombo
                Layout.fillWidth:   true
                model:              [ qsTr("Any time"), qsTr("Now"), qsTr("Next 3 hours"), qsTr("Next 24 hours") ]
                onActivated:        selectionTimer.restart()

                readonly property var hours: [ -1, 0, 3, 24 ]
            }

            QGCLabel { text: qsTr("Simplify up to") }
            QGCComboBox {
                id:                 simplifyCombo
                Layout.fillWidth:   true
                currentIndex:       3
                model:              tolerances.map(meters => meters === 0 ? qsTr("Off") : control._horizontalDistanceText(meters))
                onActivated:        selectionTimer.restart()

                readonly property var tolerances: [ 0, 10, 25, 50, 100, 250 ]
            }
        }

        QGCLabel {
            Layout.preferredWidth:  _contentWidth
            wrapMode:               Text.WordWrap
            font.pointSize:         ScreenTools.smallFontPointSize
            text:                   qsTr("Fences are simplified only as far as needed to fit. Simplified fences enclose the zone, extending up to twice the tolerance beyond it.")
            visible:                simplifyCombo.currentIndex > 0
        }

        // Capacity of the vehicle's fence
        ColumnLayout {
            Layout.preferredWidth:  _contentWidth
            spacing:                0

            QGCLabel {
                Layout.fillWidth:   true
                text:               _capacity.text ?? ""
                font.bold:          true
            }

            ProgressBar {
                Layout.fillWidth:   true
                from:               0
                to:                 Math.max(_capacity.maxItems ?? 0, 1)
                value:              _capacity.usedItems ?? 0
                visible:            (_capacity.maxItems ?? 0) > 0
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               (_capacity.note ?? "") + ((_capacity.keptItems ?? 0) > 0 ? " " + qsTr("Drawn fences and the breach return point take %n point(s).", "", _capacity.keptItems) : "")
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                color:              qgcPal.warningText
                text:               _capacity.warning ?? ""
                visible:            text !== ""
            }
        }

        QGCLabel {
            text:       _selected.length > 0 ? qsTr("Selected zones (%1)").arg(_selected.length) : qsTr("No zones selected")
            font.bold:  true
        }

        QGCListView {
            Layout.preferredWidth:  _contentWidth
            Layout.preferredHeight: Math.min(contentHeight, ScreenTools.defaultFontPixelHeight * 12)
            clip:                   true
            spacing:                ScreenTools.defaultFontPixelHeight / 4
            model:                  _selected
            visible:                _selected.length > 0
            delegate:               choiceDelegate
        }

        QGCLabel {
            text:       qsTr("Left out (%1)").arg(_leftOut.length)
            font.bold:  true
            visible:    _leftOut.length > 0
        }

        QGCListView {
            Layout.preferredWidth:  _contentWidth
            Layout.preferredHeight: Math.min(contentHeight, ScreenTools.defaultFontPixelHeight * 8)
            clip:                   true
            spacing:                ScreenTools.defaultFontPixelHeight / 4
            model:                  _leftOut
            visible:                _leftOut.length > 0
            delegate:               choiceDelegate
        }

        RowLayout {
            Layout.preferredWidth:  _contentWidth
            spacing:                _margin

            QGCButton {
                text:       qsTr("Upload to Vehicle")
                enabled:    _upload.canUpload && !control._uploading && _selected.length > 0
                onClicked: {
                    if (selectionTimer.running) {
                        selectionTimer.stop()
                        control._updateSelection()
                    }
                    importer.uploadToVehicle()
                }
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                text:               _upload.canUpload ? qsTr("Adds the fences to the plan and uploads the plan's fence, then reads it back to verify it.") : qsTr("Connect a vehicle supporting fences to upload.")
                font.pointSize:     ScreenTools.smallFontPointSize
                visible:            _upload.state === GeoZoneFenceUpload.Idle
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                text:               _upload.message
                textFormat:         Text.PlainText
                visible:            _upload.state !== GeoZoneFenceUpload.Idle
                color:              (_upload.state === GeoZoneFenceUpload.Failed || _upload.state === GeoZoneFenceUpload.Mismatch || _upload.state === GeoZoneFenceUpload.Aborted) ? qgcPal.warningText : qgcPal.text
            }
        }
    }
}
