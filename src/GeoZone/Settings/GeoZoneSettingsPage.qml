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
import QGroundControl.AppSettings
import QGroundControl.FactSystem
import QGroundControl.FactControls
import QGroundControl.Controls
import QGroundControl.GeoZone

SettingsPage {
    property var _geoZoneSettings: GeoZoneManager.settings

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("Sources")
        headingDescription: qsTr("Zones are shown on the maps, checked against missions and alerted in flight. Plan View > Fence > From Geozones... adds them as exclusion fences.")

        GeoZoneSourcesPanel {
            Layout.fillWidth: true
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("Authorisations")
        headingDescription: qsTr("Zones you are authorised to fly in are left out of the mission check, flight alerts and guided command checks while the authorisation is valid.")

        GeoZoneAuthorisationsPanel {
            Layout.fillWidth: true
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("Display")

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

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.logAlerts
            visible:            fact.visible
            enabled:            _geoZoneSettings.alertsEnabled.rawValue
        }

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.alertLogRetentionDays
            visible:            fact.visible
            enabled:            _geoZoneSettings.alertsEnabled.rawValue && _geoZoneSettings.logAlerts.rawValue
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

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.approachTime
            visible:            fact.visible
            enabled:            _geoZoneSettings.alertsEnabled.rawValue
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("Traffic")
        headingDescription: _geoZoneSettings.trafficEnabled.longDescription

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.trafficEnabled
            visible:            fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.trafficRadius
            visible:            fact.visible
            enabled:            _geoZoneSettings.trafficEnabled.rawValue
        }

        LabelledFactComboBox {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.trafficAlerts
            indexModel:         false
            visible:            fact.visible
            enabled:            _geoZoneSettings.trafficEnabled.rawValue && _geoZoneSettings.alertsEnabled.rawValue
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("Operational Intent Export")
        headingDescription: qsTr("Export Operational Intent... in the Geozones panel of the Plan View writes the volumes of the planned flight for a U-space or UTM flight authorisation request. They contain the flight path with these buffers around it from the planned start.")

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.intentHorizontalBuffer
            visible:            fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.intentVerticalBuffer
            visible:            fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.intentTimeBuffer
            visible:            fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.intentMaxVolumes
            visible:            fact.visible
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("Altitude Limits")
        headingDescription: _geoZoneSettings.qnhHPa.longDescription

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.qnhHPa
            visible:            fact.visible
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("Data Updates")
        headingDescription: qsTr("Feeds are downloaded again periodically. Sources with older data are reported as outdated. Services taking an area download the zones around the planned mission and the active vehicle.")

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

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.followAreaOfInterest
            visible:            fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.areaOfInterestMargin
            visible:            fact.visible
        }
    }
}
