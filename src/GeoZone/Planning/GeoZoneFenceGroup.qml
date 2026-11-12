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
import QtQuick.Controls

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Geozone part of GeoFenceEditor.qml, beside its lists of polygon and circular fences: the button adding fences from
/// geozones, the fence points the vehicle stores, the zones the import left out and the fences added from geozones,
/// named after their zones (see GeoZoneFenceTags), which can be selected for editing and deleted together
Column {
    id:         control
    spacing:    control._margin

    required property var geoFenceController
    required property var flightMap

    readonly property var   _polygons:  control.geoFenceController.polygons
    readonly property var   _circles:   control.geoFenceController.circles
    readonly property real  _margin:    ScreenTools.defaultFontPixelWidth / 2
    // What a connected vehicle stores, see GeoZoneFenceImporter::capacityInfo: estimated unless the vehicle is known to have it
    readonly property var   _capacity:      staleImporter.capacity
    readonly property int   _pointLimit:    staleImporter.upload.canUpload ? (control._capacity.maxItems ?? 0) : 0
    readonly property bool  _limitEstimated: control._capacity.estimated ?? false

    // Polygon vertices, one point per circle and the breach return point, as vehicles store them
    readonly property int _pointCount: {
        let points = control._circles.count + (control.geoFenceController.breachReturnPoint.isValid ? 1 : 0)
        for (let i = 0; i < control._polygons.count; i++) {
            points += control._polygons.get(i).count
        }
        return points
    }

    readonly property int _geoZoneCount: {
        let count = 0
        for (const fences of [ control._polygons, control._circles ]) {
            for (let i = 0; i < fences.count; i++) {
                count += GeoZoneFenceTags.tag(fences.get(i)).geoZone ? 1 : 0
            }
        }
        return count
    }

    // The zones the vehicle's fence leaves out are shown unless the plan's note names the same ones
    readonly property bool _vehicleNoteShown: staleImporter.vehicleFenceNote !== "" && staleImporter.vehicleFenceNoteZones.join("\n") !== staleImporter.fenceNoteZones.join("\n")

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    /// Lists each of the zones in a dialog which scrolls
    function _showZones(title, intro, zones) {
        zoneListDialogComponent.createObject(mainWindow, { title: title, intro: intro, zones: zones }).open()
    }

    /// Selects the fence for editing, moving the map to it when it is out of view
    function _select(fence) {
        if (!fence) {
            return
        }
        // Selecting the selected fence again would recreate its editing handles
        if (!fence.interactive) {
            control.geoFenceController.clearAllInteractive()
            fence.interactive = true
        }
        if (!control.flightMap.visibleRegion.boundingGeoRectangle().contains(fence.center)) {
            control.flightMap.center = fence.center
        }
    }

    QGCButton {
        text:       qsTr("From Geozones...")
        visible:    GeoZoneManager.zoneCount > 0
        onClicked:  fenceImportDialogComponent.createObject(mainWindow, { geoFenceController: control.geoFenceController, flightMap: control.flightMap }).open()
    }

    QGCLabel {
        width:          parent.width
        wrapMode:       Text.WordWrap
        font.pointSize: ScreenTools.smallFontPointSize
        color:          (control._pointLimit > 0 && control._pointCount > control._pointLimit) ? qgcPal.warningText : qgcPal.text
        text:           control._pointLimit > 0 ? (control._limitEstimated ? qsTr("Fence points: %1 of about %2 the vehicle can store (estimated)").arg(control._pointCount).arg(control._pointLimit)
                                                           : qsTr("Fence points: %1 of %2 the vehicle can store").arg(control._pointCount).arg(control._pointLimit))
                                        : qsTr("Fence points: %1").arg(control._pointCount)
        visible:        control._pointCount > 0
    }

    // Fences from zones which changed, are no longer loaded or expired since they were added
    GeoZoneFenceImporter {
        id:                 staleImporter
        geoFenceController: control.geoFenceController
    }

    QGCLabel {
        width:      parent.width
        wrapMode:   Text.WordWrap
        color:      qgcPal.warningText
        text:       staleImporter.staleText
        textFormat: Text.PlainText
        visible:    text !== ""
    }

    QGCButton {
        text:       qsTr("Rebuild Geozone Fences")
        visible:    staleImporter.staleText !== ""
        onClicked:  staleImporter.rebuild()
    }

    // The zones the last import from geozones left out, until its fences change
    QGCLabel {
        width:      parent.width
        wrapMode:   Text.WordWrap
        color:      qgcPal.warningText
        text:       staleImporter.fenceNote
        textFormat: Text.PlainText
        visible:    text !== ""
    }

    QGCButton {
        text:       qsTr("Not Fenced...")
        visible:    staleImporter.fenceNoteCount > 0
        onClicked:  control._showZones(qsTr("Geozones Not Fenced"), qsTr("The vehicle doesn't stop at these geozones, QGroundControl only alerts about them:"),
                                       staleImporter.fenceNoteZones)
    }

    // Those of the fence the vehicle stores, as recorded when it was uploaded, when they aren't those of the plan: like
    // after a restart, with the fence loaded from the vehicle
    QGCLabel {
        width:      parent.width
        wrapMode:   Text.WordWrap
        color:      qgcPal.warningText
        text:       staleImporter.vehicleFenceNote
        textFormat: Text.PlainText
        visible:    control._vehicleNoteShown
    }

    QGCButton {
        text:       qsTr("Not Fenced on Vehicle...")
        visible:    control._vehicleNoteShown
        onClicked:  control._showZones(qsTr("Geozones Not Fenced on Vehicle"),
                                       qsTr("The fence on the vehicle was uploaded without these geozones. The vehicle doesn't stop at them, QGroundControl only alerts about them:"),
                                       staleImporter.vehicleFenceNoteZones)
    }

    SectionHeader {
        id:             geoZoneSection
        anchors.left:   parent.left
        anchors.right:  parent.right
        text:           qsTr("Geozone Fences (%1)").arg(control._geoZoneCount)
        visible:        control._geoZoneCount > 0
    }

    Repeater {
        model:      geoZoneSection.visible && geoZoneSection.checked ? control._polygons : 0
        delegate:   FenceName { width: control.width; isCircle: false; onSelected: fence => control._select(fence) }
    }

    Repeater {
        model:      geoZoneSection.visible && geoZoneSection.checked ? control._circles : 0
        delegate:   FenceName { width: control.width; isCircle: true; onSelected: fence => control._select(fence) }
    }

    QGCButton {
        text:       qsTr("Delete Geozone Fences")
        visible:    geoZoneSection.visible && geoZoneSection.checked
        onClicked:  mainWindow.showMessageDialog(text, qsTr("Delete the %n fence(s) added from geozones?", "", control._geoZoneCount), Dialog.Ok | Dialog.Cancel,
                                                 function() { GeoZoneFenceTags.deleteFences(control.geoFenceController, true) })
    }

    // Name of a fence added from a geozone with its place in the lists of polygon and circular fences. The fence is
    // gone before its name when the fences are deleted.
    component FenceName: QGCLabel {
        id:         nameLabel
        visible:    _tag?.geoZone ?? false
        elide:      Text.ElideRight
        textFormat: Text.PlainText
        color:      (object?.interactive ?? false) ? namePal.buttonHighlight : namePal.text
        text:       (isCircle ? qsTr("Circle %1: %2") : qsTr("Polygon %1: %2")).arg(index + 1).arg(_tag?.name ?? "")

        required property var   object
        required property int   index
        property bool           isCircle

        readonly property var _tag: object ? GeoZoneFenceTags.tag(object) : null

        signal selected(var fence)

        QGCPalette { id: namePal; colorGroupEnabled: true }

        QGCMouseArea {
            anchors.fill:   parent
            onClicked:      nameLabel.selected(nameLabel.object)
        }
    }

    Component {
        id: fenceImportDialogComponent

        GeoZoneFenceImportDialog { }
    }

    Component {
        id: zoneListDialogComponent

        GeoZoneFenceZoneListDialog { }
    }
}
