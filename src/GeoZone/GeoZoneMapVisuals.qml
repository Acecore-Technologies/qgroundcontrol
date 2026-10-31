/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Map visuals for the ED-269 / ED-318 geozones loaded by GeoZoneManager
Item {
    id: _root

    property var    map
    property real   labelMinZoomLevel:  11  ///< Zone labels are only shown from this zoom level up
    property var    highlightedZones:   []  ///< Zones drawn with a white border
    property bool   fitToAddedSources:  false   ///< Moves the map to the zones of files and feeds the user adds when they are out of view or too small to see
    property point  controlPosition:    Qt.point(0, 0)  ///< Top left of the Geozones map button in map coordinates
    property bool   controlVisible:     true

    readonly property int   hiddenZoneCount:    _mapModel.hiddenZoneCount   ///< Smallest zones in view left out to limit the number of map items
    readonly property real  controlRight:       mapButton.visible ? mapButton.x + mapButton.width : 0

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

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    /// Legend entry showing how zones are drawn
    component Swatch: Rectangle {
        property color  restrictionColor
        property bool   active: true

        Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight
        Layout.preferredHeight: ScreenTools.defaultFontPixelHeight
        color:                  Qt.rgba(restrictionColor.r, restrictionColor.g, restrictionColor.b, active ? _root._activeFillAlpha : _root._inactiveFillAlpha)
        border.color:           restrictionColor
        border.width:           active ? 2 : 1
    }

    // Shows the legend and display options
    QGCButton {
        id:         mapButton
        parent:     _root.map
        x:          _root.controlPosition.x
        y:          _root.controlPosition.y
        z:          QGroundControl.zOrderWidgets
        text:       qsTr("Geozones")
        iconSource: "qrc:/qml/QGroundControl/GeoZone/GeoZone.svg"
        opacity:    0.85
        visible:    _root.controlVisible && !!_root.map && _root.map.visible && _geoZoneManager.zones.count > 0
        onClicked:  mapPanel.open()
    }

    DropPanel {
        id:                 mapPanel
        parent:             _root.map
        clickRect:          Qt.rect(mapButton.x, mapButton.y, mapButton.width, mapButton.height)
        sourceComponent:    mapPanelComponent
    }

    Component {
        id: mapPanelComponent

        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight / 2

            readonly property real _textWidth: ScreenTools.defaultFontPixelWidth * 32

            QGCCheckBoxSlider {
                Layout.fillWidth:   true
                text:               qsTr("Show geozones")
                checked:            _geoZoneManager.visible
                onClicked:          _geoZoneManager.visible = checked
            }

            GridLayout {
                columns:        2
                columnSpacing:  ScreenTools.defaultFontPixelWidth
                rowSpacing:     ScreenTools.defaultFontPixelHeight / 4

                Swatch { restrictionColor: _root._prohibitedColor }
                QGCLabel { text: qsTr("Prohibited") }

                Swatch { restrictionColor: _root._reqAuthorisationColor }
                QGCLabel { text: qsTr("Authorisation required") }

                Swatch { restrictionColor: _root._conditionalColor }
                QGCLabel { text: qsTr("Conditional") }

                Swatch { restrictionColor: _root._uSpaceColor }
                QGCLabel { text: qsTr("U-space") }

                Swatch { restrictionColor: _root._noRestrictionColor }
                QGCLabel { text: qsTr("No restriction") }

                Swatch {
                    restrictionColor:   _root._prohibitedColor
                    active:             false
                }
                QGCLabel { text: qsTr("Faint: not active now") }

                Swatch {
                    restrictionColor:   "white"
                    color:              "transparent"
                    border.width:       3
                    visible:            _root.highlightedZones.length > 0
                }
                QGCLabel {
                    text:       qsTr("White outline: crossed by the mission")
                    visible:    _root.highlightedZones.length > 0
                }
            }

            QGCLabel {
                text:           qsTr("Click a zone name on the map for details.")
                font.pointSize: ScreenTools.smallFontPointSize
            }

            QGCLabel {
                Layout.preferredWidth:  _textWidth
                wrapMode:               Text.WordWrap
                text:                   _geoZoneManager.busy ? _geoZoneManager.progressText :
                                            (_geoZoneManager.sources.count === 1 ? qsTr("%1 zones from 1 source.").arg(_geoZoneManager.zones.count) :
                                                                                  qsTr("%1 zones from %2 sources.").arg(_geoZoneManager.zones.count).arg(_geoZoneManager.sources.count))
            }

            QGCLabel {
                Layout.preferredWidth:  _textWidth
                wrapMode:               Text.WordWrap
                text:                   qsTr("%1 of the smallest zones in view are not shown. Zoom in to see them.").arg(_root.hiddenZoneCount)
                visible:                _geoZoneManager.visible && _root.hiddenZoneCount > 0
            }

            QGCLabel {
                Layout.preferredWidth:  _textWidth
                wrapMode:               Text.WordWrap
                color:                  qgcPal.warningText
                text:                   _geoZoneManager.validityWarning
                visible:                text !== ""
            }

            QGCButton {
                Layout.fillWidth:   true
                text:               qsTr("Manage Geozones...")
                onClicked: {
                    mapPanel.close()
                    mainWindow.showSettingsTool(qsTr("Geozones"))
                }
            }
        }
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
        target:     _geoZoneManager
        enabled:    _root.fitToAddedSources
        function onSourceAdded(source) {
            _root._boxToFit = source.boundingBox
            _root._fitAddedSource()
        }
    }

    // Sources can be added while the map's view is hidden
    Connections {
        target:     map
        enabled:    _root._boxToFit !== null
        function onVisibleChanged() { _root._fitAddedSource() }
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
