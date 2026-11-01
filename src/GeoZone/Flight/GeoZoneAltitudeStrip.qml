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

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Vertical strip of the geozones above and below the active vehicle, with their floors and ceilings and the vehicle's
/// altitude, so the pilot sees how far it is from entering or leaving them
Rectangle {
    id:         control
    width:      ScreenTools.defaultFontPixelWidth * 18
    radius:     ScreenTools.defaultFontPixelWidth / 2
    color:      Qt.rgba(qgcPal.window.r, qgcPal.window.g, qgcPal.window.b, 0.8)
    visible:    _monitor.monitoring && _bands.length > 0 && !isNaN(_altitude)

    readonly property var   _monitor:       GeoZoneMonitor
    readonly property var   _active:        GeoZoneMonitor.activeVehicle
    readonly property var   _bands:         _active.verticalBands
    readonly property real  _altitude:      _active.altitudeAMSL
    readonly property real  _terrain:       _active.terrainAMSL
    readonly property real  _margin:        ScreenTools.defaultFontPixelWidth / 2
    readonly property real  _barX:          _margin
    readonly property real  _barWidth:      ScreenTools.defaultFontPixelWidth * 2.5
    readonly property real  _plotTop:       titleLabel.y + titleLabel.height + _margin
    readonly property real  _plotHeight:    height - _plotTop - _margin

    // Shown range: the ground or below the vehicle up to the limits near it
    readonly property real _bottom: isNaN(_terrain) ? _altitude - 150 : Math.min(_terrain, _altitude - 30)
    readonly property real _top: {
        let top = Math.max(_altitude + 100, _bottom + 200)
        for (const band of _bands) {
            for (const limit of [ band.floor, band.ceiling ]) {
                if (!isNaN(limit) && limit > top && limit < _altitude + 1000) {
                    top = limit + 30
                }
            }
        }
        return top
    }

    function _y(altitude) {
        return _plotTop + (_top - altitude) / (_top - _bottom) * _plotHeight
    }

    function _heightText(altitude) {
        // Above the ground when it is known, as the zone limits mostly are
        const height = isNaN(_terrain) ? altitude : altitude - _terrain
        return Math.round(QGroundControl.unitsConversion.metersToAppSettingsVerticalDistanceUnits(height)) + " " +
                QGroundControl.unitsConversion.appSettingsVerticalDistanceUnitsString + (isNaN(_terrain) ? qsTr(" AMSL") : "")
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        id:                 titleLabel
        x:                  control._margin
        y:                  control._margin
        width:              control.width - (control._margin * 2)
        text:               isNaN(control._terrain) ? qsTr("Geozones here") : qsTr("Geozones here, above ground")
        font.pointSize:     ScreenTools.smallFontPointSize
        wrapMode:           Text.WordWrap
    }

    // Ground
    Rectangle {
        x:          control._barX
        y:          control._y(control._terrain)
        width:      control._barWidth
        height:     Math.max(0, control._plotTop + control._plotHeight - y)
        color:      Qt.rgba(0.45, 0.35, 0.2, 0.8)
        visible:    !isNaN(control._terrain)
    }

    Repeater {
        model: control._bands

        Item {
            id:         bandItem
            anchors.fill: parent

            required property var   modelData
            required property int   index

            readonly property real _floorY:     isNaN(modelData.floor) ? control._plotTop + control._plotHeight : Math.min(control._y(modelData.floor), control._plotTop + control._plotHeight)
            readonly property real _ceilingY:   isNaN(modelData.ceiling) ? control._plotTop : Math.max(control._y(modelData.ceiling), control._plotTop)
            // Overlapping bands are drawn side by side
            readonly property real _slotWidth:  control._barWidth / control._bands.length

            Rectangle {
                x:              control._barX + (bandItem.index * bandItem._slotWidth)
                y:              bandItem._ceilingY
                width:          bandItem._slotWidth
                height:         Math.max(1, bandItem._floorY - bandItem._ceilingY)
                color:          GeoZoneStyle.fillColor(bandItem.modelData.restriction, true)
                border.color:   GeoZoneStyle.restrictionColor(bandItem.modelData.restriction)
                border.width:   1
            }

            // Labels of bands sharing a floor are stacked above it
            QGCLabel {
                x:              control._barX + control._barWidth + control._margin
                y:              Math.min(Math.max(bandItem._floorY - (height * (bandItem.index + 1)), control._plotTop), control._plotTop + control._plotHeight - height)
                width:          control.width - x - control._margin
                text:           bandItem.modelData.name + "\n" + bandItem.modelData.limitsText
                wrapMode:       Text.NoWrap
                textFormat:     Text.PlainText
                elide:          Text.ElideRight
                font.pointSize: ScreenTools.smallFontPointSize
                color:          GeoZoneStyle.restrictionColor(bandItem.modelData.restriction)
                visible:        bandItem.index < 3
            }
        }
    }

    // Vehicle
    Rectangle {
        x:          control._barX - control._margin
        y:          control._y(control._altitude) - (height / 2)
        width:      control._barWidth + (control._margin * 2)
        height:     2
        color:      qgcPal.text
    }

    QGCLabel {
        x:              control._barX + control._barWidth + control._margin
        y:              Math.min(Math.max(control._y(control._altitude) - (height / 2), control._plotTop), control._plotTop + control._plotHeight - height)
        text:           "\u25C0 " + control._heightText(control._altitude)
        font.bold:      true
        font.pointSize: ScreenTools.smallFontPointSize
    }
}
