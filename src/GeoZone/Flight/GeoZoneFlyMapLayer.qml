/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// The geozone items of the Fly view's map, so that FlyViewMap.qml only has this layer, the button of its map click
/// panel (GeoZoneMapClickButton) and the check of go to locations: the zones, left out of the small picture in picture
/// map, where the active vehicle enters a zone at its velocity, where it reaches its own fence (see GeoZoneFenceWarning)
/// and the zones at map clicks which open no guided action.
GeoZoneMapVisuals {
    id:                 control
    showZones:          map.pipState.state !== map.pipState.pipState
    showPrediction:     showZones
    vehicleAltitudes:   true
    // The map filters don't hide the zones the flight alerts report
    keptZones:          GeoZoneMonitor.activeVehicle?.keptZones ?? []

    readonly property var  _fenceWarning:       GeoZoneMonitor.activeVehicle?.fenceWarning ?? null
    readonly property bool _fenceWarningShown:  showPrediction && map.visible && !!_fenceWarning && _fenceWarning.active

    Component.onCompleted: {
        map.addMapItem(fencePathItem)
        map.addMapItem(fenceIntersectionItem)
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Path of the active vehicle to where it reaches its own fence, whose action the autopilot then takes
    MapPolyline {
        id:             fencePathItem
        path:           control._fenceWarningShown ? control._fenceWarning.path : []
        line.width:     ScreenTools.defaultFontPixelHeight / 5
        line.color:     qgcPal.colorRed
        visible:        control._fenceWarningShown
        z:              QGroundControl.zOrderMapItems + 1
    }

    // The estimated intersection with the fence, with the action and the seconds left
    MapQuickItem {
        id:             fenceIntersectionItem
        coordinate:     control._fenceWarningShown ? control._fenceWarning.intersection : QtPositioning.coordinate()
        visible:        control._fenceWarningShown
        anchorPoint.x:  fenceMark.width / 2
        anchorPoint.y:  fenceMark.height / 2
        z:              QGroundControl.zOrderMapItems + 2

        sourceItem: Item {
            width:  fenceMark.width + fenceLabelBox.width
            height: fenceMark.height

            Rectangle {
                id:             fenceMark
                width:          ScreenTools.defaultFontPixelHeight * 1.2
                height:         width
                radius:         width / 2
                color:          Qt.rgba(0, 0, 0, 0.6)
                border.color:   qgcPal.colorRed
                border.width:   ScreenTools.defaultFontPixelHeight / 5

                QGCLabel {
                    anchors.centerIn:   parent
                    text:               "\u2715"
                    color:              qgcPal.colorRed
                    font.bold:          true
                }
            }

            Rectangle {
                id:                     fenceLabelBox
                anchors.left:           fenceMark.right
                anchors.leftMargin:     ScreenTools.defaultFontPixelWidth / 2
                anchors.verticalCenter: fenceMark.verticalCenter
                width:                  fenceLabel.contentWidth + ScreenTools.defaultFontPixelWidth
                height:                 fenceLabel.contentHeight + ScreenTools.defaultFontPixelHeight / 4
                radius:                 height / 4
                color:                  Qt.rgba(0, 0, 0, 0.6)
                border.color:           qgcPal.colorRed

                QGCLabel {
                    id:                 fenceLabel
                    anchors.centerIn:   parent
                    color:              "white"
                    font.pointSize:     ScreenTools.smallFontPointSize
                    textFormat:         Text.PlainText
                    text:               control._fenceWarningShown ? qsTr("Geofence: %1 in %2 s").arg(control._fenceWarning.actionText).arg(control._fenceWarning.timeToFence) : ""
                }
            }
        }
    }

    /// @return true if a go to location would enter geozones, so it is confirmed even where QGroundControl wouldn't
    function gotoEntersZones(coordinate) {
        const vehicle = QGroundControl.multiVehicleManager.activeVehicle
        return !!vehicle && GeoZoneManager.zoneCount > 0 && GeoZoneMonitor.guidedCheck.gotoWarning(vehicle, coordinate) !== ""
    }

    /// @return Number of zones shown at the coordinate
    function zoneCountAt(coordinate) {
        return zonesAt(coordinate).length
    }

    // FlyViewMap.qml opens its panel of guided actions for the other clicks, see its onMapClicked
    Connections {
        target: control.map

        function onMapClicked(position) {
            const guided = globals.guidedControllerFlyView
            if (guided.guidedUIVisible || guided.showGotoLocation || guided.showOrbit || guided.showROI || guided.showSetHome || guided.showSetEstimatorOrigin) {
                return
            }
            control.showZonesAt(control.map.toCoordinate(Qt.point(position.x, position.y), false /* clipToViewPort */))
        }
    }
}
