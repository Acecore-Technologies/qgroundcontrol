/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.FactSystem
import QGroundControl.FactControls
import QGroundControl.Controls

SettingsPage {
    property var _geoZoneSettings: QGroundControl.settingsManager.geoZoneSettings

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("Display")
        headingDescription: qsTr("Geozone files and feeds are loaded in Plan View > Fence > Geozones.")

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.showZones
            visible:            fact.visible
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("Flight Alerts")
        headingDescription: _geoZoneSettings.alertsEnabled.longDescription

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.alertsEnabled
            visible:            fact.visible
        }

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.audioAlerts
            visible:            fact.visible
            enabled:            _geoZoneSettings.alertsEnabled.rawValue
        }

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.approachDistance
            visible:            fact.visible
            enabled:            _geoZoneSettings.alertsEnabled.rawValue
        }

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.approachVerticalDistance
            visible:            fact.visible
            enabled:            _geoZoneSettings.alertsEnabled.rawValue
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("Data Updates")
        headingDescription: qsTr("Feeds are downloaded again periodically. Sources with older data are reported as outdated.")

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.feedRefreshHours
            visible:            fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.liveFeedRefreshMinutes
            visible:            fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.staleDays
            visible:            fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.liveStaleHours
            visible:            fact.visible
        }
    }
}
