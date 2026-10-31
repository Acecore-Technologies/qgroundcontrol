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
    property var    highlightedZones:   []  ///< Zones drawn with a white border

    readonly property int hiddenZoneCount: _mapModel.hiddenZoneCount   ///< Smallest zones in view left out to limit the number of map items

    readonly property var   _geoZoneManager:    QGroundControl.geoZoneManager
    readonly property bool  _showZones:         _geoZoneManager.visible && map.visible // Items are not kept for maps in hidden views
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

    // More restrictive zones are drawn on top
    function _restrictionZ(restriction) {
        switch (restriction) {
        case GeoZone.Prohibited:        return 0.5
        case GeoZone.ReqAuthorisation:  return 0.4
        case GeoZone.Conditional:       return 0.3
        case GeoZone.USpace:            return 0.2
        default:                        return 0.1
        }
    }

    function _showZoneInfo(zone) {
        geoZoneInfoDialogComponent.createObject(mainWindow, { geoZone: zone }).open()
    }

    Component {
        id: geoZoneInfoDialogComponent

        GeoZoneInfoDialog { }
    }

    // Only shapes in or near the view are created, see GeoZoneMapModel
    GeoZoneMapModel { id: _mapModel }

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
        target:                             map
        function onVisibleRegionChanged()   { _root._scheduleViewUpdate() }
    }

    Component.onCompleted: _scheduleViewUpdate()

    // Map items must be added to the map itself, so Instantiator is used instead of MapItemView
    Instantiator {
        model: _mapModel.polygons

        delegate: MapPolygon {
            geoShape:       object.geoShape
            color:          _root._fillColor(object.zone)
            border.color:   _highlighted ? "white" : _root.restrictionColor(object.zone.restriction)
            border.width:   _highlighted ? 3 : (object.zone.active ? 2 : 1)
            z:              QGroundControl.zOrderMapItems - 3 + _root._restrictionZ(object.zone.restriction)

            readonly property bool _highlighted: _root.highlightedZones.indexOf(object.zone) >= 0
        }

        onObjectAdded:      (index, object) => _root.map.addMapItem(object)
        onObjectRemoved:    (index, object) => _root.map.removeMapItem(object)
    }

    Instantiator {
        model: _mapModel.circles

        delegate: MapCircle {
            center:         object.center
            radius:         object.radius
            color:          _root._fillColor(object.zone)
            border.color:   _highlighted ? "white" : _root.restrictionColor(object.zone.restriction)
            border.width:   _highlighted ? 3 : (object.zone.active ? 2 : 1)
            z:              QGroundControl.zOrderMapItems - 3 + _root._restrictionZ(object.zone.restriction)

            readonly property bool _highlighted: _root.highlightedZones.indexOf(object.zone) >= 0
        }

        onObjectAdded:      (index, object) => _root.map.addMapItem(object)
        onObjectRemoved:    (index, object) => _root.map.removeMapItem(object)
    }

    // Clickable zone labels which show the zone details
    Instantiator {
        model: _mapModel.labelZones

        delegate: MapQuickItem {
            coordinate:     object.center
            anchorPoint.x:  sourceItem.width / 2
            anchorPoint.y:  sourceItem.height / 2
            z:              QGroundControl.zOrderMapItems - 1

            readonly property var _zone: object

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
