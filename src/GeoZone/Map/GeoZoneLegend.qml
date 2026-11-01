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

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.ScreenTools

/// Explains how the geozones are drawn on the maps, with check boxes showing or hiding them by restriction. Hidden
/// zones are still checked and alerted.
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 4

    property bool showCrossedZones: false   ///< Includes the outline of zones crossed by the mission

    readonly property var _geoZoneManager: GeoZoneManager

    component Swatch: Rectangle {
        property int    restriction
        property bool   active:         true

        Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight
        Layout.preferredHeight: ScreenTools.defaultFontPixelHeight
        color:                  GeoZoneStyle.fillColor(restriction, active)
        border.color:           GeoZoneStyle.restrictionColor(restriction)
        border.width:           active ? 2 : 1
    }

    component RestrictionRow: RowLayout {
        id:         restrictionRow
        spacing:    ScreenTools.defaultFontPixelWidth

        required property int       restriction
        required property string    text

        readonly property bool _shown: (root._geoZoneManager.display.hiddenRestrictions & (1 << restriction)) === 0

        QGCCheckBox {
            checked:    restrictionRow._shown
            onClicked:  root._geoZoneManager.display.setRestrictionVisible(restrictionRow.restriction, checked)
        }

        Swatch {
            restriction:    restrictionRow.restriction
            opacity:        restrictionRow._shown ? 1 : 0.3
        }

        QGCLabel { text: restrictionRow.text }
    }

    RestrictionRow { restriction: GeoZone.Prohibited;       text: qsTr("Prohibited") }
    RestrictionRow { restriction: GeoZone.ReqAuthorisation; text: qsTr("Authorisation required") }
    RestrictionRow { restriction: GeoZone.Conditional;      text: qsTr("Conditional") }
    RestrictionRow { restriction: GeoZone.USpace;           text: qsTr("U-space") }
    RestrictionRow { restriction: GeoZone.NoRestriction;    text: qsTr("No restriction") }
    RestrictionRow { restriction: GeoZone.Unknown;          text: qsTr("Unknown restriction") }

    QGCCheckBox {
        id:         checkBoxMetrics
        visible:    false
    }

    // Aligned with the swatches of the restrictions
    GridLayout {
        columns:        3
        columnSpacing:  ScreenTools.defaultFontPixelWidth
        rowSpacing:     root.spacing

        Item { Layout.preferredWidth: checkBoxMetrics.implicitWidth }
        Swatch {
            restriction:    GeoZone.Prohibited
            active:         false
        }
        QGCLabel { text: qsTr("Faint: not active now") }

        Item {
            Layout.preferredWidth:  checkBoxMetrics.implicitWidth
            visible:                root.showCrossedZones
        }
        Rectangle {
            Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight
            Layout.preferredHeight: ScreenTools.defaultFontPixelHeight
            color:                  "transparent"
            border.color:           GeoZoneStyle.highlightColor
            border.width:           3
            visible:                root.showCrossedZones
        }
        QGCLabel {
            text:       qsTr("White outline: crossed by the mission")
            visible:    root.showCrossedZones
        }
    }
}
