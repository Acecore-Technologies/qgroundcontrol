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
import QGroundControl.ScreenTools

/// The geozone items of the Plan view, so that PlanView.qml only has this overlay over its map, the Geozones action of
/// its tool strip (GeoZoneToolStripAction) and the calls for map clicks and uploads: the zones on the map, the mission
/// banner above the map and the Geozones panel of the tool strip.
Item {
    id: control
    z:  QGroundControl.zOrderWidgets

    required property var   map                     ///< Map of the Plan view
    required property var   planMasterController
    property real           leftLimit:  0           ///< Space of the banner, between the tool strip and the right panel
    property real           rightLimit: width
    property real           margin:     ScreenTools.defaultFontPixelWidth * 0.75    ///< Of the banner to the top

    readonly property Component dropPanelComponent: geoZoneDropPanel   ///< Geozones panel of the tool strip

    readonly property var _geoFenceController: planMasterController.geoFenceController

    /// Shows the details of the zones at the coordinate of a map click which doesn't add or select an item
    /// @return true if there are zones at the coordinate
    function showZonesAt(coordinate) {
        return mapVisuals.showZonesAt(coordinate)
    }

    /// Selects a fence at the coordinate for editing. Clicking again selects the next of overlapping fences.
    /// @return true if there is a fence at the coordinate
    function selectFenceAt(coordinate) {
        const polygons = _geoFenceController.polygons
        const circles = _geoFenceController.circles
        const fences = []
        let selected = -1
        for (let i = 0; i < polygons.count; i++) {
            const polygon = polygons.get(i)
            if (polygon.containsCoordinate(coordinate)) {
                if (polygon.interactive) {
                    selected = fences.length
                }
                fences.push(polygon)
            }
        }
        for (let j = 0; j < circles.count; j++) {
            const circle = circles.get(j)
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
        const next = fences[(selected + 1) % fences.length]
        if (!next.interactive) {
            _geoFenceController.clearAllInteractive()
            next.interactive = true
        }
        return true
    }

    /// Asks for confirmation before uploading a mission which enters prohibited zones, then calls uploadFunction
    function confirmUpload(uploadFunction) {
        banner.confirmUpload(uploadFunction)
    }

    GeoZoneMapVisuals {
        id:                 mapVisuals
        map:                control.map
        highlightedZones:   banner.crossedZones
        fitToAddedSources:  true
    }

    GeoZoneMissionBanner {
        id:                 banner
        anchors.top:        parent.top
        anchors.topMargin:  control.margin
        missionController:  control.planMasterController.missionController
        geoFenceController: control._geoFenceController
        map:                control.map
        leftLimit:          control.leftLimit
        rightLimit:         control.rightLimit
    }

    Component {
        id: geoZoneDropPanel

        // Loaded by the drop panel of the tool strip, which provides dropPanel
        GeoZoneMapPanel {
            map:                control.map
            hiddenZoneCount:    mapVisuals.hiddenZoneCount
            showCrossedZones:   banner.crossedZones.length > 0
            briefingEnabled:    true

            onManageRequested: {
                dropPanel.hide()
                mainWindow.showSettingsTool(qsTr("Geozones"))
            }
            onZoneSelected: dropPanel.hide()
            onExportBriefingRequested: {
                dropPanel.hide()
                banner.exportBriefing()
            }
        }
    }
}
