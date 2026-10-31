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

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Non-blocking popup for the flight alerts of GeoZoneMonitor, closed after a while or when clicked
Popup {
    id:             control
    y:              (stackBelow && stackBelow.visible) ?
                        stackBelow.y + stackBelow.height + ScreenTools.defaultFontPixelHeight :
                        ScreenTools.toolbarHeight + ScreenTools.defaultFontPixelHeight
    x:              Math.round((mainWindow.width - width) * 0.5)
    width:          mainWindow.width * 0.55
    height:         alertText.contentHeight + ScreenTools.defaultFontPixelHeight * 2
    modal:          false
    focus:          false
    closePolicy:    Popup.NoAutoClose

    property var stackBelow: null   ///< Popup to show this one below while it is visible

    function show(message) {
        alertText.text = message
        open()
        closeTimer.restart()
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    Connections {
        target:                     QGroundControl.geoZoneMonitor
        function onAlert(message)   { control.show(message) }
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

    QGCLabel {
        id:                     alertText
        width:                  control.width - ScreenTools.defaultFontPixelHeight
        anchors.centerIn:       parent
        wrapMode:               Text.WordWrap
        horizontalAlignment:    Text.AlignHCenter
        color:                  qgcPal.alertText
    }

    MouseArea {
        anchors.fill:   parent
        onClicked:      control.close()
    }
}
