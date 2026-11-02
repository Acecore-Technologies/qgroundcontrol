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
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Draws the vertical extent of the geozones along the flight path behind the TerrainProfile of the Plan View, in the
/// colors of the maps. Floors and ceilings beyond the profile's altitude range are drawn at its edges. Only the zones the
/// path enters are labeled, once where their volumes stack, the most restrictive first, stacked where they would overlap
/// and left out where they don't fit.
Canvas {
    id:     control
    z:      -1  // Behind the flight and terrain profiles drawn by the parent

    required property var terrainProfile    ///< TerrainProfile providing the scale
    required property var missionController

    readonly property var _bands: _profile.bands

    GeoZoneProfile {
        id:                 _profile
        missionController:  control.missionController
        active:             control.visible && GeoZoneManager.zoneCount > 0
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Changes only when the scale does, the profile signals each value as it updates
    readonly property string _scaleKey: [ terrainProfile.pixelsPerMeter, terrainProfile.minAMSLAlt, terrainProfile.maxAMSLAlt ].join("/")

    on_BandsChanged:    requestPaint()
    on_ScaleKeyChanged: requestPaint()
    onWidthChanged:     requestPaint()
    onHeightChanged:    requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const pixelsPerMeter = terrainProfile.pixelsPerMeter
        const minAlt = terrainProfile.minAMSLAlt
        const altRange = terrainProfile.maxAMSLAlt - minAlt
        if (!(pixelsPerMeter > 0) || !(altRange > 0) || !isFinite(pixelsPerMeter)) {
            return
        }
        // Unknown floors are drawn from the bottom and missing ceilings to the top
        const yOf = (altitude, fallback) => isNaN(altitude) ? fallback : Math.max(-1, Math.min(height + 1, height - ((altitude - minAlt) / altRange) * height))
        for (const band of _bands) {
            const points = band.points
            const color = GeoZoneStyle.restrictionColor(band.restriction)
            ctx.fillStyle = GeoZoneStyle.fillColor(band.restriction, true)
            ctx.strokeStyle = Qt.rgba(color.r, color.g, color.b, 0.9)
            ctx.lineWidth = 1
            ctx.beginPath()
            for (let j = 0; j < points.length; j++) {
                const x = points[j].distance * pixelsPerMeter
                const y = yOf(points[j].ceiling, -1)
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
            for (let k = points.length - 1; k >= 0; k--) {
                ctx.lineTo(points[k].distance * pixelsPerMeter, yOf(points[k].floor, height + 1))
            }
            ctx.closePath()
            ctx.fill()
            ctx.stroke()
        }

        // Labels above all bands, the most restrictive (drawn last) placed first, in the first free row of the band
        ctx.font = ScreenTools.smallFontPointSize + "pt sans-serif"
        ctx.fillStyle = qgcPal.text
        const textHeight = ScreenTools.smallFontPointSize * 1.6
        const placed = []
        const overlaps = (a, b) => a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom
        for (let i = _bands.length - 1; i >= 0; i--) {
            const band = _bands[i]
            if (!band.entered) {
                continue
            }
            const points = band.points
            const left = Math.max(points[0].distance * pixelsPerMeter, 0)
            const right = Math.min(points[points.length - 1].distance * pixelsPerMeter, width)
            const middle = points[Math.floor(points.length / 2)]
            const top = Math.max(yOf(middle.ceiling, 0), 0)
            const bottom = Math.min(yOf(middle.floor, height), height)
            const textWidth = ctx.measureText(band.name).width
            if (right - left < textWidth + 4) {
                continue
            }
            const x = (left + right - textWidth) / 2
            // Volumes of a zone stacked at the same place are labeled once
            if (placed.some(other => other.name === band.name && other.left < right && left < other.right)) {
                continue
            }
            for (let y = top; y + textHeight <= bottom; y += textHeight) {
                const rect = { name: band.name, left: x - 2, right: x + textWidth + 2, top: y, bottom: y + textHeight }
                if (!placed.some(other => overlaps(rect, other))) {
                    placed.push(rect)
                    ctx.fillText(band.name, x, y + textHeight * 0.8)
                    break
                }
            }
        }
    }
}
