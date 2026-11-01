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
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.ScreenTools

/// Map visuals for the ED-269 / ED-318 geozones loaded by GeoZoneManager
Item {
    id: _root

    property var    map
    property bool   showZones:          true    ///< False hides the zones on this map, like the small picture in picture map
    property real   labelMinZoomLevel:  11      ///< Zone labels are only shown from this zoom level up
    property var    highlightedZones:   []      ///< Zones drawn with a white border
    property bool   fitToAddedSources:  false   ///< Moves the map to the zones of files and feeds the user adds when they are out of view or too small to see
    property bool   showPrediction:     false   ///< Shows where the active vehicle enters a zone at its velocity, see GeoZoneMonitor

    readonly property int hiddenZoneCount: _mapModel.hiddenZoneCount   ///< Smallest zones in view left out to limit the number of map items

    readonly property var   _geoZoneManager:    GeoZoneManager
    readonly property bool  _showZones:         showZones && _geoZoneManager.display.visible && map.visible // Items are not kept for maps in hidden views
    readonly property bool  _showLabels:        _showZones && map.zoomLevel >= labelMinZoomLevel

    function _showZoneInfo(zone, zones) {
        geoZoneInfoDialogComponent.createObject(mainWindow, { geoZone: zone, zones: zones ? zones : [] }).open()
    }

    /// Shows the details of the shown zones at the coordinate, with a choice where several overlap
    /// @return true if there are zones at the coordinate
    function showZonesAt(coordinate) {
        if (!_showZones) {
            return false
        }
        const zones = _geoZoneManager.zonesAt(coordinate)
        if (zones.length === 0) {
            return false
        }
        _showZoneInfo(zones[0], zones)
        return true
    }

    Component {
        id: geoZoneInfoDialogComponent

        GeoZoneInfoDialog { }
    }

    property var _boxToFit: null

    function _fitAddedSource() {
        if (!_boxToFit || !map.visible) {
            return
        }
        const box = _boxToFit
        _boxToFit = null
        const view = map.visibleRegion.boundingGeoRectangle()
        // Also when the zones would only be a few pixels wide
        const tooSmall = (box.width * 20 < view.width) && (box.height * 20 < view.height)
        if (box.isValid && (!view.intersects(box) || tooSmall)) {
            map.fitViewportToGeoShape(box, ScreenTools.defaultFontPixelHeight)
        }
    }

    Connections {
        target:     _root._geoZoneManager
        enabled:    _root.fitToAddedSources
        function onSourceAdded(source) {
            _root._boxToFit = source.boundingBox
            _root._fitAddedSource()
        }
    }

    // Zones picked in a search are shown on the visible map
    Connections {
        target:     _root._geoZoneManager
        enabled:    _root.map.visible && _root.showZones
        function onShowZoneRequested(zone) {
            _root._geoZoneManager.display.visible = true
            const box = zone.boundingBox
            if (box.isValid) {
                // At least about a kilometer across, so small zones are shown with their surroundings
                const minimumSize = 0.01
                const fitBox = QtPositioning.rectangle(box.center, Math.max(box.width, minimumSize), Math.max(box.height, minimumSize))
                _root.map.fitViewportToGeoShape(fitBox, ScreenTools.defaultFontPixelHeight * 4)
            }
            _root._showZoneInfo(zone)
        }
    }

    // Sources can be added while the map's view is hidden
    Connections {
        target:     _root.map
        enabled:    _root._boxToFit !== null
        function onVisibleChanged() { _root._fitAddedSource() }
    }

    // Only shapes in or near the view are created, see GeoZoneMapModel
    GeoZoneMapModel {
        id:                 _mapModel
        highlightedZones:   _root.highlightedZones
    }

    function _updateView() {
        _mapModel.setView(_showZones ? map.visibleRegion.boundingGeoRectangle() : QtPositioning.rectangle(), map.zoomLevel, _showLabels)
    }

    // Throttled so the zones follow a pan or zoom without updating on every frame
    Timer {
        id:             viewUpdateTimer
        interval:       250
        onTriggered:    _root._updateView()
    }

    function _scheduleViewUpdate() {
        if (!viewUpdateTimer.running) {
            viewUpdateTimer.start()
        }
    }

    on_ShowZonesChanged:    _updateView()
    on_ShowLabelsChanged:   _scheduleViewUpdate()

    Connections {
        target:                             _root.map
        function onVisibleRegionChanged()   { _root._scheduleViewUpdate() }
    }

    readonly property bool _predictionShown: showPrediction && map.visible && GeoZoneMonitor.predictedEntryTime >= 0
    readonly property color _predictionColor: _predictionShown ? GeoZoneStyle.restrictionColor(GeoZoneMonitor.predictedEntryZone.restriction) : "transparent"

    Component.onCompleted: {
        _scheduleViewUpdate()
        map.addMapItem(_alertPositionItem)
        map.addMapItem(_predictedPathItem)
        map.addMapItem(_predictedEntryItem)
    }

    // Path of the active vehicle to the zone it enters at its velocity
    MapPolyline {
        id:             _predictedPathItem
        path:           _root._predictionShown ? GeoZoneMonitor.predictedPath : []
        line.width:     ScreenTools.defaultFontPixelHeight / 5
        line.color:     _root._predictionColor
        visible:        _root._predictionShown
        z:              QGroundControl.zOrderMapItems
    }

    // Entry point with the seconds left
    MapQuickItem {
        id:             _predictedEntryItem
        coordinate:     GeoZoneMonitor.predictedEntry
        visible:        _root._predictionShown
        anchorPoint.x:  entryDot.width / 2
        anchorPoint.y:  entryDot.height / 2
        z:              QGroundControl.zOrderMapItems + 1

        sourceItem: Item {
            width:  entryDot.width + entryLabel.width
            height: entryDot.height

            Rectangle {
                id:             entryDot
                width:          ScreenTools.defaultFontPixelHeight
                height:         width
                radius:         width / 2
                color:          Qt.rgba(0, 0, 0, 0.5)
                border.color:   _root._predictionColor
                border.width:   ScreenTools.defaultFontPixelHeight / 5
            }

            Rectangle {
                anchors.left:           entryDot.right
                anchors.leftMargin:     ScreenTools.defaultFontPixelWidth / 2
                anchors.verticalCenter: entryDot.verticalCenter
                width:                  entryLabel.contentWidth + ScreenTools.defaultFontPixelWidth
                height:                 entryLabel.contentHeight + ScreenTools.defaultFontPixelHeight / 4
                radius:                 height / 4
                color:                  Qt.rgba(0, 0, 0, 0.6)
                border.color:           _root._predictionColor

                QGCLabel {
                    id:                 entryLabel
                    anchors.centerIn:   parent
                    color:              "white"
                    font.pointSize:     ScreenTools.smallFontPointSize
                    textFormat:         Text.PlainText
                    text:               _root._predictionShown ?
                                            qsTr("%1 in %2 s").arg(GeoZoneMonitor.predictedEntryZone.displayName).arg(GeoZoneMonitor.predictedEntryTime) : ""
                }
            }
        }
    }

    // Vehicle position of the alert shown from the alert history, click to hide it
    MapQuickItem {
        id:             _alertPositionItem
        coordinate:     GeoZoneMonitor.alertHistory.shownPosition
        visible:        _root.showZones && coordinate.isValid
        anchorPoint.x:  sourceItem.width / 2
        anchorPoint.y:  sourceItem.height / 2
        z:              QGroundControl.zOrderMapItems + 1

        sourceItem: Rectangle {
            width:          ScreenTools.defaultFontPixelHeight * 1.6
            height:         width
            radius:         width / 2
            color:          Qt.rgba(0, 0, 0, 0.4)
            border.color:   GeoZoneStyle.highlightColor
            border.width:   ScreenTools.defaultFontPixelHeight / 6

            Rectangle {
                anchors.centerIn:   parent
                width:              parent.width / 3
                height:             width
                radius:             width / 2
                color:              GeoZoneStyle.highlightColor
            }

            QGCMouseArea {
                anchors.fill:   parent
                onClicked:      GeoZoneMonitor.alertHistory.clearShownPosition()
            }
        }
    }

    // All shapes are drawn by one item over the map, below the other map items
    GeoZoneMapLayer {
        parent:             _root.map
        anchors.fill:       parent
        map:                _root.map
        model:              _mapModel
        visible:            _root._showZones
        z:                  QGroundControl.zOrderMapItems - 3
        restrictionColors:  [ GeoZone.Prohibited, GeoZone.ReqAuthorisation, GeoZone.Conditional, GeoZone.NoRestriction, GeoZone.USpace, GeoZone.Unknown ]
                                .map(restriction => GeoZoneStyle.restrictionColor(restriction))
        highlightColor:     GeoZoneStyle.highlightColor
        activeFillAlpha:    GeoZoneStyle.activeFillAlpha
        inactiveFillAlpha:  GeoZoneStyle.inactiveFillAlpha
    }

    // Clickable zone labels which show the zone details
    Instantiator {
        model: _mapModel.labelZones

        delegate: MapQuickItem {
            id:             labelItem
            coordinate:     modelData.center
            anchorPoint.x:  sourceItem.width / 2
            anchorPoint.y:  sourceItem.height / 2
            z:              QGroundControl.zOrderMapItems - 1

            required property var modelData

            sourceItem: Rectangle {
                width:          zoneLabel.contentWidth + ScreenTools.defaultFontPixelWidth
                height:         zoneLabel.contentHeight + ScreenTools.defaultFontPixelHeight / 4
                radius:         height / 4
                color:          Qt.rgba(0, 0, 0, 0.6)
                border.color:   GeoZoneStyle.restrictionColor(labelItem.modelData.restriction)

                QGCLabel {
                    id:                 zoneLabel
                    anchors.centerIn:   parent
                    text:               labelItem.modelData.displayName
                    textFormat:         Text.PlainText
                    color:              "white"
                    font.pointSize:     ScreenTools.smallFontPointSize
                }

                QGCMouseArea {
                    anchors.fill:   parent
                    onClicked:      _root._showZoneInfo(labelItem.modelData)
                }
            }
        }

        onObjectAdded:      (index, object) => _root.map.addMapItem(object)
        onObjectRemoved:    (index, object) => _root.map.removeMapItem(object)
    }
}
