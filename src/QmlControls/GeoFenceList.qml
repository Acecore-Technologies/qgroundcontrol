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
import QGroundControl.FactControls
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Polygon and circular fences of a GeoFence in one list. Fences added from geozones are grouped. Clicking a fence
/// selects it for editing and shows it on the map, and the fence points are compared with the vehicle's storage.
Column {
    id:         control
    spacing:    _margin

    required property var   geoFenceController
    required property var   flightMap
    property real           maxListHeight: ScreenTools.defaultFontPixelHeight * 16

    readonly property var   _polygons:  geoFenceController.polygons
    readonly property var   _circles:   geoFenceController.circles
    readonly property var   _vehicle:   geoFenceController.masterController ? geoFenceController.masterController.managerVehicle : null
    readonly property real  _margin:    ScreenTools.defaultFontPixelWidth / 2

    readonly property int   px4MaxFencePoints:              64      ///< PX4 dataman fence storage (DM_KEY_FENCE_POINTS_MAX)
    readonly property int   arduPilotMaxPolygonVertices:    255     ///< ArduPilot stores the polygon vertex count in 8 bits

    readonly property int   fenceCount:     _polygons.count + _circles.count
    readonly property int   importedCount:  _countImported()
    readonly property int   drawnCount:     fenceCount - importedCount

    // Polygon vertices, one point per circle and the breach return point, as vehicles store them
    readonly property int   pointCount: {
        var points = _circles.count + (geoFenceController.breachReturnPoint.isValid ? 1 : 0)
        for (var i = 0; i < _polygons.count; i++) {
            points += _polygons.get(i).count
        }
        return points
    }
    readonly property int   pointLimit:     (_vehicle && _vehicle.px4Firmware) ? px4MaxFencePoints : 0
    readonly property int   largestPolygon: {
        var largest = 0
        for (var i = 0; i < _polygons.count; i++) {
            largest = Math.max(largest, _polygons.get(i).count)
        }
        return largest
    }
    readonly property bool  _polygonTooLarge: _vehicle && _vehicle.apmFirmware && largestPolygon > arduPilotMaxPolygonVertices

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    function _countImported() {
        var imported = 0
        for (var i = 0; i < _polygons.count; i++) {
            imported += _polygons.get(i).geoZone ? 1 : 0
        }
        for (var j = 0; j < _circles.count; j++) {
            imported += _circles.get(j).geoZone ? 1 : 0
        }
        return imported
    }

    /// Selects the fence for editing and shows it on the map when it is out of view or too small to see
    function selectFence(fence) {
        // Selecting the selected fence again would recreate its editing handles
        if (!fence.interactive) {
            geoFenceController.clearAllInteractive()
            fence.interactive = true
        }

        var box
        if (fence.path !== undefined) {
            var path = fence.path
            if (path.length === 0) {
                return
            }
            var north = -90, south = 90, east = -180, west = 180
            for (var i = 0; i < path.length; i++) {
                north = Math.max(north, path[i].latitude)
                south = Math.min(south, path[i].latitude)
                east = Math.max(east, path[i].longitude)
                west = Math.min(west, path[i].longitude)
            }
            box = QtPositioning.rectangle(QtPositioning.coordinate(north, west), QtPositioning.coordinate(south, east))
        } else {
            box = QtPositioning.circle(fence.center, fence.radius.rawValue).boundingGeoRectangle()
        }
        if (!box.isValid) {
            return
        }
        var view = flightMap.visibleRegion.boundingGeoRectangle()
        var tooSmall = (box.width * 20 < view.width) && (box.height * 20 < view.height)
        if (!view.contains(box.center) || tooSmall) {
            flightMap.fitViewportToGeoShape(box, ScreenTools.defaultFontPixelHeight * 2)
        }
    }

    /// Selects a fence at the coordinate. Clicking again selects the next of overlapping fences.
    function selectFenceAt(coordinate) {
        var fences = []
        var selected = -1
        for (var i = 0; i < _polygons.count; i++) {
            var polygon = _polygons.get(i)
            if (polygon.containsCoordinate(coordinate)) {
                if (polygon.interactive) {
                    selected = fences.length
                }
                fences.push(polygon)
            }
        }
        for (var j = 0; j < _circles.count; j++) {
            var circle = _circles.get(j)
            if (circle.center.distanceTo(coordinate) <= circle.radius.rawValue) {
                if (circle.interactive) {
                    selected = fences.length
                }
                fences.push(circle)
            }
        }
        if (fences.length === 0) {
            return false
        }
        var next = fences[(selected + 1) % fences.length]
        if (!next.interactive) {
            geoFenceController.clearAllInteractive()
            next.interactive = true
        }
        return true
    }

    function _deleteFences(imported) {
        for (var i = _polygons.count - 1; i >= 0; i--) {
            if (imported === undefined || _polygons.get(i).geoZone === imported) {
                geoFenceController.deletePolygon(i)
            }
        }
        for (var j = _circles.count - 1; j >= 0; j--) {
            if (imported === undefined || _circles.get(j).geoZone === imported) {
                geoFenceController.deleteCircle(j)
            }
        }
    }

    function _confirmDelete(title, text, imported) {
        mainWindow.showMessageDialog(title, text, Dialog.Ok | Dialog.Cancel, function() { control._deleteFences(imported) })
    }

    function _distanceText(meters) {
        return Math.round(QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnits(meters)) + " " +
                QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString
    }

    function _areaText(squareMeters) {
        var area = QGroundControl.unitsConversion.squareMetersToAppSettingsAreaUnits(squareMeters)
        return (area >= 100 ? Math.round(area) : area.toPrecision(2)) + " " + QGroundControl.unitsConversion.appSettingsAreaUnitsString
    }

    QGCLabel {
        text:       qsTr("None")
        visible:    fenceCount === 0
    }

    // Fence storage of the vehicle
    Column {
        anchors.left:   parent.left
        anchors.right:  parent.right
        spacing:        _margin / 2
        visible:        fenceCount > 0

        QGCLabel {
            anchors.left:   parent.left
            anchors.right:  parent.right
            wrapMode:       Text.WordWrap
            font.pointSize: ScreenTools.smallFontPointSize
            color:          (pointLimit > 0 && pointCount > pointLimit) ? qgcPal.warningText : qgcPal.text
            text:           pointLimit > 0 ?
                                qsTr("Fence points: %1 of %2 the vehicle can store").arg(pointCount).arg(pointLimit) :
                                qsTr("Fence points: %1").arg(pointCount)
        }

        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            height:         ScreenTools.defaultFontPixelHeight / 3
            radius:         height / 2
            color:          qgcPal.button
            visible:        pointLimit > 0

            Rectangle {
                width:  parent.width * Math.min(1, pointCount / pointLimit)
                height: parent.height
                radius: parent.radius
                color:  pointCount > pointLimit ? qgcPal.colorRed : (pointCount > pointLimit * 0.8 ? qgcPal.colorOrange : qgcPal.colorGreen)
            }
        }

        QGCLabel {
            anchors.left:   parent.left
            anchors.right:  parent.right
            wrapMode:       Text.WordWrap
            font.pointSize: ScreenTools.smallFontPointSize
            color:          qgcPal.warningText
            text:           qsTr("A polygon has %1 vertices. ArduPilot supports at most %2 per polygon.").arg(largestPolygon).arg(arduPilotMaxPolygonVertices)
            visible:        _polygonTooLarge
        }
    }

    // Long lists scroll on their own so the sections below stay within reach
    QGCFlickable {
        anchors.left:   parent.left
        anchors.right:  parent.right
        height:         Math.min(listColumn.implicitHeight, maxListHeight)
        contentWidth:   width
        contentHeight:  listColumn.implicitHeight
        visible:        fenceCount > 0

        Column {
            id:         listColumn
            width:      parent.width
            spacing:    _margin / 2

            QGCLabel {
                text:           qsTr("Drawn (%1)").arg(drawnCount)
                font.bold:      true
                visible:        importedCount > 0 && drawnCount > 0
            }

            // Each model is listed in both groups, the rows of the other group are inactive loaders
            Repeater {
                model: _polygons
                delegate: Loader {
                    width:              listColumn.width
                    active:             !object.geoZone
                    visible:            active
                    sourceComponent:    FenceRow { list: control; fence: object; fenceIndex: index; isCircle: false }
                }
            }

            Repeater {
                model: _circles
                delegate: Loader {
                    width:              listColumn.width
                    active:             !object.geoZone
                    visible:            active
                    sourceComponent:    FenceRow { list: control; fence: object; fenceIndex: index; isCircle: true }
                }
            }

            SectionHeader {
                id:             importedSection
                anchors.left:   parent.left
                anchors.right:  parent.right
                text:           qsTr("From geozones (%1)").arg(importedCount)
                showSpacer:     drawnCount > 0
                visible:        importedCount > 0
            }

            Repeater {
                model: _polygons
                delegate: Loader {
                    width:              listColumn.width
                    active:             object.geoZone && importedSection.checked
                    visible:            active
                    sourceComponent:    FenceRow { list: control; fence: object; fenceIndex: index; isCircle: false }
                }
            }

            Repeater {
                model: _circles
                delegate: Loader {
                    width:              listColumn.width
                    active:             object.geoZone && importedSection.checked
                    visible:            active
                    sourceComponent:    FenceRow { list: control; fence: object; fenceIndex: index; isCircle: true }
                }
            }
        }
    }

    QGCButton {
        anchors.left:   parent.left
        anchors.right:  parent.right
        text:           qsTr("Delete Geozone Fences")
        visible:        importedCount > 0 && drawnCount > 0
        onClicked:      control._confirmDelete(text, qsTr("Delete the %1 fences added from geozones?").arg(importedCount), true)
    }

    QGCButton {
        anchors.left:   parent.left
        anchors.right:  parent.right
        text:           qsTr("Delete All Fences")
        visible:        fenceCount > 1
        onClicked:      control._confirmDelete(text, qsTr("Delete all %1 fences?").arg(fenceCount), undefined)
    }

    component FenceRow: Rectangle {
        id:             row
        width:          parent ? parent.width : 0
        height:         visible ? rowColumn.implicitHeight + _rowMargin : 0
        radius:         ScreenTools.defaultFontPixelWidth / 2
        color:          fence.interactive ? rowPalette.buttonHighlight : "transparent"

        // Not required properties, which would hide the model roles of the Repeater
        property var    list
        property var    fence
        property int    fenceIndex
        property bool   isCircle

        readonly property real _rowMargin: ScreenTools.defaultFontPixelWidth / 2

        QGCPalette { id: rowPalette; colorGroupEnabled: true }

        readonly property color _textColor:     fence.interactive ? rowPalette.buttonHighlightText : rowPalette.text
        readonly property string _numberText:   isCircle ? qsTr("Circle %1").arg(fenceIndex + 1) : qsTr("Polygon %1").arg(fenceIndex + 1)
        readonly property string _sizeText:     isCircle ?
                                                    qsTr("radius %1").arg(list._distanceText(fence.radius.rawValue)) :
                                                    qsTr("%1 points, %2").arg(fence.count).arg(list._areaText(fence.area))

        QGCMouseArea {
            anchors.fill:   parent
            onClicked:      row.list.selectFence(row.fence)
        }

        ColumnLayout {
            id:                 rowColumn
            anchors.left:       parent.left
            anchors.right:      parent.right
            anchors.margins:    row._rowMargin
            anchors.verticalCenter: parent.verticalCenter
            spacing:            row._rowMargin / 2

            RowLayout {
                Layout.fillWidth:   true
                spacing:            row._rowMargin

                // Type of fence
                Canvas {
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight
                    Layout.preferredHeight: ScreenTools.defaultFontPixelHeight
                    Layout.alignment:       Qt.AlignVCenter

                    property color strokeColor: row._textColor
                    property bool  filled:      !row.fence.inclusion

                    onStrokeColorChanged:   requestPaint()
                    onFilledChanged:        requestPaint()

                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        var size = Math.min(width, height)
                        var center = size / 2
                        var r = size * 0.4
                        ctx.beginPath()
                        if (row.isCircle) {
                            ctx.arc(center, center, r, 0, 2 * Math.PI)
                        } else {
                            for (var i = 0; i < 5; i++) {
                                var angle = -Math.PI / 2 + i * 2 * Math.PI / 5
                                var x = center + r * Math.cos(angle)
                                var y = center + r * Math.sin(angle)
                                if (i === 0) {
                                    ctx.moveTo(x, y)
                                } else {
                                    ctx.lineTo(x, y)
                                }
                            }
                            ctx.closePath()
                        }
                        ctx.lineWidth = 1.5
                        ctx.strokeStyle = strokeColor
                        ctx.stroke()
                        // Exclusion fences are filled like on the map
                        if (filled) {
                            ctx.fillStyle = Qt.rgba(1, 0.65, 0, 0.6)
                            ctx.fill()
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth:   true
                    spacing:            0

                    QGCLabel {
                        Layout.fillWidth:   true
                        elide:              Text.ElideRight
                        color:              row._textColor
                        text:               row.fence.name !== "" ? row.fence.name : row._numberText
                        textFormat:         Text.PlainText
                    }

                    QGCLabel {
                        Layout.fillWidth:   true
                        elide:              Text.ElideRight
                        font.pointSize:     ScreenTools.smallFontPointSize
                        color:              row._textColor
                        text:               row.fence.name !== "" ? qsTr("%1, %2").arg(row._numberText).arg(row._sizeText) : row._sizeText
                        textFormat:         Text.PlainText
                    }
                }

                QGCColoredImage {
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight
                    Layout.preferredHeight: ScreenTools.defaultFontPixelHeight
                    source:                 "/res/TrashDelete.svg"
                    color:                  row._textColor
                    fillMode:               Image.PreserveAspectFit

                    QGCMouseArea {
                        anchors.fill:   parent
                        onClicked: {
                            if (row.isCircle) {
                                row.list.geoFenceController.deleteCircle(row.fenceIndex)
                            } else {
                                row.list.geoFenceController.deletePolygon(row.fenceIndex)
                            }
                        }
                    }
                }
            }

            // Settings of the selected fence
            Loader {
                Layout.fillWidth:   true
                active:             row.fence.interactive
                visible:            active

                sourceComponent: ColumnLayout {
                    spacing: row._rowMargin / 2

                    QGCCheckBox {
                        text:           qsTr("Inclusion fence")
                        textColor:      row._textColor
                        checked:        row.fence.inclusion
                        onClicked:      row.fence.inclusion = checked
                    }

                    Loader {
                        Layout.fillWidth:   true
                        active:             row.isCircle
                        visible:            active

                        sourceComponent: RowLayout {
                            QGCLabel {
                                text:   qsTr("Radius")
                                color:  row._textColor
                            }

                            FactTextField {
                                Layout.fillWidth:   true
                                fact:               row.fence.radius
                            }
                        }
                    }
                }
            }
        }
    }
}
