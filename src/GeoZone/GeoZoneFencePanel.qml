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
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Geozones section of the GeoFence editor: shows the loaded zones and adds them as exclusion fences
Column {
    id:         control
    spacing:    _margin

    required property var   geoFenceController
    required property var   flightMap
    property int            hiddenZoneCount: 0  ///< Smallest geozones in the map view left out to limit the number of map items

    readonly property var   _geoZoneManager:    QGroundControl.geoZoneManager
    readonly property bool  _zonesLoaded:       _geoZoneManager.zones.count > 0
    readonly property real  _margin:            ScreenTools.defaultFontPixelWidth / 2

    property var _importPreview: ({ fenceCount: 0, pointCount: 0, totalPointCount: 0, limitWarning: "" })
    readonly property bool _importVisible: visible && _zonesLoaded && geoFenceController.supported

    on_ImportVisibleChanged: importPreviewTimer.restart()

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        anchors.left:       parent.left
        anchors.right:      parent.right
        wrapMode:           Text.WordWrap
        font.pointSize:     ScreenTools.smallFontPointSize
        text:               _geoZoneManager.sources.count === 0 ?
                                qsTr("Geozone files and national feeds show UAS geographical zones on the maps.") :
                                (_geoZoneManager.busy ? _geoZoneManager.progressText : qsTr("%1 zones from %2 sources.").arg(_geoZoneManager.zones.count).arg(_geoZoneManager.sources.count))
    }

    QGCLabel {
        anchors.left:       parent.left
        anchors.right:      parent.right
        wrapMode:           Text.WordWrap
        font.pointSize:     ScreenTools.smallFontPointSize
        color:              qgcPal.warningText
        text:               _geoZoneManager.validityWarning
        visible:            text !== ""
    }

    QGCLabel {
        anchors.left:       parent.left
        anchors.right:      parent.right
        wrapMode:           Text.WordWrap
        font.pointSize:     ScreenTools.smallFontPointSize
        text:               qsTr("%1 of the smallest zones in view are not shown. Zoom in to see them.").arg(hiddenZoneCount)
        visible:            _zonesLoaded && _geoZoneManager.visible && hiddenZoneCount > 0
    }

    QGCButton {
        anchors.left:       parent.left
        anchors.right:      parent.right
        text:               qsTr("Manage Geozones...")
        onClicked:          mainWindow.showSettingsTool(qsTr("Geozones"))
    }

    QGCCheckBox {
        text:       qsTr("Show geozones on map")
        checked:    _geoZoneManager.visible
        visible:    _zonesLoaded
        onClicked:  _geoZoneManager.visible = checked
    }

    Column {
        anchors.left:   parent.left
        anchors.right:  parent.right
        spacing:        _margin
        visible:        _zonesLoaded && geoFenceController.supported

        QGCLabel {
            anchors.left:       parent.left
            anchors.right:      parent.right
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            text:               qsTr("Add the selected zone types as exclusion fences. Altitude limits and polygon holes are not transferred to the fence, and vehicles can only store a limited number of fence points.")
        }

        QGCCheckBox {
            id:                 importProhibitedCheck
            text:               qsTr("Prohibited")
            checked:            true
            onCheckedChanged:   importPreviewTimer.restart()
        }

        QGCCheckBox {
            id:                 importReqAuthorisationCheck
            text:               qsTr("Authorisation required")
            checked:            true
            onCheckedChanged:   importPreviewTimer.restart()
        }

        QGCCheckBox {
            id:                 importConditionalCheck
            text:               qsTr("Conditional")
            checked:            false
            onCheckedChanged:   importPreviewTimer.restart()
        }

        QGCCheckBox {
            id:                 importMapViewOnlyCheck
            text:               qsTr("Only zones in current map view")
            checked:            true
            onCheckedChanged:   importPreviewTimer.restart()
        }

        GridLayout {
            anchors.left:       parent.left
            anchors.right:      parent.right
            columns:            2
            columnSpacing:      _margin
            rowSpacing:         _margin

            QGCLabel { text: qsTr("Active") }
            QGCComboBox {
                id:                 importActiveCombo
                Layout.fillWidth:   true
                model:              [ qsTr("Any time"), qsTr("Now"), qsTr("Next 3 hours"), qsTr("Next 24 hours") ]
                onActivated:        importPreviewTimer.restart()

                readonly property var hours: [ -1, 0, 3, 24 ]
            }

            QGCLabel { text: qsTr("Simplify") }
            QGCComboBox {
                id:                 importSimplifyCombo
                Layout.fillWidth:   true
                currentIndex:       2
                model:              tolerances.map(meters => meters === 0 ? qsTr("Off") : control._horizontalDistanceText(meters))
                onActivated:        importPreviewTimer.restart()

                readonly property var tolerances: [ 0, 10, 25, 50, 100 ]
            }
        }

        QGCLabel {
            anchors.left:       parent.left
            anchors.right:      parent.right
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            text:               qsTr("Simplified fences have fewer points and enclose the zone, extending up to twice the tolerance beyond it.")
            visible:            importSimplifyCombo.currentIndex > 0
        }

        QGCLabel {
            anchors.left:       parent.left
            anchors.right:      parent.right
            wrapMode:           Text.WordWrap
            text:               _importPreview.fenceCount > 0 ?
                                    qsTr("Adds %1 fences with %2 points (%3 fence points in total).").arg(_importPreview.fenceCount).arg(_importPreview.pointCount).arg(_importPreview.totalPointCount) :
                                    qsTr("No new matching zones.")
        }

        QGCLabel {
            anchors.left:       parent.left
            anchors.right:      parent.right
            wrapMode:           Text.WordWrap
            color:              qgcPal.warningText
            text:               _importPreview.limitWarning
            visible:            text !== ""
        }

        QGCButton {
            anchors.left:       parent.left
            anchors.right:      parent.right
            text:               qsTr("Add Exclusion Fences")
            enabled:            importProhibitedCheck.checked || importReqAuthorisationCheck.checked || importConditionalCheck.checked
            onClicked:          control._importGeoZones()
        }
    }

    GeoZoneFenceImporter {
        id:                 importer
        geoFenceController: control.geoFenceController
    }

    // Previews are computed after changes settle since simplifying many zones takes a moment
    Timer {
        id:             importPreviewTimer
        interval:       300
        onTriggered:    control._updateImportPreview()
    }

    Connections {
        target:                 flightMap
        enabled:                importMapViewOnlyCheck.checked
        function onVisibleRegionChanged() { importPreviewTimer.restart() }
    }

    Connections {
        target:                 _geoZoneManager
        function onZonesChanged() { importPreviewTimer.restart() }
    }

    Connections {
        target:                 geoFenceController.polygons
        function onCountChanged() { importPreviewTimer.restart() }
    }

    Connections {
        target:                 geoFenceController.circles
        function onCountChanged() { importPreviewTimer.restart() }
    }

    function _horizontalDistanceText(meters) {
        var distance = QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnits(meters)
        return Math.round(distance) + " " + QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString
    }

    function _importRestrictions() {
        var restrictions = []
        if (importProhibitedCheck.checked) {
            restrictions.push(GeoZone.Prohibited)
        }
        if (importReqAuthorisationCheck.checked) {
            restrictions.push(GeoZone.ReqAuthorisation)
        }
        if (importConditionalCheck.checked) {
            restrictions.push(GeoZone.Conditional)
        }
        return restrictions
    }

    function _importArea() {
        return importMapViewOnlyCheck.checked ? flightMap.visibleRegion.boundingGeoRectangle() : QtPositioning.rectangle()
    }

    function _updateImportPreview() {
        if (!_importVisible) {
            return
        }
        _importPreview = importer.preview(_importRestrictions(), _importArea(),
                                          importActiveCombo.hours[importActiveCombo.currentIndex],
                                          importSimplifyCombo.tolerances[importSimplifyCombo.currentIndex])
    }

    function _importGeoZones() {
        var restrictions = _importRestrictions()
        var area = _importArea()
        var activeWithinHours = importActiveCombo.hours[importActiveCombo.currentIndex]
        var tolerance = importSimplifyCombo.tolerances[importSimplifyCombo.currentIndex]
        var doImport = function() {
            var fenceCount = importer.importZones(restrictions, area, activeWithinHours, tolerance)
            mainWindow.showMessageDialog(qsTr("Geozones"),
                                         fenceCount > 0 ?
                                             qsTr("%1 exclusion fences added.").arg(fenceCount) :
                                             qsTr("No new matching zones found."))
        }

        var preview = importer.preview(restrictions, area, activeWithinHours, tolerance)
        if (preview.limitWarning !== "") {
            mainWindow.showMessageDialog(qsTr("Geozones"), preview.limitWarning + "\n\n" + qsTr("Add the fences anyway?"),
                                         Dialog.Ok | Dialog.Cancel, doImport)
        } else {
            doImport()
        }
    }
}
