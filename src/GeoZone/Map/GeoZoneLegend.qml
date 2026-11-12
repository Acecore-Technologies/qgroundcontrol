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
import QtQuick.Layouts

import QGroundControl.Controls
import QGroundControl.FactControls
import QGroundControl.GeoZone
import QGroundControl.ScreenTools

/// Explains how the geozones are drawn on the maps, with their colors, restriction letters and the hatching of the
/// prohibited zones, and the map filters: check boxes showing or hiding the zones by restriction, and filters by
/// altitude, schedule and source. Zones the mission check or the flight alerts report stay shown, and hidden zones
/// are still checked and alerted.
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 4

    property bool showCrossedZones: false   ///< Includes the outline of zones crossed by the mission

    readonly property var   _geoZoneManager:    GeoZoneManager
    readonly property var   _settings:          _geoZoneManager.settings
    readonly property var   _sources:           _geoZoneManager.sources
    readonly property int   _sourceIndex: {
        const key = _settings.mapSource.rawValue
        for (let i = 0; i < _sources.count; i++) {
            if (_sources.get(i).key === key) {
                return i + 1
            }
        }
        return 0
    }

    component Swatch: Rectangle {
        id:                     swatch
        property int    restriction
        property bool   active:         true

        Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight
        Layout.preferredHeight: ScreenTools.defaultFontPixelHeight
        color:                  GeoZoneStyle.fillColor(restriction, active)
        border.color:           GeoZoneStyle.restrictionColor(restriction)
        border.width:           active ? 2 : 1
        clip:                   true

        // Hatched like prohibited zones on the maps
        Repeater {
            model: swatch.restriction === GeoZone.Prohibited ? 3 : 0

            Rectangle {
                required property int index

                readonly property real _offset: (index - 1) * swatch.width / 3

                x:          (swatch.width - width) / 2 + _offset / Math.SQRT2
                y:          (swatch.height - height) / 2 + _offset / Math.SQRT2
                width:      swatch.width * 1.5
                height:     Math.max(1, Math.round(ScreenTools.defaultFontPixelHeight / 10))
                rotation:   -45
                color:      GeoZoneStyle.restrictionColor(swatch.restriction)
                opacity:    swatch.active ? 0.8 : 0.35
            }
        }
    }

    component RestrictionBadge: GeoZoneBadge {
        property int restriction

        text:       GeoZoneStyle.restrictionLetter(restriction)
        color:      GeoZoneStyle.restrictionColor(restriction)
        textColor:  GeoZoneStyle.restrictionTextColor(restriction)
    }

    component RestrictionRow: RowLayout {
        id:         restrictionRow
        spacing:    ScreenTools.defaultFontPixelWidth

        required property int       restriction
        required property string    text

        // Inline components don't see the ids of the file when used elsewhere, so the singleton rather than root
        readonly property bool _shown: (GeoZoneManager.display.hiddenRestrictions & (1 << restriction)) === 0

        QGCCheckBox {
            checked:                restrictionRow._shown
            onClicked:              GeoZoneManager.display.setRestrictionVisible(restrictionRow.restriction, checked)
            Accessible.name:        qsTr("Show %1 zones on the maps").arg(restrictionRow.text)
        }

        Swatch {
            restriction:    restrictionRow.restriction
            opacity:        restrictionRow._shown ? 1 : 0.3
        }

        RestrictionBadge {
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

    RestrictionBadge {
        id:         badgeMetrics
        visible:    false
    }

    // Aligned with the swatches of the restrictions
    GridLayout {
        columns:        4
        columnSpacing:  ScreenTools.defaultFontPixelWidth
        rowSpacing:     root.spacing

        Item { Layout.preferredWidth: checkBoxMetrics.implicitWidth }
        Swatch {
            restriction:    GeoZone.Prohibited
            active:         false
        }
        Item { Layout.preferredWidth: badgeMetrics.implicitWidth }
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
        Item {
            Layout.preferredWidth:  badgeMetrics.implicitWidth
            visible:                root.showCrossedZones
        }
        QGCLabel {
            text:       qsTr("White outline: crossed by the mission")
            visible:    root.showCrossedZones
        }
    }

    FactCheckBoxSlider {
        Layout.fillWidth:   true
        text:               fact.shortDescription
        fact:               root._settings.altitudeFilter
    }

    LabelledFactComboBox {
        Layout.fillWidth:   true
        label:              fact.shortDescription
        fact:               root._settings.scheduleFilter
        indexModel:         false
    }

    LabelledComboBox {
        id:                 sourceCombo
        Layout.fillWidth:   true
        label:              qsTr("Source")
        visible:            root._sources.count > 1
        currentIndex:       root._sourceIndex
        model: {
            const names = [ qsTr("All sources") ]
            for (let i = 0; i < root._sources.count; i++) {
                names.push(root._sources.get(i).name)
            }
            return names
        }

        // A new list of sources selects the first choice, All sources, while the source of the filter is the same:
        // the source shown is the one of the filter again
        onModelChanged: sourceCombo.currentIndex = Qt.binding(() => root._sourceIndex)

        onActivated: (index) => {
            root._settings.mapSource.rawValue = index > 0 ? root._sources.get(index - 1).key : ""
            // Selecting in the combo box replaced the binding
            sourceCombo.currentIndex = Qt.binding(() => root._sourceIndex)
        }
    }

    QGCLabel {
        Layout.fillWidth:       true
        Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 32
        wrapMode:               Text.WordWrap
        font.pointSize:         ScreenTools.smallFontPointSize
        text:                   qsTr("Filters only change the maps. Zones the mission check or the flight alerts report stay shown, and hidden zones are still checked and alerted.")
    }

    QGCButton {
        text:       qsTr("Reset Filters")
        visible:    root._geoZoneManager.display.filtered
        onClicked:  root._geoZoneManager.display.resetFilters()
    }
}
