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

/// Non-blocking popup for the flight alerts of GeoZoneMonitor, closed after a while or when clicked. Alerts
/// arriving while it is open are added, the most severe first. A geozone alert can be acknowledged, which also stops
/// the repeats of an entry into a prohibited zone, and the alerts can be muted for a while. Repeats are shown also
/// when the alerts aren't spoken. With alerts about the active vehicle, it offers to hold or return the vehicle and to
/// climb above the ceiling of a zone, each confirmed with the guided action slider of the Fly View, and a zone the
/// vehicle is in can be muted until it leaves the zone.
Popup {
    id:             control
    // Below a guided action being confirmed, which the alerts don't hide
    y:              _confirming ?
                        _confirmDialog.mapToItem(parent, 0, _confirmDialog.height).y + ScreenTools.defaultFontPixelHeight :
                        ((stackBelow && stackBelow.visible) ?
                             stackBelow.y + stackBelow.height + ScreenTools.defaultFontPixelHeight :
                             ScreenTools.toolbarHeight + ScreenTools.defaultFontPixelHeight)
    x:              Math.round((mainWindow.width - width) * 0.5)
    width:          mainWindow.width * 0.55
    height:         alertColumn.implicitHeight + topPadding + bottomPadding
    padding:        ScreenTools.defaultFontPixelHeight * 0.5
    topPadding:     ScreenTools.defaultFontPixelHeight
    modal:          false
    focus:          false
    closePolicy:    Popup.NoAutoClose

    property var stackBelow: null   ///< Popup to show this one below while it is visible

    readonly property int   _maxAlerts:     4
    readonly property var   _monitor:       GeoZoneMonitor
    readonly property var   _guided:        globals.guidedControllerFlyView
    readonly property var   _confirmDialog: _guided ? _guided.confirmDialog : null
    readonly property bool  _confirming:    !!_confirmDialog && _confirmDialog.visible

    property var _alerts: []        ///< Shown alerts as { message, severity, alertId }, alertId 0 for fence breaches
    property int _revision: 0       ///< Changes with the zone states the actions of the alerts depend on

    /// GeoZoneMonitor.alertActions of the shown alerts, with zoneActions set for the first alert of each zone and vehicle
    readonly property var _actions: {
        if (_revision < 0) {
            return []
        }
        const zones = new Set()
        return _alerts.map(alert => {
            const actions = alert.alertId > 0 ? _monitor.alertActions(alert.alertId) : {}
            const zone = actions.vehicleId + "/" + actions.zoneKey
            actions.zoneActions = actions.zoneKey !== undefined && !zones.has(zone)
            zones.add(zone)
            return actions
        })
    }
    readonly property bool _guidable: !!_guided && _actions.some(actions => actions.activeVehicle === true)

    function show(message, severity, alertId) {
        const id = alertId === undefined ? 0 : alertId
        // A repeated alert replaces the shown one
        let alerts = visible ? _alerts.filter(alert => alert.message !== message && (id <= 0 || alert.alertId !== id)) : []
        alerts.push({ message: message, severity: severity === undefined ? Number.MAX_SAFE_INTEGER : severity, alertId: id })
        // Stable sort, the latest of equally severe alerts last
        alerts = alerts.map((alert, index) => ({ alert: alert, index: index }))
                       .sort((a, b) => (a.alert.severity - b.alert.severity) || (a.index - b.index))
                       .map(entry => entry.alert)
        _alerts = alerts.slice(0, _maxAlerts)
        open()
        closeTimer.restart()
    }

    function _remove(alertId) {
        _alerts = _alerts.filter(alert => alert.alertId !== alertId)
        if (_alerts.length === 0) {
            close()
        }
    }

    function _acknowledge(alertId) {
        _monitor.acknowledgeAlert(alertId)
        _remove(alertId)
    }

    function _muteZone(alertId) {
        _monitor.muteZone(alertId)
        _remove(alertId)
    }

    /// @return Altitude above home in meters which puts the active vehicle above the ceiling of the alert's zone, NaN
    ///         when the guided altitude change can't reach it
    function _climbTarget(actions) {
        const vehicle = QGroundControl.multiVehicleManager.activeVehicle
        if (!vehicle || actions.activeVehicle !== true || !(actions.climbHeight > 0)) {
            return NaN
        }
        const target = vehicle.altitudeRelative.rawValue + actions.climbHeight
        return target <= QGroundControl.settingsManager.flyViewSettings.guidedMaximumAltitude.rawValue ? target : NaN
    }

    /// Asks to confirm a guided action of the active vehicle with the slider of the Fly View, like its own buttons
    function _confirmGuidedAction(actionCode, altitude) {
        mainWindow.showFlyView()
        _guided.confirmAction(actionCode)
        if (altitude !== undefined) {
            _guided.guidedValueSlider.setCurrentValue(QGroundControl.unitsConversion.metersToAppSettingsVerticalDistanceUnits(altitude), true)
        }
        close()
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    Connections {
        target:                                             GeoZoneMonitor
        function onAlert(message, severity, alertId)            { control.show(message, severity, alertId) }
        function onAlertRepeated(message, severity, alertId)    { control.show(message, severity, alertId) }
        // Fences stop or return the vehicle, shown above geozone alerts
        function onFenceBreach(message)                         { control.show(message, -1, 0) }
        function onZoneMutesChanged()                           { control._revision++ }
    }

    Connections {
        target:                                 GeoZoneMonitor.activeVehicle
        function onActiveMonitorChanged()       { control._revision++ }
        function onNearbyZonesChanged()         { control._revision++ }
        function onVerticalBandsChanged()       { control._revision++ }
        function onPositionUnavailableChanged() { control._revision++ }
    }

    Timer {
        id:             closeTimer
        interval:       15000
        onTriggered:    control.close()
    }

    background: Rectangle {
        anchors.fill:   parent
        color:          qgcPal.alertBackground
        radius:         ScreenTools.defaultFontPixelHeight * 0.5
        border.color:   qgcPal.alertBorder
        border.width:   2

        // Clicks outside the buttons close the popup
        MouseArea {
            anchors.fill:   parent
            onClicked:      control.close()
        }

        Rectangle {
            anchors.horizontalCenter:   parent.horizontalCenter
            anchors.top:                parent.top
            anchors.topMargin:          -(height / 2)
            color:                      qgcPal.alertBackground
            radius:                     ScreenTools.defaultFontPixelHeight * 0.25
            border.color:               qgcPal.alertBorder
            border.width:               1
            width:                      titleLabel.contentWidth + _margins
            height:                     titleLabel.contentHeight + _margins

            property real _margins: ScreenTools.defaultFontPixelHeight * 0.25

            QGCLabel {
                id:                 titleLabel
                anchors.centerIn:   parent
                text:               qsTr("Geozone")
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.alertText
            }
        }
    }

    contentItem: ColumnLayout {
        id:         alertColumn
        spacing:    ScreenTools.defaultFontPixelHeight * 0.25

        Repeater {
            model: control._alerts

            RowLayout {
                id:                 alertRow
                Layout.fillWidth:   true
                spacing:            ScreenTools.defaultFontPixelWidth

                required property var   modelData
                required property int   index

                readonly property var   _actions:       control._actions[index] || ({})
                readonly property real  _climbTarget:   _actions.zoneActions === true ? control._climbTarget(_actions) : NaN

                QGCLabel {
                    Layout.fillWidth:       true
                    wrapMode:               Text.WordWrap
                    horizontalAlignment:    Text.AlignHCenter
                    textFormat:             Text.PlainText
                    color:                  qgcPal.alertText
                    text:                   alertRow.modelData.message
                }

                QGCButton {
                    text:       qsTr("Climb Above Ceiling")
                    visible:    !isNaN(alertRow._climbTarget) && !!control._guided && control._guided.showChangeAlt
                    onClicked:  control._confirmGuidedAction(control._guided.actionChangeAlt, alertRow._climbTarget)
                }

                QGCButton {
                    text:       qsTr("Mute Zone")
                    visible:    alertRow._actions.zoneActions === true && alertRow._actions.inside === true && alertRow._actions.zoneMuted !== true
                    onClicked:  control._muteZone(alertRow.modelData.alertId)
                }

                QGCButton {
                    text:       qsTr("Acknowledge")
                    visible:    alertRow.modelData.alertId > 0
                    onClicked:  control._acknowledge(alertRow.modelData.alertId)
                }
            }
        }

        RowLayout {
            Layout.alignment:   Qt.AlignHCenter
            spacing:            ScreenTools.defaultFontPixelWidth

            QGCButton {
                text:       qsTr("Hold")
                visible:    control._guidable && control._guided.showPause
                onClicked:  control._confirmGuidedAction(control._guided.actionPause)
            }

            QGCButton {
                text:       qsTr("Return")
                visible:    control._guidable && control._guided.showRTL
                onClicked:  control._confirmGuidedAction(control._guided.actionRTL)
            }

            QGCButton {
                text:       control._monitor.alertsMuted ? qsTr("Unmute Alerts") : qsTr("Mute Alerts for %1 min").arg(Math.round(control._monitor.defaultMuteSecs / 60))
                onClicked:  control._monitor.alertsMuted ? control._monitor.unmuteAlerts() : control._monitor.muteAlerts(0)
            }

            QGCButton {
                text:       qsTr("Close")
                onClicked:  control.close()
            }
        }
    }
}
