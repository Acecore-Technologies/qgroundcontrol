/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick

import QGroundControl.Controls
import QGroundControl.ScreenTools

/// Pill shaped badge with a short bold text, like the restriction letter of a zone or a count. Short texts make
/// round badges.
Rectangle {
    id: root

    property alias  text:           label.text
    property color  textColor:      "black"
    property real   pointSize:      ScreenTools.smallFontPointSize
    property real   minimumWidth:   implicitHeight

    implicitHeight: Math.round(label.contentHeight + ScreenTools.defaultFontPixelHeight * 0.15)
    implicitWidth:  Math.max(minimumWidth, Math.round(label.contentWidth + ScreenTools.defaultFontPixelWidth))
    radius:         height / 2
    color:          "gray"

    Accessible.role:    Accessible.StaticText
    Accessible.name:    label.text

    QGCLabel {
        id:                 label
        anchors.centerIn:   parent
        color:              root.textColor
        font.pointSize:     root.pointSize
        font.bold:          true
        textFormat:         Text.PlainText
    }
}
