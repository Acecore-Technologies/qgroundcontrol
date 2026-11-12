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

import QGroundControl.Controls
import QGroundControl.ScreenTools
import QGroundControl.GeoZone

/// Note about the geozone source the user added last, see GeoZoneManager::sourceNote, which the user dismisses
RowLayout {
    spacing:    ScreenTools.defaultFontPixelWidth
    visible:    GeoZoneManager.sourceNote !== ""

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        textFormat:         Text.PlainText
        font.pointSize:     ScreenTools.smallFontPointSize
        text:               GeoZoneManager.sourceNote
    }

    QGCButton {
        text:       qsTr("Dismiss")
        onClicked:  GeoZoneManager.clearSourceNote()
    }
}
