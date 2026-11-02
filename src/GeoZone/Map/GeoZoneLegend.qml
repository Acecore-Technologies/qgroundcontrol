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
import QGroundControl.FactControls
import QGroundControl.GeoZone
import QGroundControl.ScreenTools

/// Explains how the geozones are drawn on the maps, with the map filters: check boxes showing or hiding the zones by
/// restriction, and filters by altitude, schedule and source. Zones the mission check or the flight alerts report
/// stay shown, and hidden zones are still checked and alerted.
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 4

    property bool showCrossedZones: false   ///< Includes the outline of zones crossed by the mission

    readonly property var   _geoZoneManager:    GeoZoneManager
    readonly property var   _settings:          _geoZoneManager.settings
    readonly property var   _sources:           _geoZoneManager.sources
    readonly property int   _activeSoon:        2
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

    FactCheckBoxSlider {
        Layout.fillWidth:   true
        text:               fact.shortDescription
        fact:               root._settings.altitudeFilter
    }

    LabelledFactTextField {
        Layout.fillWidth:   true
        fact:               root._settings.altitudeFilterMargin
        visible:            root._settings.altitudeFilter.rawValue
    }

    LabelledFactComboBox {
        Layout.fillWidth:   true
        label:              fact.shortDescription
        fact:               root._settings.scheduleFilter
        indexModel:         false
    }

    LabelledFactTextField {
        Layout.fillWidth:   true
        fact:               root._settings.scheduleFilterHours
        visible:            root._settings.scheduleFilter.rawValue === root._activeSoon
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
