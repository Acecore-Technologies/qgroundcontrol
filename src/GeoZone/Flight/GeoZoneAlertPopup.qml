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
/// the spoken repeats of an entry into a prohibited zone, and the spoken alerts can be muted for a while.
Popup {
    id:             control
    y:              (stackBelow && stackBelow.visible) ?
                        stackBelow.y + stackBelow.height + ScreenTools.defaultFontPixelHeight :
                        ScreenTools.toolbarHeight + ScreenTools.defaultFontPixelHeight
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
    readonly property bool  _audioAlerts:   GeoZoneManager.settings.audioAlerts.rawValue

    property var _alerts: []        ///< Shown alerts as { message, severity, alertId }, alertId 0 for fence breaches

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

    function _acknowledge(alertId) {
        _monitor.acknowledgeAlert(alertId)
        _alerts = _alerts.filter(alert => alert.alertId !== alertId)
        if (_alerts.length === 0) {
            close()
        }
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    Connections {
        target:                                             GeoZoneMonitor
        function onAlert(message, severity, alertId)            { control.show(message, severity, alertId) }
        function onAlertRepeated(message, severity, alertId)    { control.show(message, severity, alertId) }
        // Fences stop or return the vehicle, shown above geozone alerts
        function onFenceBreach(message)                         { control.show(message, -1, 0) }
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

                required property var modelData

                QGCLabel {
                    Layout.fillWidth:       true
                    wrapMode:               Text.WordWrap
                    horizontalAlignment:    Text.AlignHCenter
                    textFormat:             Text.PlainText
                    color:                  qgcPal.alertText
                    text:                   alertRow.modelData.message
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
                text:       control._monitor.alertsMuted ? qsTr("Unmute Alerts") : qsTr("Mute Alerts for %1 min").arg(Math.round(control._monitor.defaultMuteSecs / 60))
                visible:    control._audioAlerts
                onClicked:  control._monitor.alertsMuted ? control._monitor.unmuteAlerts() : control._monitor.muteAlerts(0)
            }

            QGCButton {
                text:       qsTr("Close")
                onClicked:  control.close()
            }
        }
    }
}
