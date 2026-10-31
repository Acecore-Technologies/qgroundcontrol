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

/// Draws the vertical extent of the geozones along the flight path behind the TerrainProfile of the Plan View.
/// Floors and ceilings beyond the profile's altitude range are drawn at its edges.
Canvas {
    id:     control
    z:      -1  // Behind the flight and terrain profiles drawn by the parent

    required property var terrainProfile    ///< TerrainProfile providing the scale
    required property var missionController

    readonly property var _bands: _profile.bands

    GeoZoneProfile {
        id:                 _profile
        missionController:  control.missionController
        active:             control.visible && QGroundControl.geoZoneManager.zones.count > 0
    }

    on_BandsChanged:    requestPaint()
    onWidthChanged:     requestPaint()
    onHeightChanged:    requestPaint()

    Connections {
        target:                             terrainProfile
        function onPixelsPerMeterChanged()  { control.requestPaint() }
        function onMinAMSLAltChanged()      { control.requestPaint() }
        function onMaxAMSLAltChanged()      { control.requestPaint() }
    }

    function _color(restriction) {
        switch (restriction) {
        case GeoZone.Prohibited:        return Qt.rgba(1, 0, 0, 0.3)
        case GeoZone.ReqAuthorisation:  return Qt.rgba(1, 0.65, 0, 0.3)
        case GeoZone.Conditional:       return Qt.rgba(1, 1, 0, 0.3)
        case GeoZone.USpace:            return Qt.rgba(0, 0.75, 1, 0.25)
        default:                        return Qt.rgba(0.5, 0.5, 0.5, 0.25)
        }
    }

    onPaint: {
        var ctx = getContext("2d")
        ctx.reset()
        var pixelsPerMeter = terrainProfile.pixelsPerMeter
        var minAlt = terrainProfile.minAMSLAlt
        var altRange = terrainProfile.maxAMSLAlt - minAlt
        if (!(pixelsPerMeter > 0) || !(altRange > 0) || !isFinite(pixelsPerMeter)) {
            return
        }
        // Unknown floors are drawn from the bottom and missing ceilings to the top
        var yOf = function(altitude, fallback) {
            return isNaN(altitude) ? fallback : Math.max(-1, Math.min(height + 1, height - ((altitude - minAlt) / altRange) * height))
        }
        ctx.font = ScreenTools.smallFontPointSize + "pt sans-serif"
        for (var i = 0; i < _bands.length; i++) {
            var band = _bands[i]
            var points = band.points
            var color = _color(band.restriction)
            ctx.fillStyle = color
            ctx.strokeStyle = Qt.rgba(color.r, color.g, color.b, 0.9)
            ctx.lineWidth = 1
            ctx.beginPath()
            for (var j = 0; j < points.length; j++) {
                var x = points[j].distance * pixelsPerMeter
                var y = yOf(points[j].ceiling, -1)
                if (j === 0) {
                    ctx.moveTo(x, y)
                } else {
                    ctx.lineTo(x, y)
                }
            }
            // A zone crossed at a single sample is drawn as a narrow band
            if (points.length === 1) {
                ctx.lineTo(points[0].distance * pixelsPerMeter + 2, yOf(points[0].ceiling, -1))
                ctx.lineTo(points[0].distance * pixelsPerMeter + 2, yOf(points[0].floor, height + 1))
            }
            for (var k = points.length - 1; k >= 0; k--) {
                ctx.lineTo(points[k].distance * pixelsPerMeter, yOf(points[k].floor, height + 1))
            }
            ctx.closePath()
            ctx.fill()
            ctx.stroke()

            // Labeled in the middle when the band is visible there
            var left = points[0].distance * pixelsPerMeter
            var right = points[points.length - 1].distance * pixelsPerMeter
            var middle = points[Math.floor(points.length / 2)]
            var top = Math.max(yOf(middle.ceiling, 0), 0)
            var bottom = Math.min(yOf(middle.floor, height), height)
            var textWidth = ctx.measureText(band.name).width
            var textHeight = ScreenTools.smallFontPointSize * 1.6
            if ((right - left > textWidth + 4) && (bottom - top > textHeight * 1.2)) {
                ctx.fillStyle = Qt.rgba(1, 1, 1, 0.85)
                ctx.fillText(band.name, (left + right - textWidth) / 2, top + textHeight)
            }
        }
    }
}
