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

/// Shows whether the active vehicle is in or near restricted geozones, with the map legend and geozone options
Item {
    id:             control
    width:          geoZoneIcon.width * 1.1
    anchors.top:    parent.top
    anchors.bottom: parent.bottom

    property bool showIndicator: GeoZoneManager.zoneCount > 0

    readonly property var _monitor:         GeoZoneMonitor
    readonly property var _history:         GeoZoneMonitor.alertHistory
    readonly property var _geoZoneSettings: GeoZoneManager.settings
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
        color:              control._monitor.fenceBreached ? qgcPal.colorRed : control._restrictionColor(control._monitor.monitoring ? control._monitor.alertRestriction : -1)
        opacity:            (control._monitor.monitoring && !control._monitor.positionUnavailable) ? 1 : 0.5
    }

    // Alerts not acknowledged in the alert history
    Rectangle {
        anchors.top:    parent.top
        anchors.right:  geoZoneIcon.right
        height:         ScreenTools.defaultFontPixelHeight * 0.9
        width:          Math.max(height, badgeLabel.contentWidth + (height / 2))
        radius:         height / 2
        color:          qgcPal.colorRed
        visible:        control._history.unacknowledgedCount > 0

        QGCLabel {
            id:                 badgeLabel
            anchors.centerIn:   parent
            text:               control._history.unacknowledgedCount
            color:              "white"
            font.pointSize:     ScreenTools.smallFontPointSize
        }
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
                        headingDescription: qsTr("Active restricted and U-space zones within %1 of the active vehicle, or reached within %2 s at its velocity.").arg(control._distanceText(control._geoZoneSettings.approachDistance.rawValue)).arg(control._geoZoneSettings.approachTime.rawValue)

                        QGCLabel {
                            text: !control._alertsEnabled ? qsTr("Flight alerts are off") :
                                      (QGroundControl.multiVehicleManager.vehicles.count === 0 ? qsTr("No vehicle connected") : qsTr("No zones nearby"))
                            visible: (!control._monitor.monitoring || control._monitor.nearbyModel.count === 0) && !_pausedLabel.visible
                        }

                        QGCLabel {
                            Layout.fillWidth:   true
                            wrapMode:           Text.WordWrap
                            color:              qgcPal.colorRed
                            font.bold:          true
                            text:               qsTr("Vehicle: %1").arg(control._monitor.fenceBreachText)
                            visible:            control._monitor.fenceBreached
                        }

                        QGCLabel {
                            id:                 _pausedLabel
                            Layout.fillWidth:   true
                            wrapMode:           Text.WordWrap
                            color:              qgcPal.warningText
                            text:               qsTr("No current position from the vehicle, its alerts are paused. The zones below are from its last position.")
                            visible:            control._monitor.monitoring && control._monitor.positionUnavailable
                        }

                        Repeater {
                            model: control._monitor.monitoring ? control._monitor.nearbyModel : null

                            RowLayout {
                                id:         nearbyRow
                                spacing:    ScreenTools.defaultFontPixelWidth

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
                                }

                                QGCLabel {
                                    text: nearbyRow.inside ? qsTr("Inside") :
                                              (nearbyRow.timeToEntry >= 0 ? qsTr("%1, entry in %2 s").arg(control._distanceText(nearbyRow.distance)).arg(nearbyRow.timeToEntry) :
                                                                            control._distanceText(nearbyRow.distance))
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
                                    wrapMode:           Text.WordWrap
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
                        heading: qsTr("Map")

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
                            fact:               control._geoZoneSettings.approachVerticalDistance
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
