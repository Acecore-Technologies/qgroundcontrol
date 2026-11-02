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

import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Vertical strip of the geozones above and below the active vehicle, with their floors and ceilings and the vehicle's
/// altitude, so the pilot sees how far it is from entering or leaving them. Bands with the same restriction and limits,
/// like the same zone from two sources, are drawn as one, with their labels listed beside the bar without overlapping.
/// While no limit is within reach of the vehicle the strip collapses to a chip, which expands it when clicked. Badges
/// mark the strip stale while the vehicle's position is lost, and its height above ground as estimated from terrain
/// data when the vehicle sends none.
Item {
    id:         control
    width:      ScreenTools.defaultFontPixelWidth * 18
    visible:    _monitor.monitoring && _bands.length > 0 && !isNaN(_altitude)

    readonly property var   _monitor:       GeoZoneMonitor
    readonly property var   _active:        GeoZoneMonitor.activeVehicle    ///< GeoZoneVehicleMonitor, null without
    readonly property var   _bands:         _active?.verticalBands ?? []
    readonly property real  _altitude:      _active?.altitudeAMSL ?? NaN
    readonly property real  _terrain:       _active?.terrainAMSL ?? NaN
    readonly property bool  _stale:         _active?.positionUnavailable === true
    readonly property bool  _degraded:      _active?.altitudeDegraded === true && !_stale
    readonly property real  _margin:        ScreenTools.defaultFontPixelWidth / 2
    readonly property real  _barX:          _margin
    readonly property real  _barWidth:      ScreenTools.defaultFontPixelWidth * 2.5
    readonly property real  _labelX:        _barX + _barWidth + _margin
    readonly property real  _labelHeight:   lineMetrics.implicitHeight * 2
    readonly property real  _plotTop:       header.y + header.height + _margin
    readonly property real  _plotHeight:    height - _plotTop - _margin
    readonly property real  _reach:         300     ///< Meters within which a limit keeps the strip expanded
    readonly property color _background:    Qt.rgba(qgcPal.window.r, qgcPal.window.g, qgcPal.window.b, 0.8)

    property bool _expandedByUser: false

    /// The bands with the same restriction, floor and ceiling as one, with their names, the most severe last so that
    /// it is drawn on top
    readonly property var _merged: {
        const merged = []
        const byKey = {}
        for (const band of _bands) {
            const key = band.restriction + "/" + Math.round(band.floor) + "/" + Math.round(band.ceiling)
            let entry = byKey[key]
            if (!entry) {
                entry = { restriction: band.restriction, floor: band.floor, ceiling: band.ceiling, limitsText: band.limitsText, names: [] }
                byKey[key] = entry
                merged.push(entry)
            }
            if (entry.names.indexOf(band.name) < 0) {
                entry.names.push(band.name)
            }
        }
        return merged.sort((a, b) => b.restriction - a.restriction)
    }

    readonly property bool _limitNear: {
        for (const band of _merged) {
            for (const limit of [ band.floor, band.ceiling ]) {
                if (!isNaN(limit) && Math.abs(limit - _altitude) <= _reach) {
                    return true
                }
            }
        }
        return false
    }

    /// Most severe band the vehicle is in, null when it is in none
    readonly property var _insideBand: {
        for (let i = _merged.length - 1; i >= 0; i--) {
            const band = _merged[i]
            if ((isNaN(band.floor) || band.floor <= _altitude) && (isNaN(band.ceiling) || _altitude <= band.ceiling)) {
                return band
            }
        }
        return null
    }

    readonly property bool _collapsed: !_limitNear && !_expandedByUser

    // Shown range: the ground or below the vehicle up to the limits near it
    readonly property real _bottom: isNaN(_terrain) ? _altitude - 150 : Math.min(_terrain, _altitude - 30)
    readonly property real _top: {
        let top = Math.max(_altitude + 100, _bottom + 200)
        for (const band of _merged) {
            for (const limit of [ band.floor, band.ceiling ]) {
                if (!isNaN(limit) && limit > top && limit < _altitude + 1000) {
                    top = limit + 30
                }
            }
        }
        return top
    }

    /// Label positions by index of _merged, NaN for the labels which don't fit: next to their bands, moved apart so
    /// that they don't overlap
    readonly property var _labelYs: {
        const plotBottom = _plotTop + _plotHeight
        const entries = _merged.map((band, index) => ({ index: index, y: ((_floorY(band) + _ceilingY(band)) - _labelHeight) / 2 }))
        entries.sort((a, b) => a.y - b.y)
        let bottom = _plotTop
        for (const entry of entries) {
            entry.y = Math.max(entry.y, bottom)
            bottom = entry.y + _labelHeight
        }
        let top = plotBottom
        for (let i = entries.length - 1; i >= 0; i--) {
            entries[i].y = Math.min(entries[i].y, top - _labelHeight)
            top = entries[i].y
        }
        const ys = []
        for (const entry of entries) {
            ys[entry.index] = entry.y >= _plotTop - 0.5 ? entry.y : NaN
        }
        return ys
    }

    function _y(altitude) {
        return _plotTop + (_top - altitude) / (_top - _bottom) * _plotHeight
    }

    function _floorY(band) {
        return isNaN(band.floor) ? _plotTop + _plotHeight : Math.min(_y(band.floor), _plotTop + _plotHeight)
    }

    function _ceilingY(band) {
        return isNaN(band.ceiling) ? _plotTop : Math.max(_y(band.ceiling), _plotTop)
    }

    function _names(band) {
        return band.names.length > 1 ? qsTr("%1 (+%2)").arg(band.names[0]).arg(band.names.length - 1) : band.names[0]
    }

    function _heightText(altitude) {
        // Above the ground when it is known, as the zone limits mostly are
        return isNaN(_terrain) ? GeoZoneStyle.verticalText(altitude, qsTr("AMSL")) : GeoZoneStyle.verticalText(altitude - _terrain)
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        id:             lineMetrics
        text:           "X"
        font.pointSize: ScreenTools.smallFontPointSize
        visible:        false
    }

    // Collapsed: what the vehicle is in, without limits near it
    Rectangle {
        id:                     chip
        anchors.verticalCenter: parent.verticalCenter
        width:                  chipColumn.x + chipColumn.width + control._margin * 2
        height:                 chipColumn.height + control._margin * 2
        radius:                 control._margin
        color:                  control._background
        opacity:                control._stale ? 0.5 : 1
        visible:                control._collapsed

        Rectangle {
            id:                     chipDot
            x:                      control._margin * 2
            anchors.verticalCenter: parent.verticalCenter
            width:                  ScreenTools.defaultFontPixelHeight * 0.6
            height:                 width
            radius:                 width / 2
            color:                  GeoZoneStyle.restrictionColor(control._merged.length > 0 ? control._merged[control._merged.length - 1].restriction : -1)
        }

        Column {
            id:                     chipColumn
            x:                      chipDot.x + chipDot.width + control._margin
            anchors.verticalCenter: parent.verticalCenter

            readonly property real _maxWidth: control.width - x - control._margin

            QGCLabel {
                width:          Math.min(implicitWidth, chipColumn._maxWidth)
                text:           qsTr("Zone altitudes")
                font.pointSize: ScreenTools.smallFontPointSize
                font.bold:      true
                elide:          Text.ElideRight
            }

            QGCLabel {
                width:          Math.min(implicitWidth, chipColumn._maxWidth)
                text:           control._insideBand ? qsTr("In %1").arg(control._names(control._insideBand)) :
                                                      qsTr("No limit within %1").arg(GeoZoneStyle.verticalText(control._reach))
                font.pointSize: ScreenTools.smallFontPointSize
                textFormat:     Text.PlainText
                elide:          Text.ElideRight
            }
        }

        QGCMouseArea {
            fillItem:   chip
            onClicked:  control._expandedByUser = true
        }
    }

    Rectangle {
        id:             strip
        anchors.fill:   parent
        radius:         control._margin
        color:          control._background
        visible:        !control._collapsed

        Column {
            id:         header
            x:          control._margin
            y:          control._margin
            width:      control.width - (control._margin * 2)
            spacing:    control._margin / 2

            QGCLabel {
                width:          parent.width
                text:           isNaN(control._terrain) ? qsTr("Zone altitudes") : qsTr("Zone altitudes, above ground")
                font.pointSize: ScreenTools.smallFontPointSize
                font.bold:      true
                wrapMode:       Text.WordWrap
            }

            QGCLabel {
                width:          parent.width
                text:           qsTr("Vehicle %1").arg(control._heightText(control._altitude)) + (control._degraded ? " \u2248" : "")
                font.pointSize: ScreenTools.smallFontPointSize
                opacity:        control._stale ? 0.5 : 1
            }

            Flow {
                width:      parent.width
                spacing:    control._margin
                visible:    control._stale || control._degraded

                GeoZoneBadge {
                    text:       qsTr("Stale")
                    color:      qgcPal.colorOrange
                    visible:    control._stale
                }

                GeoZoneBadge {
                    text:       qsTr("AGL estimated")
                    color:      qgcPal.colorYellow
                    visible:    control._degraded
                }
            }
        }

        // Collapses again while no limit is near
        QGCMouseArea {
            fillItem:   header
            enabled:    control._expandedByUser
            onClicked:  control._expandedByUser = false
        }

        // Ground
        Rectangle {
            x:          control._barX
            y:          control._y(control._terrain)
            width:      control._barWidth
            height:     Math.max(0, control._plotTop + control._plotHeight - y)
            color:      GeoZoneStyle.terrainColor
            visible:    !isNaN(control._terrain) && y < control._plotTop + control._plotHeight
        }

        Repeater {
            model: control._merged

            Item {
                id:             bandItem
                anchors.fill:   parent
                opacity:        control._stale ? 0.5 : 1

                required property var   modelData
                required property int   index

                readonly property real _floorY:     control._floorY(modelData)
                readonly property real _ceilingY:   control._ceilingY(modelData)
                readonly property real _labelY:     control._labelYs[index] ?? NaN
                readonly property color _color:     GeoZoneStyle.restrictionColor(modelData.restriction)

                // Overlapping bands share the bar, the most severe on top
                Rectangle {
                    x:              control._barX
                    y:              bandItem._ceilingY
                    width:          control._barWidth
                    height:         Math.max(1, bandItem._floorY - bandItem._ceilingY)
                    color:          GeoZoneStyle.fillColor(bandItem.modelData.restriction, true)
                    border.color:   bandItem._color
                    border.width:   1
                }

                Rectangle {
                    x:          control._labelX
                    y:          bandItem._labelY + (control._labelHeight - height) / 2
                    width:      control._margin
                    height:     control._labelHeight * 0.8
                    color:      bandItem._color
                    visible:    !isNaN(bandItem._labelY)
                }

                Column {
                    x:          control._labelX + control._margin * 2
                    y:          bandItem._labelY
                    width:      control.width - x - control._margin
                    visible:    !isNaN(bandItem._labelY)

                    QGCLabel {
                        width:          parent.width
                        text:           control._names(bandItem.modelData)
                        textFormat:     Text.PlainText
                        elide:          Text.ElideRight
                        font.pointSize: ScreenTools.smallFontPointSize
                    }

                    QGCLabel {
                        width:          parent.width
                        text:           bandItem.modelData.limitsText
                        textFormat:     Text.PlainText
                        elide:          Text.ElideRight
                        font.pointSize: ScreenTools.smallFontPointSize
                    }
                }
            }
        }

        // Vehicle, at its last known altitude while its position is lost
        Rectangle {
            x:              control._barX - control._margin
            y:              control._y(control._altitude) - (height / 2)
            width:          control._barWidth + (control._margin * 2)
            height:         3
            color:          qgcPal.text
            border.color:   qgcPal.window
            border.width:   1
            opacity:        control._stale ? 0.5 : 1
        }
    }
}
