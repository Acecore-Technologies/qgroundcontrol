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
import QGroundControl.FlightMap
import QGroundControl.ScreenTools

/// Map visuals for the ED-269 / ED-318 geozones loaded by GeoZoneManager
Item {
    id: _root

    property var    map
    property real   labelMinZoomLevel:  11  ///< Zone labels are only shown from this zoom level up

    readonly property var   _geoZoneManager:    QGroundControl.geoZoneManager
    readonly property bool  _showZones:         _geoZoneManager.visible
    readonly property bool  _showLabels:        _showZones && map.zoomLevel >= labelMinZoomLevel
    readonly property real  _activeFillAlpha:   0.25
    readonly property real  _inactiveFillAlpha: 0.08

    readonly property color _prohibitedColor:       "red"
    readonly property color _reqAuthorisationColor: "orange"
    readonly property color _conditionalColor:      "yellow"
    readonly property color _noRestrictionColor:    "limegreen"
    readonly property color _uSpaceColor:           "deepskyblue"
    readonly property color _unknownColor:          "gray"

    function restrictionColor(restriction) {
        switch (restriction) {
        case GeoZone.Prohibited:        return _prohibitedColor
        case GeoZone.ReqAuthorisation:  return _reqAuthorisationColor
        case GeoZone.Conditional:       return _conditionalColor
        case GeoZone.NoRestriction:     return _noRestrictionColor
        case GeoZone.USpace:            return _uSpaceColor
        default:                        return _unknownColor
        }
    }

    function _fillColor(zone) {
        const color = restrictionColor(zone.restriction)
        return Qt.rgba(color.r, color.g, color.b, zone.active ? _activeFillAlpha : _inactiveFillAlpha)
    }

    function _showZoneInfo(zone) {
        geoZoneInfoDialogComponent.createObject(mainWindow, { geoZone: zone }).open()
    }

    Component {
        id: geoZoneInfoDialogComponent

        GeoZoneInfoDialog { }
    }

    // Map items must be added to the map itself, so Instantiator is used instead of MapItemView.
    // Visibility is controlled with active since Qt 6.8 ignores a model change to 0.
    Instantiator {
        model:  _geoZoneManager.polygonShapes
        active: _showZones

        delegate: MapPolygon {
            geoShape:       object.geoShape
            color:          _root._fillColor(object.zone)
            border.color:   _root.restrictionColor(object.zone.restriction)
            border.width:   object.zone.active ? 2 : 1
            z:              QGroundControl.zOrderMapItems - 2
        }

        onObjectAdded:      (index, object) => _root.map.addMapItem(object)
        onObjectRemoved:    (index, object) => _root.map.removeMapItem(object)
    }

    Instantiator {
        model:  _geoZoneManager.circleShapes
        active: _showZones

        delegate: MapCircle {
            center:         object.center
            radius:         object.radius
            color:          _root._fillColor(object.zone)
            border.color:   _root.restrictionColor(object.zone.restriction)
            border.width:   object.zone.active ? 2 : 1
            z:              QGroundControl.zOrderMapItems - 2
        }

        onObjectAdded:      (index, object) => _root.map.addMapItem(object)
        onObjectRemoved:    (index, object) => _root.map.removeMapItem(object)
    }

    // Labels are only created for zones in the visible map region, limited in number by the manager
    property var _labelZones: []

    function _updateLabelZones() {
        const zones = _showLabels ? _geoZoneManager.labelZones(map.visibleRegion.boundingGeoRectangle()) : []
        if (zones.length === _labelZones.length && zones.every((zone, index) => zone === _labelZones[index])) {
            return
        }
        _labelZones = zones
    }

    on_ShowLabelsChanged: labelZonesTimer.restart()

    Connections {
        target:                             map
        function onVisibleRegionChanged()   { labelZonesTimer.restart() }
    }

    Connections {
        target:                     _geoZoneManager
        function onZonesChanged()   { _root._labelZones = []; labelZonesTimer.restart() }
    }

    Timer {
        id:             labelZonesTimer
        interval:       250
        onTriggered:    _root._updateLabelZones()
    }

    Component.onCompleted: labelZonesTimer.restart()

    // Clickable zone labels which show the zone details
    Instantiator {
        model:  _root._labelZones
        active: _showLabels

        delegate: MapQuickItem {
            coordinate:     modelData.center
            anchorPoint.x:  sourceItem.width / 2
            anchorPoint.y:  sourceItem.height / 2
            z:              QGroundControl.zOrderMapItems - 1

            readonly property var _zone: modelData

            sourceItem: Rectangle {
                width:          zoneLabel.contentWidth + ScreenTools.defaultFontPixelWidth
                height:         zoneLabel.contentHeight + ScreenTools.defaultFontPixelHeight / 4
                radius:         height / 4
                color:          Qt.rgba(0, 0, 0, 0.6)
                border.color:   _root.restrictionColor(_zone.restriction)

                QGCLabel {
                    id:                 zoneLabel
                    anchors.centerIn:   parent
                    text:               _zone.name !== "" ? _zone.name : _zone.identifier
                    color:              "white"
                    font.pointSize:     ScreenTools.smallFontPointSize
                }

                QGCMouseArea {
                    anchors.fill:   parent
                    onClicked:      _root._showZoneInfo(_zone)
                }
            }
        }

        onObjectAdded:      (index, object) => _root.map.addMapItem(object)
        onObjectRemoved:    (index, object) => _root.map.removeMapItem(object)
    }
}
