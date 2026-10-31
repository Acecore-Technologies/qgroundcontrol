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
import QGroundControl.Palette

/// Shows whether the active vehicle is in or near restricted geozones, with the map legend and geozone options
Item {
    id:             control
    width:          geoZoneIcon.width * 1.1
    anchors.top:    parent.top
    anchors.bottom: parent.bottom

    property bool showIndicator: QGroundControl.geoZoneManager.zones.count > 0

    readonly property var _monitor:         QGroundControl.geoZoneMonitor
    readonly property var _geoZoneSettings: QGroundControl.settingsManager.geoZoneSettings
    readonly property bool _alertsEnabled:  _geoZoneSettings.alertsEnabled.rawValue

    function _restrictionColor(restriction) {
        return restriction < 0 ? qgcPal.text : GeoZoneStyle.restrictionColor(restriction)
    }

    function _distanceText(meters) {
        return Math.round(QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnits(meters)) + " " +
                QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString
    }

    QGCPalette { id: qgcPal }

    QGCColoredImage {
        id:                 geoZoneIcon
        width:              height
        anchors.top:        parent.top
        anchors.bottom:     parent.bottom
        source:             "qrc:/qml/QGroundControl/GeoZone/GeoZone.svg"
        fillMode:           Image.PreserveAspectFit
        sourceSize.height:  height
        color:              control._restrictionColor(_monitor.monitoring ? _monitor.alertRestriction : -1)
        opacity:            _monitor.monitoring ? 1 : 0.5
    }

    MouseArea {
        anchors.fill:   parent
        onClicked:      mainWindow.showIndicatorDrawer(geoZonePage, control)
    }

    Component {
        id: geoZonePage

        ToolIndicatorPage {
            showExpand: true

            contentComponent: Component {
                ColumnLayout {
                    spacing: ScreenTools.defaultFontPixelHeight / 2

                    SettingsGroupLayout {
                        heading:            qsTr("Nearby Geozones")
                        headingDescription: qsTr("Active prohibited, authorisation required, conditional and U-space zones within %1 of the active vehicle, considering altitude limits.").arg(control._distanceText(_geoZoneSettings.approachDistance.rawValue))

                        QGCLabel {
                            text: !control._alertsEnabled ? qsTr("Flight alerts are off") :
                                      (QGroundControl.multiVehicleManager.vehicles.count === 0 ? qsTr("No vehicle connected") : qsTr("No zones nearby"))
                            visible: !_monitor.monitoring || _monitor.nearbyZones.length === 0
                        }

                        Repeater {
                            model: _monitor.monitoring ? _monitor.nearbyZones : []

                            RowLayout {
                                spacing: ScreenTools.defaultFontPixelWidth

                                Rectangle {
                                    width:  ScreenTools.defaultFontPixelHeight * 0.75
                                    height: width
                                    radius: width / 2
                                    color:  control._restrictionColor(modelData.zone.restriction)
                                }

                                QGCLabel {
                                    Layout.fillWidth:   true
                                    text:               modelData.zone.name !== "" ? modelData.zone.name : modelData.zone.identifier
                                }

                                QGCLabel {
                                    text: modelData.inside ? qsTr("Inside") : control._distanceText(modelData.distance)
                                }

                                QGCLabel {
                                    font.pointSize: ScreenTools.smallFontPointSize
                                    text:           modelData.zone.restrictionText + "\n" + modelData.zone.verticalLimitsText
                                }
                            }
                        }
                    }

                    SettingsGroupLayout {
                        heading: qsTr("Map")

                        GeoZoneLegend { }

                        GeoZoneSummary {
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            expandedComponent: Component {
                ColumnLayout {
                    spacing: ScreenTools.defaultFontPixelHeight / 2

                    SettingsGroupLayout {
                        Layout.fillWidth: true

                        FactCheckBoxSlider {
                            Layout.fillWidth:   true
                            text:               fact.shortDescription
                            fact:               _geoZoneSettings.showZones
                            visible:            fact.visible
                        }

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
                            enabled:            control._alertsEnabled
                        }

                        FactCheckBoxSlider {
                            Layout.fillWidth:   true
                            text:               fact.shortDescription
                            fact:               _geoZoneSettings.logAlerts
                            visible:            fact.visible
                            enabled:            control._alertsEnabled
                        }

                        LabelledFactTextField {
                            Layout.fillWidth:   true
                            label:              fact.shortDescription
                            fact:               _geoZoneSettings.approachDistance
                            visible:            fact.visible
                            enabled:            control._alertsEnabled
                        }

                        LabelledFactTextField {
                            Layout.fillWidth:   true
                            label:              fact.shortDescription
                            fact:               _geoZoneSettings.approachVerticalDistance
                            visible:            fact.visible
                            enabled:            control._alertsEnabled
                        }
                    }

                    QGCButton {
                        Layout.fillWidth:   true
                        text:               qsTr("Manage Geozones...")
                        onClicked: {
                            mainWindow.closeIndicatorDrawer()
                            mainWindow.showSettingsTool(qsTr("Geozones"))
                        }
                    }
                }
            }
        }
    }
}
