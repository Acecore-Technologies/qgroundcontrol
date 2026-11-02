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

/// Geozone part of GeoFenceEditor.qml, beside its lists of polygon and circular fences: the button adding fences from
/// geozones, the fence points the vehicle stores and the fences added from geozones, named after their zones (see
/// GeoZoneFenceTags), which can be selected for editing and deleted together
Column {
    id:         control
    spacing:    _margin

    required property var geoFenceController
    required property var flightMap

    readonly property var   _polygons:  geoFenceController.polygons
    readonly property var   _circles:   geoFenceController.circles
    readonly property var   _vehicle:   geoFenceController.masterController ? geoFenceController.masterController.managerVehicle : null
    readonly property real  _margin:    ScreenTools.defaultFontPixelWidth / 2
    readonly property int   _pointLimit: (_vehicle && _vehicle.px4Firmware) ? 64 : 0   // PX4 dataman fence storage (DM_KEY_FENCE_POINTS_MAX)

    // Polygon vertices, one point per circle and the breach return point, as vehicles store them
    readonly property int _pointCount: {
        let points = _circles.count + (geoFenceController.breachReturnPoint.isValid ? 1 : 0)
        for (let i = 0; i < _polygons.count; i++) {
            points += _polygons.get(i).count
        }
        return points
    }

    readonly property int _geoZoneCount: {
        let count = 0
        for (const fences of [ _polygons, _circles ]) {
            for (let i = 0; i < fences.count; i++) {
                count += GeoZoneFenceTags.tag(fences.get(i)).geoZone ? 1 : 0
            }
        }
        return count
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    /// Selects the fence for editing, moving the map to it when it is out of view
    function _select(fence) {
        // Selecting the selected fence again would recreate its editing handles
        if (!fence.interactive) {
            geoFenceController.clearAllInteractive()
            fence.interactive = true
        }
        if (!flightMap.visibleRegion.boundingGeoRectangle().contains(fence.center)) {
            flightMap.center = fence.center
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
        color:          (_pointLimit > 0 && _pointCount > _pointLimit) ? qgcPal.warningText : qgcPal.text
        text:           _pointLimit > 0 ? qsTr("Fence points: %1 of %2 the vehicle can store").arg(_pointCount).arg(_pointLimit) : qsTr("Fence points: %1").arg(_pointCount)
        visible:        _pointCount > 0
    }

    SectionHeader {
        id:             geoZoneSection
        anchors.left:   parent.left
        anchors.right:  parent.right
        text:           qsTr("Geozone Fences (%1)").arg(_geoZoneCount)
        visible:        _geoZoneCount > 0
    }

    Repeater {
        model:      geoZoneSection.visible && geoZoneSection.checked ? _polygons : 0
        delegate:   FenceName { isCircle: false }
    }

    Repeater {
        model:      geoZoneSection.visible && geoZoneSection.checked ? _circles : 0
        delegate:   FenceName { isCircle: true }
    }

    QGCButton {
        text:       qsTr("Delete Geozone Fences")
        visible:    geoZoneSection.visible && geoZoneSection.checked
        onClicked:  mainWindow.showMessageDialog(text, qsTr("Delete the %n fence(s) added from geozones?", "", _geoZoneCount), Dialog.Ok | Dialog.Cancel,
                                                 function() { GeoZoneFenceTags.deleteFences(control.geoFenceController, true) })
    }

    // Name of a fence added from a geozone with its place in the lists of polygon and circular fences
    component FenceName: QGCLabel {
        id:         nameLabel
        width:      control.width
        visible:    _tag.geoZone
        elide:      Text.ElideRight
        textFormat: Text.PlainText
        color:      object.interactive ? qgcPal.buttonHighlight : qgcPal.text
        text:       (isCircle ? qsTr("Circle %1: %2") : qsTr("Polygon %1: %2")).arg(index + 1).arg(_tag.name)

        required property var   object
        required property int   index
        property bool           isCircle

        readonly property var _tag: GeoZoneFenceTags.tag(object)

        QGCMouseArea {
            anchors.fill:   parent
            onClicked:      control._select(nameLabel.object)
        }
    }

    Component {
        id: fenceImportDialogComponent

        GeoZoneFenceImportDialog { }
    }
}
