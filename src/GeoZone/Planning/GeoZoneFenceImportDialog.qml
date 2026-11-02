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
/// uploads the fence to the vehicle and verifies it. The explanations are behind the Help toggle, the zones left out
/// are listed on demand.
QGCPopupDialog {
    id:                     control
    title:                  qsTr("Fences from Geozones")
    buttons:                Dialog.Apply | Dialog.Cancel
    acceptButtonEnabled:    !_uploading

    required property var geoFenceController
    required property var flightMap

    readonly property var   _geoZoneManager:    GeoZoneManager
    readonly property real  _margin:            ScreenTools.defaultFontPixelHeight / 2
    // Narrower on small screens, so that the texts wrap rather than scroll sideways
    readonly property real  _contentWidth:      Math.min(ScreenTools.defaultFontPixelWidth * 56, mainWindow.width - ScreenTools.defaultFontPixelWidth * 8)
    readonly property var   _capacity:          importer.capacity
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

    // Rows of GeoZoneFenceChoiceModel
    Component {
        id: choiceDelegate

        RowLayout {
            id:         choiceRow
            width:      ListView.view ? ListView.view.width : implicitWidth
            spacing:    ScreenTools.defaultFontPixelWidth

            required property string    key
            required property string    name
            required property int       restriction
            required property string    restrictionText
            required property string    proximityText
            required property int       points
            required property real      simplifyTolerance
            required property bool      selected
            required property bool      included
            required property bool      includable
            required property string    reason
            required property bool      entered

            // Include the zone if it fits. Zones without a fence of their own can't be included.
            QGCCheckBox {
                checked:    choiceRow.includable && choiceRow.included
                enabled:    choiceRow.includable && !control._uploading
                onClicked: {
                    const included = checked
                    // Follows the zone again, which the importer updates right away
                    checked = Qt.binding(() => choiceRow.includable && choiceRow.included)
                    importer.setZoneIncluded(choiceRow.key, included)
                }
            }

            Rectangle {
                Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight * 0.75
                Layout.preferredHeight: Layout.preferredWidth
                radius:                 width / 2
                color:                  GeoZoneStyle.restrictionColor(choiceRow.restriction)
            }

            ColumnLayout {
                Layout.fillWidth:   true
                spacing:            0

                // The whole texts of elided rows
                ToolTip.visible:    rowHover.hovered && (nameLabel.truncated || detailLabel.truncated)
                ToolTip.text:       nameLabel.text + "\n" + detailLabel.text
                ToolTip.delay:      Qt.styleHints.mousePressAndHoldInterval

                HoverHandler { id: rowHover }

                QGCLabel {
                    id:                 nameLabel
                    Layout.fillWidth:   true
                    text:               choiceRow.name
                    textFormat:         Text.PlainText
                    elide:              Text.ElideRight
                }

                QGCLabel {
                    id:                 detailLabel
                    Layout.fillWidth:   true
                    text:               control._choiceDetail(choiceRow)
                    textFormat:         Text.PlainText
                    elide:              Text.ElideRight
                    font.pointSize:     ScreenTools.smallFontPointSize
                    // Only prohibited zones the flight enters stand out among those left out
                    color:              choiceRow.selected ? qgcPal.text :
                                            ((choiceRow.entered && choiceRow.restriction === GeoZone.Prohibited) ? qgcPal.warningText : qgcPal.colorGrey)
                }
            }
        }
    }

    ColumnLayout {
        spacing: _margin

        RowLayout {
            Layout.preferredWidth:  _contentWidth
            spacing:                _margin

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                text:               qsTr("Exclusion fences for the zones which fit, those the flight enters or passes near first.")
            }

            QGCButton {
                id:         helpButton
                text:       qsTr("Help")
                checkable:  true
            }
        }

        QGCLabel {
            Layout.preferredWidth:  _contentWidth
            wrapMode:               Text.WordWrap
            font.pointSize:         ScreenTools.smallFontPointSize
            visible:                helpButton.checked
            text:                   qsTr("Replaces the fences added from geozones with exclusion fences for the zones which fit into the vehicle's fence, those the flight enters or passes near first. Altitude limits and polygon holes are not transferred to the fence.")
        }

        GridLayout {
            Layout.preferredWidth:  _contentWidth
            columns:                2
            columnSpacing:          _margin
            rowSpacing:             _margin / 2

            QGCLabel {
                Layout.alignment:       Qt.AlignTop
                Layout.preferredHeight: prohibitedCheck.height
                verticalAlignment:      Text.AlignVCenter
                text:                   qsTr("Zone types")
            }
            // Wraps when the types don't fit next to each other
            Flow {
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
            visible:                helpButton.checked && simplifyCombo.currentIndex > 0
        }

        // Capacity of the vehicle's fence
        ColumnLayout {
            Layout.preferredWidth:  _contentWidth
            spacing:                0

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
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
                visible:            helpButton.checked && text !== ""
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
            Layout.preferredWidth:  _contentWidth
            wrapMode:               Text.WordWrap
            font.pointSize:         ScreenTools.smallFontPointSize
            text:                   qsTr("Checked zones are included in priority order while they fit, unchecked zones are left out. Uncheck a zone to make room for the zones after it. Zones without a fence of their own, like those with the same area as a zone before them, follow that zone and can't be checked.")
            visible:                helpButton.checked
        }

        RowLayout {
            Layout.preferredWidth:  _contentWidth
            spacing:                _margin

            QGCLabel {
                Layout.fillWidth:   true
                text:               importer.selectedCount > 0 ? qsTr("Selected zones (%1)").arg(importer.selectedCount) : qsTr("No zones selected")
                elide:              Text.ElideRight
                font.bold:          true
            }

            QGCLabel {
                text:       qsTr("Selecting zones...")
                visible:    importer.busy || selectionTimer.running
            }
        }

        QGCListView {
            Layout.preferredWidth:  _contentWidth
            Layout.preferredHeight: Math.min(contentHeight, ScreenTools.defaultFontPixelHeight * 12)
            clip:                   true
            spacing:                ScreenTools.defaultFontPixelHeight / 4
            model:                  GeoZoneSortFilterModel { sourceModel: importer.choices; filterRole: GeoZoneFenceChoiceModel.SelectedRole; filterRegularExpression: /true/ }
            visible:                importer.selectedCount > 0
            delegate:               choiceDelegate
        }

        // Collapsed, the zones which don't fit can be many
        RowLayout {
            Layout.preferredWidth:  _contentWidth
            spacing:                _margin
            visible:                importer.leftOutCount > 0

            QGCLabel {
                Layout.fillWidth:   true
                text:               importer.leftOutCount > leftOutList.count ?
                                        qsTr("%1 left out, the first %2 listed").arg(importer.leftOutCount).arg(leftOutList.count) :
                                        qsTr("%1 left out").arg(importer.leftOutCount)
                elide:              Text.ElideRight
                font.bold:          true
            }

            QGCButton {
                id:         leftOutButton
                text:       checked ? qsTr("Hide") : qsTr("Show")
                checkable:  true
            }
        }

        QGCListView {
            id:                     leftOutList
            Layout.preferredWidth:  _contentWidth
            Layout.preferredHeight: Math.min(contentHeight, ScreenTools.defaultFontPixelHeight * 8)
            clip:                   true
            spacing:                ScreenTools.defaultFontPixelHeight / 4
            model:                  GeoZoneSortFilterModel { sourceModel: importer.choices; filterRole: GeoZoneFenceChoiceModel.SelectedRole; filterRegularExpression: /false/ }
            visible:                importer.leftOutCount > 0 && leftOutButton.checked
            delegate:               choiceDelegate
        }

        RowLayout {
            Layout.preferredWidth:  _contentWidth
            spacing:                _margin

            QGCButton {
                text:       qsTr("Upload to Vehicle")
                enabled:    _upload.canUpload && !control._uploading && importer.selectedCount > 0
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
                visible:            _upload.state === GeoZoneFenceUpload.Idle && (helpButton.checked || !_upload.canUpload)
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
