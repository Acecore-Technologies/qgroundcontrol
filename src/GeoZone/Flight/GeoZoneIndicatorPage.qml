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

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FactControls
import QGroundControl.GeoZone
import QGroundControl.ScreenTools
import QGroundControl.Palette

/// Drop-down page of GeoZoneIndicator: whether some geozone data may be outdated, the zones in or near which the active
/// vehicle is, with a badge marking them stale while the vehicle's position is lost, the ADS-B and Remote ID traffic in or near zones when it is checked,
/// the recent alerts, the map legend and the geozone options.
ToolIndicatorPage {
    id:         control
    showExpand: true

    readonly property var _monitor:         GeoZoneMonitor
    readonly property var _active:          GeoZoneMonitor.activeVehicle     ///< GeoZoneVehicleMonitor, null without
    readonly property var _history:         GeoZoneMonitor.alertHistory
    readonly property var _geoZoneSettings: GeoZoneManager.settings
    readonly property bool _alertsEnabled:  _geoZoneSettings.alertsEnabled.rawValue
    readonly property bool _stale:          _monitor.monitoring && _active?.positionUnavailable === true
    // Long zone names, alerts and descriptions wrap within the page rather than widening it off the window
    readonly property real _pageWidth:      ScreenTools.defaultFontPixelWidth * 60

    function _restrictionColor(restriction) {
        return restriction < 0 ? qgcPal.text : GeoZoneStyle.restrictionColor(restriction)
    }

    QGCPalette { id: qgcPal }

    contentComponent: Component {
        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight / 2

            // When the data was last pulled, and whether it expired
            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                color:              GeoZoneManager.repository.dataInvalid ? qgcPal.colorRed : qgcPal.text
                font.bold:          GeoZoneManager.repository.dataInvalid
                text:               GeoZoneManager.repository.dataInvalid ?
                                        qsTr("Expired geozone data, its zones still apply. %1").arg(GeoZoneManager.repository.lastPulledText) :
                                        GeoZoneManager.repository.lastPulledText
                visible:            text !== ""
            }

            // Rather than a message at startup
            SettingsGroupLayout {
                Layout.preferredWidth:  control._pageWidth
                heading:                qsTr("Outdated Data")
                headingDescription:     qsTr("Some geozone data may be outdated. Load current data in Application Settings > Geozones.")
                visible:                GeoZoneManager.repository.validityWarning !== ""

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    color:              qgcPal.warningText
                    textFormat:         Text.PlainText
                    text:               GeoZoneManager.repository.validityWarning
                }

                QGCButton {
                    text: qsTr("Manage Geozones...")
                    onClicked: {
                        mainWindow.closeIndicatorDrawer()
                        mainWindow.showSettingsTool(qsTr("Geozones"))
                    }
                }
            }

            SettingsGroupLayout {
                Layout.preferredWidth:  control._pageWidth
                heading:            qsTr("Nearby Geozones")
                headingDescription: qsTr("Active restricted and U-space zones within %1 of the active vehicle, or reached within %2 s at its velocity.").arg(GeoZoneStyle.horizontalText(control._geoZoneSettings.approachDistance.rawValue)).arg(control._geoZoneSettings.approachTime.rawValue)

                QGCLabel {
                    text: !control._alertsEnabled ? qsTr("Flight alerts are off") :
                              (QGroundControl.multiVehicleManager.vehicles.count === 0 ? qsTr("No vehicle connected") : qsTr("No zones nearby"))
                    visible: (!control._monitor.monitoring || !control._active?.nearbyModel.count) && !_pausedLabel.visible
                }

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    color:              qgcPal.colorRed
                    font.bold:          true
                    text:               qsTr("Vehicle: %1").arg(control._active?.fenceBreachText ?? "")
                    visible:            control._active?.fenceBreached === true
                }

                GeoZoneConformanceStatus { Layout.fillWidth: true }

                RowLayout {
                    id:                 _pausedLabel
                    Layout.fillWidth:   true
                    spacing:            ScreenTools.defaultFontPixelWidth
                    visible:            control._stale

                    GeoZoneBadge {
                        Layout.alignment:   Qt.AlignTop
                        text:               qsTr("Stale")
                        color:              qgcPal.colorOrange
                    }

                    QGCLabel {
                        Layout.fillWidth:   true
                        wrapMode:           Text.WordWrap
                        color:              qgcPal.warningText
                        text:               qsTr("Position lost: the vehicle sends no current position, its alerts are paused. The zones below are from its last position.")
                    }
                }

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    color:              qgcPal.warningText
                    text:               qsTr("The vehicle sends no height above terrain: its height above ground is estimated from terrain data, so limits above ground are less accurate.")
                    visible:            control._monitor.monitoring && control._active?.altitudeDegraded === true && !control._stale
                }

                RowLayout {
                    Layout.fillWidth:   true
                    spacing:            ScreenTools.defaultFontPixelWidth
                    visible:            control._monitor.alertsMuted

                    QGCLabel {
                        Layout.fillWidth:   true
                        wrapMode:           Text.WordWrap
                        color:              qgcPal.warningText
                        text:               qsTr("Alerts are muted: they are shown but neither spoken nor repeated, except entries into prohibited zones.")
                    }

                    QGCButton {
                        text:       qsTr("Unmute")
                        onClicked:  control._monitor.unmuteAlerts()
                    }
                }

                Repeater {
                    model: control._monitor.monitoring ? control._active?.nearbyModel ?? null : null

                    RowLayout {
                        id:                 nearbyRow
                        Layout.fillWidth:   true
                        spacing:            ScreenTools.defaultFontPixelWidth
                        opacity:            control._stale ? 0.5 : 1

                        required property var   zone
                        required property bool  inside
                        required property int   distance
                        required property int   timeToEntry

                        Rectangle {
                            Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight * 0.75
                            Layout.preferredHeight: Layout.preferredWidth
                            radius:                 width / 2
                            color:                  control._restrictionColor(nearbyRow.zone.restriction)
                        }

                        QGCLabel {
                            Layout.fillWidth:   true
                            text:               nearbyRow.zone.displayName
                            textFormat:         Text.PlainText
                            wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                        }

                        QGCLabel {
                            text:           qsTr("Approved")
                            color:          qgcPal.warningText
                            font.pointSize: ScreenTools.smallFontPointSize
                            visible:        control._monitor.approvedZoneKeys.indexOf(nearbyRow.zone.key) >= 0
                        }

                        QGCLabel {
                            text: nearbyRow.inside ? qsTr("Inside") :
                                      (nearbyRow.timeToEntry >= 0 ? qsTr("%1, entry in %2 s").arg(GeoZoneStyle.horizontalText(nearbyRow.distance)).arg(nearbyRow.timeToEntry) :
                                                                    GeoZoneStyle.horizontalText(nearbyRow.distance))
                        }

                        QGCLabel {
                            font.pointSize: ScreenTools.smallFontPointSize
                            text:           nearbyRow.zone.restrictionText + "\n" + nearbyRow.zone.verticalLimitsText
                            textFormat:     Text.PlainText
                        }
                    }
                }
            }

            SettingsGroupLayout {
                Layout.preferredWidth:  control._pageWidth
                heading:            qsTr("Traffic in or near Geozones")
                headingDescription: qsTr("ADS-B and Remote ID aircraft within %1 of the active vehicle which are in or near active zones.").arg(GeoZoneStyle.horizontalText(control._monitor.trafficRadius))
                visible:            control._geoZoneSettings.trafficEnabled.rawValue && control._monitor.monitoring

                QGCLabel {
                    text:       qsTr("No traffic in or near zones")
                    visible:    control._monitor.traffic.length === 0
                }

                Repeater {
                    model: control._monitor.traffic

                    RowLayout {
                        id:                 trafficRow
                        Layout.fillWidth:   true
                        spacing:            ScreenTools.defaultFontPixelWidth

                        required property var modelData

                        Rectangle {
                            Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight * 0.75
                            Layout.preferredHeight: Layout.preferredWidth
                            radius:                 width / 2
                            color:                  control._restrictionColor(trafficRow.modelData.zone.restriction)
                        }

                        QGCLabel {
                            text:           trafficRow.modelData.callsign
                            textFormat:     Text.PlainText
                            font.bold:      true
                        }

                        QGCLabel {
                            Layout.fillWidth:   true
                            text:               trafficRow.modelData.zone.displayName
                            textFormat:         Text.PlainText
                            elide:              Text.ElideRight
                        }

                        QGCLabel {
                            text: trafficRow.modelData.inside ? qsTr("Inside") : GeoZoneStyle.horizontalText(trafficRow.modelData.distance)
                        }

                        QGCLabel {
                            text:           GeoZoneStyle.verticalText(trafficRow.modelData.altitude, qsTr("AMSL"))
                            font.pointSize: ScreenTools.smallFontPointSize
                        }
                    }
                }
            }

            SettingsGroupLayout {
                Layout.preferredWidth:  control._pageWidth
                heading:            qsTr("Recent Alerts")
                headingDescription: qsTr("Alerts of this session and earlier today, newest first. Show puts the zone and the vehicle position on the map.")
                visible:            control._history.count > 0

                QGCListView {
                    Layout.fillWidth:       true
                    Layout.preferredHeight: Math.min(contentHeight, ScreenTools.defaultFontPixelHeight * 12)
                    clip:                   true
                    spacing:                ScreenTools.defaultFontPixelHeight / 4
                    model:                  control._history

                    delegate: RowLayout {
                        id:         alertRow
                        width:      ListView.view.width
                        spacing:    ScreenTools.defaultFontPixelWidth

                        required property int       index
                        required property date      time
                        required property string    message
                        required property int       restriction
                        required property bool      acknowledged
                        required property bool      earlier

                        Rectangle {
                            Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight * 0.75
                            Layout.preferredHeight: Layout.preferredWidth
                            radius:                 width / 2
                            color:                  control._restrictionColor(alertRow.restriction)
                        }

                        QGCLabel {
                            text:           Qt.formatTime(alertRow.time, "HH:mm:ss")
                            font.pointSize: ScreenTools.smallFontPointSize
                            opacity:        alertRow.earlier ? 0.6 : 1
                        }

                        QGCLabel {
                            Layout.fillWidth:   true
                            wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                            text:               alertRow.message
                            textFormat:         Text.PlainText
                            font.bold:          !alertRow.acknowledged
                            opacity:            alertRow.earlier ? 0.6 : 1
                        }

                        QGCButton {
                            text:       qsTr("Show")
                            onClicked: {
                                control._history.show(alertRow.index)
                                mainWindow.closeIndicatorDrawer()
                            }
                        }
                    }
                }

                QGCButton {
                    text:       qsTr("Acknowledge All")
                    visible:    control._history.unacknowledgedCount > 0
                    onClicked:  control._history.acknowledgeAll()
                }
            }

            SettingsGroupLayout {
                Layout.preferredWidth:  control._pageWidth
                heading:                qsTr("Map")

                GeoZoneLegend { }

                GeoZoneSourceSwitches {
                    Layout.fillWidth: true
                }

                QGCLabel {
                    text:           qsTr("Click a zone on the map for its details.")
                    font.pointSize: ScreenTools.smallFontPointSize
                }

                GeoZoneSearch {
                    Layout.fillWidth:   true
                    onZoneSelected:     mainWindow.closeIndicatorDrawer()
                }

                GeoZoneSummary {
                    Layout.fillWidth: true
                    showPullTime:       false
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
                    fact:               control._geoZoneSettings.showZones
                    visible:            fact.visible
                }

                FactCheckBoxSlider {
                    Layout.fillWidth:   true
                    text:               fact.shortDescription
                    fact:               control._geoZoneSettings.alertsEnabled
                    visible:            fact.visible
                }

                FactCheckBoxSlider {
                    Layout.fillWidth:   true
                    text:               fact.shortDescription
                    fact:               control._geoZoneSettings.audioAlerts
                    visible:            fact.visible
                    enabled:            control._alertsEnabled
                }

                FactCheckBoxSlider {
                    Layout.fillWidth:   true
                    text:               fact.shortDescription
                    fact:               control._geoZoneSettings.logAlerts
                    visible:            fact.visible
                    enabled:            control._alertsEnabled
                }

                LabelledFactTextField {
                    Layout.fillWidth:   true
                    label:              fact.shortDescription
                    fact:               control._geoZoneSettings.approachDistance
                    visible:            fact.visible
                    enabled:            control._alertsEnabled
                }

                LabelledFactTextField {
                    Layout.fillWidth:   true
                    label:              fact.shortDescription
                    fact:               control._geoZoneSettings.approachTime
                    visible:            fact.visible
                    enabled:            control._alertsEnabled
                }

                FactCheckBoxSlider {
                    Layout.fillWidth:   true
                    text:               fact.shortDescription
                    fact:               control._geoZoneSettings.trafficEnabled
                    visible:            fact.visible
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
