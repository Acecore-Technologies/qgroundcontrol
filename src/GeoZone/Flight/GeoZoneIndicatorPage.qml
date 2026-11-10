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

/// Drop-down page of GeoZoneIndicator: whether geo-awareness is available for the active vehicle and why not, whether
/// it is on the ground inside a restricted zone, the permits of this flight, whether some geozone data may be outdated,
/// the zones in or near which the active
/// vehicle is, with a badge marking them stale while the vehicle's position is lost, the ADS-B and Remote ID traffic in or near zones when it is checked,
/// the recent alerts, the map legend and the geozone options.
ToolIndicatorPage {
    id:         control
    showExpand: true

    readonly property var _monitor:         GeoZoneMonitor
    readonly property var _active:          GeoZoneMonitor.activeVehicle     ///< GeoZoneVehicleMonitor, null without
    readonly property var _history:         GeoZoneMonitor.alertHistory
    readonly property var _geoZoneSettings: GeoZoneManager.settings
    readonly property bool _stale:          _monitor.monitoring && _active?.positionUnavailable === true
    // Long zone names, alerts and descriptions wrap within the page rather than widening it off the window
    readonly property real _pageWidth:      ScreenTools.defaultFontPixelWidth * 60

    function _restrictionColor(restriction) {
        return restriction < 0 ? qgcPal.text : GeoZoneStyle.restrictionColor(restriction)
    }

    /// @return Color of a GeoZoneIntendedLevel
    function _levelColor(level) {
        return level === GeoZoneIntendedLevel.Warning ? qgcPal.colorRed : (level === GeoZoneIntendedLevel.Caution ? qgcPal.colorOrange : qgcPal.colorGrey)
    }

    function _levelText(level) {
        return level === GeoZoneIntendedLevel.Warning ? qsTr("WARNING") : (level === GeoZoneIntendedLevel.Caution ? qsTr("CAUTION") : qsTr("ADVISORY"))
    }

    /// @return The alert thresholds of the active vehicle at its current speed, see GeoZoneVehicleMonitor.alertThresholds
    function _thresholdsText(thresholds) {
        if (!thresholds || thresholds.distance === undefined) {
            return ""
        }
        const units = QGroundControl.unitsConversion
        // The warning time applies to entries predicted along the velocity
        if (thresholds.groundSpeed < 0.5 && Math.abs(thresholds.verticalSpeed) < 0.5) {
            return qsTr("Warning %1 around, %2 above or below while hovering (%3, %4 s reaction).")
                .arg(GeoZoneStyle.horizontalText(thresholds.distance)).arg(GeoZoneStyle.verticalText(thresholds.verticalDistance))
                .arg(thresholds.profileName).arg(thresholds.reactionTime)
        }
        const speed = Math.round(units.metersSecondToAppSettingsSpeedUnits(thresholds.groundSpeed)) + " " + units.appSettingsSpeedUnitsString
        return qsTr("Warning %1 / %2 s ahead at %3, %4 above or below (%5, %6 s reaction).")
            .arg(GeoZoneStyle.horizontalText(thresholds.distance)).arg(Math.round(thresholds.time)).arg(speed)
            .arg(GeoZoneStyle.verticalText(thresholds.verticalDistance)).arg(thresholds.profileName).arg(thresholds.reactionTime)
    }

    QGCPalette { id: qgcPal }

    contentComponent: Component {
        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight / 2

            // Whether the zones can be checked for the active vehicle, with the reasons when not
            RowLayout {
                Layout.preferredWidth:  control._pageWidth
                spacing:                ScreenTools.defaultFontPixelWidth

                readonly property int _state: control._monitor.awareness.state

                GeoZoneBadge {
                    Layout.alignment:   Qt.AlignTop
                    text:               control._monitor.awareness.stateText
                    textColor:          parent._state === GeoZoneAwareness.Degraded ? "black" : "white"
                    color:              parent._state === GeoZoneAwareness.Unavailable ? qgcPal.colorRed :
                                            (parent._state === GeoZoneAwareness.Degraded ? qgcPal.colorOrange : qgcPal.colorGreen)
                }

                ColumnLayout {
                    Layout.fillWidth:   true
                    spacing:            0

                    QGCLabel {
                        Layout.fillWidth:   true
                        wrapMode:           Text.WordWrap
                        font.bold:          parent.parent._state !== GeoZoneAwareness.Available
                        text:               qsTr("Geo-awareness")
                    }

                    Repeater {
                        model: control._monitor.awareness.reasons

                        QGCLabel {
                            required property string modelData

                            Layout.fillWidth:   true
                            wrapMode:           Text.WordWrap
                            textFormat:         Text.PlainText
                            text:               "\u2022 " + modelData
                        }
                    }
                }
            }

            // The vehicle on the ground inside a restricted zone, like a pre-arm message
            RowLayout {
                Layout.preferredWidth:  control._pageWidth
                spacing:                ScreenTools.defaultFontPixelWidth
                visible:                control._monitor.monitoring && (control._active?.groundWarning ?? "") !== ""

                GeoZoneBadge {
                    Layout.alignment:   Qt.AlignTop
                    text:               control._levelText(control._active?.groundLevel ?? 0)
                    textColor:          "white"
                    color:              control._levelColor(control._active?.groundLevel ?? 0)
                }

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    textFormat:         Text.PlainText
                    font.bold:          (control._active?.groundLevel ?? 0) !== GeoZoneIntendedLevel.Advisory
                    text:               control._active?.groundWarning ?? ""
                }
            }

            // Permits acknowledged for this flight, which end when the vehicle disarms or disconnects
            SettingsGroupLayout {
                Layout.preferredWidth:  control._pageWidth
                heading:                qsTr("Permits This Flight")
                headingDescription:     qsTr("Zones you confirmed the permission to enter. Their alerts stop until the vehicle disarms after the flight or disconnects.")
                visible:                control._monitor.permits.count > 0

                Repeater {
                    model: control._monitor.permits

                    RowLayout {
                        id:                 permitRow
                        Layout.fillWidth:   true
                        spacing:            ScreenTools.defaultFontPixelWidth

                        required property string    key
                        required property string    zoneName
                        required property string    restrictionText
                        required property date      time
                        required property string    originText

                        QGCLabel {
                            Layout.fillWidth:   true
                            wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                            textFormat:         Text.PlainText
                            text:               qsTr("%1 (%2)").arg(permitRow.zoneName).arg(permitRow.restrictionText)
                        }

                        QGCLabel {
                            font.pointSize: ScreenTools.smallFontPointSize
                            text:           Qt.formatTime(permitRow.time, "HH:mm") + "\n" + permitRow.originText
                        }

                        QGCButton {
                            text:       qsTr("Withdraw")
                            onClicked:  control._monitor.permits.withdraw(permitRow.key)
                        }
                    }
                }
            }

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
                headingDescription: qsTr("Active restricted and U-space zones within the alert thresholds of the active vehicle, by distance or by the time to reach them at its velocity.")

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text:               control._thresholdsText(control._active?.alertThresholds)
                    visible:            control._monitor.monitoring && text !== ""
                }

                QGCLabel {
                    text: QGroundControl.multiVehicleManager.vehicles.count === 0 ? qsTr("No vehicle connected") : qsTr("No zones nearby")
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

                // The fence action and how to override it, while the alert popup may be closed
                GeoZoneFenceAlert {
                    Layout.fillWidth:   true
                    textColor:          qgcPal.text
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
                        text:               qsTr("Speech is silenced: alerts are shown and repeated but not spoken, except new entries into prohibited zones.")
                    }

                    QGCButton {
                        text:       qsTr("Resume Speech")
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
                            text:           qsTr("Permit")
                            color:          qgcPal.warningText
                            font.pointSize: ScreenTools.smallFontPointSize
                            visible:        (control._monitor.permits.count, control._monitor.permits.permitted(nearbyRow.zone))
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
                        required property int       level
                        required property bool      acknowledged
                        required property bool      earlier

                        // The level of the alert, like the popup
                        Rectangle {
                            Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight * 0.3
                            Layout.fillHeight:      true
                            color:                  GeoZoneStyle.levelColor(alertRow.level)
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
                    fact:               control._geoZoneSettings.audioAlerts
                    visible:            fact.visible
                }

                LabelledFactTextField {
                    Layout.fillWidth:   true
                    label:              fact.shortDescription
                    fact:               control._geoZoneSettings.approachExtraTime
                    visible:            fact.visible
                }

                LabelledFactTextField {
                    Layout.fillWidth:   true
                    label:              fact.shortDescription
                    fact:               control._geoZoneSettings.approachExtraDistance
                    visible:            fact.visible
                }

                LabelledFactTextField {
                    Layout.fillWidth:   true
                    label:              fact.shortDescription
                    fact:               control._geoZoneSettings.activationCaution
                    visible:            fact.visible
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
