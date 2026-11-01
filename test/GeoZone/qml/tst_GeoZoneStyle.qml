/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick
import QtTest

import QGroundControl.GeoZone

TestCase {
    name:       "GeoZoneStyle"
    when:       windowShown
    visible:    true
    width:      100
    height:     100

    // A swatch of the legend on a black background, filled and outlined in the colors of a zone's restriction
    Rectangle {
        id:     background
        width:  40
        height: 40
        color:  "black"

        Rectangle {
            id:             swatch
            anchors.centerIn: parent
            width:          30
            height:         30
            color:          GeoZoneStyle.fillColor(restriction, active)
            border.color:   GeoZoneStyle.restrictionColor(restriction)
            border.width:   2

            property int    restriction:    GeoZone.Prohibited
            property bool   active:         true
        }
    }

    function init() {
        swatch.restriction = GeoZone.Prohibited
        swatch.active = true
    }

    function test_restrictionColor_data() {
        return [
            { tag: "prohibited",        restriction: GeoZone.Prohibited,        color: "red" },
            { tag: "req authorisation", restriction: GeoZone.ReqAuthorisation,  color: "orange" },
            { tag: "conditional",       restriction: GeoZone.Conditional,       color: "yellow" },
            { tag: "no restriction",    restriction: GeoZone.NoRestriction,     color: "limegreen" },
            { tag: "u-space",           restriction: GeoZone.USpace,            color: "deepskyblue" },
            { tag: "unknown",           restriction: GeoZone.Unknown,           color: "gray" },
            { tag: "out of range",      restriction: 99,                        color: "gray" },
        ]
    }

    function test_restrictionColor(data) {
        verify(Qt.colorEqual(GeoZoneStyle.restrictionColor(data.restriction), data.color))
    }

    function test_fillColor() {
        // The color of the restriction, translucent and fainter for zones which aren't active
        const restriction = GeoZoneStyle.restrictionColor(GeoZone.Conditional)
        for (const active of [ true, false ]) {
            const fill = GeoZoneStyle.fillColor(GeoZone.Conditional, active)
            compare(fill.r, restriction.r)
            compare(fill.g, restriction.g)
            compare(fill.b, restriction.b)
            fuzzyCompare(fill.a, active ? GeoZoneStyle.activeFillAlpha : GeoZoneStyle.inactiveFillAlpha, 0.005)
        }
        verify(GeoZoneStyle.inactiveFillAlpha < GeoZoneStyle.activeFillAlpha)
    }

    function test_bindings() {
        swatch.restriction = GeoZone.USpace
        verify(Qt.colorEqual(swatch.border.color, "deepskyblue"))
        fuzzyCompare(swatch.color.a, GeoZoneStyle.activeFillAlpha, 0.005)
        swatch.active = false
        fuzzyCompare(swatch.color.a, GeoZoneStyle.inactiveFillAlpha, 0.005)
        verify(Qt.colorEqual(swatch.border.color, "deepskyblue"))
    }

    function test_rendering() {
        // Red at a quarter over black inside the red border
        waitForRendering(background)
        let image = grabImage(background)
        fuzzyCompare(image.red(20, 20), 64, 3)
        compare(image.green(20, 20), 0)
        compare(image.red(6, 20), 255)
        compare(image.red(1, 20), 0)

        // Inactive zones are fainter
        swatch.active = false
        waitForRendering(background)
        image = grabImage(background)
        fuzzyCompare(image.red(20, 20), 20, 3)
    }
}
