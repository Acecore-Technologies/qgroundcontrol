/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick

import QGroundControl
import QGroundControl.GeoZone

/// The geozone items of the Fly view's map, so that FlyViewMap.qml only has this layer, the button of its map click
/// panel (GeoZoneMapClickButton) and the check of go to locations: the zones, left out of the small picture in picture
/// map, where the active vehicle enters a zone at its velocity, and the zones at map clicks which open no guided action.
GeoZoneMapVisuals {
    id:             control
    showZones:      map.pipState.state !== map.pipState.pipState
    showPrediction: showZones

    /// @return true if a go to location would enter geozones, so it is confirmed even where QGroundControl wouldn't
    function gotoEntersZones(coordinate) {
        const vehicle = QGroundControl.multiVehicleManager.activeVehicle
        return !!vehicle && GeoZoneManager.zoneCount > 0 && GeoZoneMonitor.guidedCheck.gotoWarning(vehicle, coordinate) !== ""
    }

    /// @return Number of zones shown at the coordinate
    function zoneCountAt(coordinate) {
        return showZones && GeoZoneManager.display.visible ? GeoZoneManager.zonesAt(coordinate).length : 0
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
