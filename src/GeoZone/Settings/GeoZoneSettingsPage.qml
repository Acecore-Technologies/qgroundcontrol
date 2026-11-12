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

import QGroundControl.AppSettings
import QGroundControl.FactControls
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.ScreenTools

SettingsPage {
    property var _geoZoneSettings: GeoZoneManager.settings

    // Like the other settings pages, the groups don't stretch with their descriptions so labels stay near their values
    readonly property real _groupWidth: ScreenTools.defaultFontPixelWidth * 80
    readonly property real _fieldWidth: ScreenTools.defaultFontPixelWidth * 16

    SettingsGroupLayout {
        Layout.fillWidth:       true
        Layout.preferredWidth:  _groupWidth
        heading:                qsTr("Sources")
        headingDescription:     qsTr("Zones are shown on the maps, checked against missions and alerted in flight. Plan View > Fence > From Geozones... adds them as exclusion fences.")

        GeoZoneSourcesPanel {
            Layout.fillWidth: true
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:       true
        Layout.preferredWidth:  _groupWidth
        heading:                qsTr("Authorisations")
        headingDescription:     qsTr("Zones you are authorised to fly in are left out of the mission check, flight alerts and guided command checks while the authorisation is valid.")

        GeoZoneAuthorisationsPanel {
            Layout.fillWidth: true
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:       true
        Layout.preferredWidth:  _groupWidth
        heading:                qsTr("Display")

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.showZones
            visible:            fact.visible
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:       true
        Layout.preferredWidth:  _groupWidth
        heading:                qsTr("Flight Alerts")
        headingDescription:     qsTr("Always on: connected vehicles are warned when they approach or enter an active restricted geozone or U-space airspace, and the alerts are logged.")

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.audioAlerts
            visible:            fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:        true
            textFieldPreferredWidth: _fieldWidth
            label:                   fact.shortDescription
            fact:                    _geoZoneSettings.approachExtraTime
            visible:                 fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:        true
            textFieldPreferredWidth: _fieldWidth
            label:                   fact.shortDescription
            fact:                    _geoZoneSettings.approachExtraDistance
            visible:                 fact.visible
        }

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            text:               qsTr("The manufacturer's alert thresholds for the vehicle type set when a vehicle is warned, from its speed, position accuracy, the reaction time, its braking or turning distance, the wind and a safety margin. The extra time and distance only make the alerts earlier.")
        }

        LabelledFactTextField {
            Layout.fillWidth:        true
            textFieldPreferredWidth: _fieldWidth
            label:                   fact.shortDescription
            fact:                    _geoZoneSettings.activationCaution
            visible:                 fact.visible
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:       true
        Layout.preferredWidth:  _groupWidth
        heading:                qsTr("Traffic")
        headingDescription:     _geoZoneSettings.trafficEnabled.longDescription

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.trafficEnabled
            visible:            fact.visible
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:       true
        Layout.preferredWidth:  _groupWidth
        heading:                qsTr("Records")
        headingDescription:     qsTr("Each flight is recorded in the GeoZones folder of the telemetry folder when the vehicle disarms: its track, the zones it entered or passed near, the permits used and the alerts with your answers. Records and exports can name the operator and aircraft of the Remote ID settings.")

        LabelledFactComboBox {
            Layout.fillWidth:   true
            label:              fact.shortDescription
            fact:               _geoZoneSettings.recordRetention
            indexModel:         false
            visible:            fact.visible
        }

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.includeOperatorDetails
            visible:            fact.visible
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:       true
        Layout.preferredWidth:  _groupWidth
        heading:                qsTr("Altitude Limits")
        headingDescription:     _geoZoneSettings.qnhHPa.longDescription

        LabelledFactTextField {
            Layout.fillWidth:        true
            textFieldPreferredWidth: _fieldWidth
            label:                   fact.shortDescription
            fact:                    _geoZoneSettings.qnhHPa
            visible:                 fact.visible
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:       true
        Layout.preferredWidth:  _groupWidth
        heading:                qsTr("Data Updates")
        headingDescription:     qsTr("Feeds are downloaded again periodically. Sources with older data are reported as outdated, or as expired when outdated data is treated as invalid; their zones still apply. Services taking an area download the zones around the planned mission, the active vehicle and the map view.")

        LabelledFactTextField {
            Layout.fillWidth:        true
            textFieldPreferredWidth: _fieldWidth
            label:                   fact.shortDescription
            fact:                    _geoZoneSettings.feedRefreshHours
            visible:                 fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:        true
            textFieldPreferredWidth: _fieldWidth
            label:                   fact.shortDescription
            fact:                    _geoZoneSettings.liveFeedRefreshMinutes
            visible:                 fact.visible
        }

        LabelledFactTextField {
            Layout.fillWidth:        true
            textFieldPreferredWidth: _fieldWidth
            label:                   fact.shortDescription
            fact:                    _geoZoneSettings.staleDays
            visible:                 fact.visible
        }

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.outdatedInvalid
            visible:            fact.visible
        }

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.followAreaOfInterest
            visible:            fact.visible
        }

        FactCheckBoxSlider {
            Layout.fillWidth:   true
            text:               fact.shortDescription
            fact:               _geoZoneSettings.followMapView
            visible:            fact.visible
            enabled:            _geoZoneSettings.followAreaOfInterest.rawValue
        }

        // A catalogue of feeds extending or changing the built-in ones, see GeoZoneFeedCatalogue
        ColumnLayout {
            Layout.fillWidth:   true
            visible:            _geoZoneSettings.feedCatalogueUrl.visible

            QGCLabel { text: _geoZoneSettings.feedCatalogueUrl.shortDescription }

            FactTextField {
                Layout.fillWidth:   true
                fact:               _geoZoneSettings.feedCatalogueUrl
                placeholderText:    qsTr("https://... (optional)")
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                textFormat:         Text.PlainText
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               qsTr("Adds or changes the feeds offered by Add Feed..., like a catalogue of the manufacturer. A feeds.json file in %1 does the same.")
                                        .arg(GeoZoneManager.repository.feedCatalogue.filePath.replace(/[\\/]feeds\.json$/, ""))
            }
        }
    }
}
