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

/// Adds loaded geozones to the plan as exclusion fences
QGCPopupDialog {
    id:                     control
    title:                  qsTr("Add Fences from Geozones")
    buttons:                Dialog.Ok | Dialog.Cancel
    acceptButtonEnabled:    _restrictions().length > 0 && _preview.fenceCount > 0

    required property var geoFenceController
    required property var flightMap

    readonly property var   _geoZoneManager:    GeoZoneManager
    readonly property real  _margin:            ScreenTools.defaultFontPixelHeight / 2
    readonly property real  _contentWidth:      ScreenTools.defaultFontPixelWidth * 50

    property var _preview: ({ fenceCount: 0, pointCount: 0, totalPointCount: 0, limitWarning: "" })

    // The dialog shows before the first preview is computed
    Component.onCompleted: previewTimer.start()

    onAccepted: {
        // The importer reuses the fences of a preview with the same choices
        if (previewTimer.running) {
            previewTimer.stop()
            _updatePreview()
        }
        const preview = _preview
        if (preview.limitWarning === "") {
            _import()
            return
        }
        preventClose = true
        mainWindow.showMessageDialog(title, preview.limitWarning + "\n\n" + qsTr("Add the fences anyway?"),
                                     Dialog.Ok | Dialog.Cancel,
                                     function() {
                                         control._import()
                                         control.close()
                                     })
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    GeoZoneFenceImporter {
        id:                 importer
        geoFenceController: control.geoFenceController
    }

    // Previews are computed after changes settle since simplifying many zones takes a moment
    Timer {
        id:             previewTimer
        interval:       300
        onTriggered:    control._updatePreview()
    }

    Connections {
        target:                     _geoZoneManager
        function onZonesChanged()   { previewTimer.restart() }
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

    function _area() {
        return mapViewOnlyCheck.checked ? flightMap.visibleRegion.boundingGeoRectangle() : QtPositioning.rectangle()
    }

    function _activeWithinHours() {
        return activeCombo.hours[activeCombo.currentIndex]
    }

    function _tolerance() {
        return simplifyCombo.tolerances[simplifyCombo.currentIndex]
    }

    function _updatePreview() {
        _preview = importer.preview(_restrictions(), _area(), _activeWithinHours(), _tolerance())
    }

    function _import() {
        const fenceCount = importer.importZones(_restrictions(), _area(), _activeWithinHours(), _tolerance())
        if (fenceCount === 0) {
            mainWindow.showMessageDialog(title, qsTr("No new matching zones found."))
        }
    }

    ColumnLayout {
        spacing: _margin

        QGCLabel {
            Layout.preferredWidth:  _contentWidth
            wrapMode:               Text.WordWrap
            text:                   qsTr("Adds the selected zone types as exclusion fences. Altitude limits and polygon holes are not transferred to the fence, and vehicles can only store a limited number of fence points.")
        }

        QGCLabel { text: qsTr("Zone types") }

        QGCCheckBox {
            id:                 prohibitedCheck
            text:               qsTr("Prohibited")
            checked:            true
            onCheckedChanged:   previewTimer.restart()
        }

        QGCCheckBox {
            id:                 reqAuthorisationCheck
            text:               qsTr("Authorisation required")
            checked:            true
            onCheckedChanged:   previewTimer.restart()
        }

        QGCCheckBox {
            id:                 conditionalCheck
            text:               qsTr("Conditional")
            checked:            false
            onCheckedChanged:   previewTimer.restart()
        }

        GridLayout {
            Layout.preferredWidth:  _contentWidth
            columns:                2
            columnSpacing:          _margin
            rowSpacing:             _margin

            QGCLabel { text: qsTr("Area") }
            QGCCheckBox {
                id:                 mapViewOnlyCheck
                Layout.fillWidth:   true
                text:               qsTr("Only zones in current map view")
                checked:            true
                onCheckedChanged:   previewTimer.restart()
            }

            QGCLabel { text: qsTr("Active") }
            QGCComboBox {
                id:                 activeCombo
                Layout.fillWidth:   true
                model:              [ qsTr("Any time"), qsTr("Now"), qsTr("Next 3 hours"), qsTr("Next 24 hours") ]
                onActivated:        previewTimer.restart()

                readonly property var hours: [ -1, 0, 3, 24 ]
            }

            QGCLabel { text: qsTr("Simplify") }
            QGCComboBox {
                id:                 simplifyCombo
                Layout.fillWidth:   true
                currentIndex:       2
                model:              tolerances.map(meters => meters === 0 ? qsTr("Off") : control._horizontalDistanceText(meters))
                onActivated:        previewTimer.restart()

                readonly property var tolerances: [ 0, 10, 25, 50, 100 ]
            }
        }

        QGCLabel {
            Layout.preferredWidth:  _contentWidth
            wrapMode:               Text.WordWrap
            font.pointSize:         ScreenTools.smallFontPointSize
            text:                   qsTr("Simplified fences have fewer points and enclose the zone, extending up to twice the tolerance beyond it.")
            visible:                simplifyCombo.currentIndex > 0
        }

        QGCLabel {
            Layout.preferredWidth:  _contentWidth
            wrapMode:               Text.WordWrap
            text:                   _preview.fenceCount > 0 ?
                                        qsTr("Adds %1 fences with %2 points (%3 fence points in total).").arg(_preview.fenceCount).arg(_preview.pointCount).arg(_preview.totalPointCount) :
                                        qsTr("No new matching zones.")
        }

        QGCLabel {
            Layout.preferredWidth:  _contentWidth
            wrapMode:               Text.WordWrap
            color:                  qgcPal.warningText
            text:                   _preview.limitWarning
            visible:                text !== ""
        }
    }
}
