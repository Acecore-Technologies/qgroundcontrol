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

import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.ScreenTools

/// Explains how the geozones are drawn on the maps
GridLayout {
    columns:        2
    columnSpacing:  ScreenTools.defaultFontPixelWidth
    rowSpacing:     ScreenTools.defaultFontPixelHeight / 4

    property bool showCrossedZones: false   ///< Includes the outline of zones crossed by the mission

    component Swatch: Rectangle {
        property int    restriction
        property bool   active:         true

        Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight
        Layout.preferredHeight: ScreenTools.defaultFontPixelHeight
        color:                  GeoZoneStyle.fillColor(restriction, active)
        border.color:           GeoZoneStyle.restrictionColor(restriction)
        border.width:           active ? 2 : 1
    }

    Swatch { restriction: GeoZone.Prohibited }
    QGCLabel { text: qsTr("Prohibited") }

    Swatch { restriction: GeoZone.ReqAuthorisation }
    QGCLabel { text: qsTr("Authorisation required") }

    Swatch { restriction: GeoZone.Conditional }
    QGCLabel { text: qsTr("Conditional") }

    Swatch { restriction: GeoZone.USpace }
    QGCLabel { text: qsTr("U-space") }

    Swatch { restriction: GeoZone.NoRestriction }
    QGCLabel { text: qsTr("No restriction") }

    Swatch {
        restriction:    GeoZone.Prohibited
        active:         false
    }
    QGCLabel { text: qsTr("Faint: not active now") }

    Rectangle {
        Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight
        Layout.preferredHeight: ScreenTools.defaultFontPixelHeight
        color:                  "transparent"
        border.color:           GeoZoneStyle.highlightColor
        border.width:           3
        visible:                showCrossedZones
    }
    QGCLabel {
        text:       qsTr("White outline: crossed by the mission")
        visible:    showCrossedZones
    }
}
