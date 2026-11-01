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

/// Filters hiding geozones on the maps by altitude, schedule and source, besides the restrictions of GeoZoneLegend.
/// Zones the mission check or the flight alerts report stay shown, and hidden zones are still checked and alerted.
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 4

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
