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
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools
import QGroundControl.GeoZone

/// Non-blocking popup for the flight alerts of GeoZoneMonitor (GeoZoneMonitor.alerts), the most severe first. An alert
/// of the vehicle approaching or entering a restricted zone asks whether the pilot has the legal permission to fly
/// there: "I do" acknowledges it and silences the zone until the vehicle leaves it and its approach range. The popup
/// stays open while such a question is unanswered, otherwise it closes after a while. Other alerts are dismissed. The
/// overflow menu holds, returns or climbs the active vehicle above the ceilings of the zones, each confirmed with the
/// guided action slider of the Fly View, and on mobile devices mutes the alerts for a while. While the active vehicle is
/// about to breach or breaches its own fence, GeoZoneFenceAlert tells what the autopilot does.
Popup {
    id:             control
    // Below a guided action being confirmed, which the alerts don't hide
    y:              _confirming ?
                        _confirmDialog.mapToItem(parent, 0, _confirmDialog.height).y + ScreenTools.defaultFontPixelHeight :
                        ((stackBelow && stackBelow.visible) ?
                             stackBelow.y + stackBelow.height + ScreenTools.defaultFontPixelHeight :
                             ScreenTools.toolbarHeight + ScreenTools.defaultFontPixelHeight)
    x:              Math.round((mainWindow.width - width) * 0.5)
    width:          Math.min(mainWindow.width - ScreenTools.defaultFontPixelWidth * 2, ScreenTools.defaultFontPixelWidth * 60)
    // Scrolls rather than leaving the window
    height:         Math.min(alertColumn.implicitHeight + topPadding + bottomPadding, Math.max(mainWindow.height - y - ScreenTools.defaultFontPixelHeight, ScreenTools.defaultFontPixelHeight * 6))
    padding:        ScreenTools.defaultFontPixelHeight * 0.5
    topPadding:     ScreenTools.defaultFontPixelHeight
    modal:          false
    focus:          false
    closePolicy:    Popup.NoAutoClose

    property var stackBelow: null   ///< Popup to show this one below while it is visible

    readonly property var   _monitor:       GeoZoneMonitor
    readonly property var   _alerts:        GeoZoneMonitor.alerts
    readonly property var   _guided:        globals.guidedControllerFlyView
    readonly property var   _confirmDialog: _guided ? _guided.confirmDialog : null
    readonly property bool  _confirming:    !!_confirmDialog && _confirmDialog.visible
    readonly property bool  _guidable:      !!_guided && _alerts.activeVehicleAlerts
    readonly property bool  _questions:     _alerts.questionCount > 0
    readonly property color _textColor:     "white"
    readonly property real  _climbTarget:   _guidable ? _climbTargetFor(_alerts.climbHeight) : NaN
    readonly property bool  _showHold:      _guidable && _guided.showPause
    readonly property bool  _showReturn:    _guidable && _guided.showRTL
    readonly property bool  _showClimb:     _guidable && !isNaN(_climbTarget) && _guided.showChangeAlt
    readonly property bool  _muteInMenu:    ScreenTools.isMobile
    readonly property string _muteText:     _monitor.alertsMuted ? qsTr("Unmute Alerts") : qsTr("Mute Alerts for %1 min").arg(Math.round(_monitor.defaultMuteSecs / 60))

    /// @return Altitude above home in meters which puts the active vehicle the climb height higher, NaN when the guided
    ///         altitude change can't reach it
    function _climbTargetFor(climbHeight) {
        const vehicle = QGroundControl.multiVehicleManager.activeVehicle
        if (!vehicle || !(climbHeight > 0)) {
            return NaN
        }
        const target = vehicle.altitudeRelative.rawValue + climbHeight
        return target <= QGroundControl.settingsManager.flyViewSettings.guidedMaximumAltitude.rawValue ? target : NaN
    }

    /// Asks to confirm a guided action of the active vehicle with the slider of the Fly View, like its own buttons
    function _confirmGuidedAction(actionCode, altitude) {
        mainWindow.showFlyView()
        _guided.confirmAction(actionCode)
        if (altitude !== undefined) {
            _guided.guidedValueSlider.setCurrentValue(QGroundControl.unitsConversion.metersToAppSettingsVerticalDistanceUnits(altitude), true)
        }
        // Unanswered questions stay below the confirmation
        if (!_questions) {
            _alerts.clear()
        }
    }

    function _toggleMute() {
        if (_monitor.alertsMuted) {
            _monitor.unmuteAlerts()
        } else {
            _monitor.muteAlerts(0)
        }
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    Connections {
        target: control._alerts
        function onAlertShown() {
            control.open()
            closeTimer.restart()
        }
        function onCountChanged() {
            if (control._alerts.count === 0) {
                control.close()
            }
        }
        function onQuestionCountChanged() {
            if (!control._questions && control.visible) {
                closeTimer.restart()
            }
        }
    }

    // Closes by itself unless a question waits for an answer, or the active vehicle breaches its fence
    Timer {
        id:             closeTimer
        interval:       15000
        onTriggered: {
            if (fenceAlert.visible) {
                restart()
            } else if (!control._questions) {
                control._alerts.clear()
            }
        }
    }

    // Dark, unlike the zone fills of the map
    background: Rectangle {
        anchors.fill:   parent
        color:          Qt.rgba(0.11, 0.11, 0.12, 0.95)
        radius:         ScreenTools.defaultFontPixelHeight * 0.5
        border.color:   control._textColor
        border.width:   2

        Rectangle {
            anchors.horizontalCenter:   parent.horizontalCenter
            anchors.top:                parent.top
            anchors.topMargin:          -(height / 2)
            color:                      parent.color
            radius:                     ScreenTools.defaultFontPixelHeight * 0.25
            border.color:               control._textColor
            border.width:               1
            width:                      titleLabel.contentWidth + _margins * 4
            height:                     titleLabel.contentHeight + _margins

            property real _margins: ScreenTools.defaultFontPixelHeight * 0.25

            QGCLabel {
                id:                 titleLabel
                anchors.centerIn:   parent
                text:               qsTr("Geozone")
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              control._textColor
            }
        }
    }

    contentItem: QGCFlickable {
        contentWidth:   width
        contentHeight:  alertColumn.implicitHeight
        indicatorColor: control._textColor

        ColumnLayout {
            id:         alertColumn
            width:      parent.width
            spacing:    ScreenTools.defaultFontPixelHeight * 0.5

            Repeater {
                model: control._alerts

                ColumnLayout {
                    id:                 alertItem
                    Layout.fillWidth:   true
                    spacing:            ScreenTools.defaultFontPixelHeight * 0.25

                    required property int       alertId
                    required property string    message
                    required property bool      question

                    RowLayout {
                        Layout.fillWidth:   true
                        spacing:            ScreenTools.defaultFontPixelWidth

                        QGCLabel {
                            Layout.fillWidth:   true
                            wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                            textFormat:         Text.PlainText
                            color:              control._textColor
                            text:               alertItem.message
                        }

                        QGCButton {
                            text:       qsTr("Dismiss")
                            visible:    !alertItem.question
                            onClicked:  control._monitor.dismissAlert(alertItem.alertId)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth:   true
                        spacing:            ScreenTools.defaultFontPixelWidth
                        visible:            alertItem.question

                        QGCLabel {
                            Layout.fillWidth:   true
                            wrapMode:           Text.WordWrap
                            color:              control._textColor
                            font.bold:          true
                            text:               qsTr("Do you have legal permission to fly here?")
                        }

                        QGCButton {
                            text:       qsTr("I do")
                            primary:    true
                            onClicked:  control._monitor.approveAlert(alertItem.alertId)
                        }
                    }
                }
            }

            // The fence of the active vehicle
            GeoZoneFenceAlert {
                id:                 fenceAlert
                Layout.fillWidth:   true
                textColor:          control._textColor
            }

            RowLayout {
                Layout.alignment:   Qt.AlignHCenter
                spacing:            ScreenTools.defaultFontPixelWidth

                QGCButton {
                    id:         moreButton
                    text:       "\u2022\u2022\u2022"
                    visible:    control._showHold || control._showReturn || control._showClimb || control._muteInMenu
                    onClicked:  actionsMenu.popup(moreButton, 0, moreButton.height)
                }

                QGCButton {
                    text:       control._muteText
                    visible:    !control._muteInMenu
                    onClicked:  control._toggleMute()
                }

                QGCButton {
                    text:       qsTr("Close")
                    onClicked:  control._monitor.closeAlerts()
                }
            }
        }
    }

    QGCMenu {
        id: actionsMenu

        QGCMenuItem {
            text:           qsTr("Hold")
            visible:        control._showHold
            onTriggered:    control._confirmGuidedAction(control._guided.actionPause)
        }

        QGCMenuItem {
            text:           qsTr("Return")
            visible:        control._showReturn
            onTriggered:    control._confirmGuidedAction(control._guided.actionRTL)
        }

        QGCMenuItem {
            text:           qsTr("Climb Above Ceiling")
            visible:        control._showClimb
            onTriggered:    control._confirmGuidedAction(control._guided.actionChangeAlt, control._climbTarget)
        }

        QGCMenuItem {
            text:           control._muteText
            visible:        control._muteInMenu
            onTriggered:    control._toggleMute()
        }
    }
}
