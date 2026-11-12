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

import QGroundControl.Controls
import QGroundControl.ScreenTools

/// Lists zones with why, like those a fence leaves out: each of them, in a list which scrolls, for the fence import
/// dialog and the GeoFence panel. With Ok and Cancel buttons it asks, and calls the accept function when accepted.
QGCPopupDialog {
    id: control

    property string intro
    property var    zones:          []      ///< Texts, shown as plain text
    property string note
    property var    acceptFunction: null

    readonly property real _listWidth: Math.min(ScreenTools.defaultFontPixelWidth * 56, mainWindow.width - ScreenTools.defaultFontPixelWidth * 8)

    onAccepted: {
        if (acceptFunction) {
            acceptFunction()
        }
    }

    ColumnLayout {
        spacing: ScreenTools.defaultFontPixelHeight / 2

        QGCLabel {
            Layout.preferredWidth:  control._listWidth
            wrapMode:               Text.WordWrap
            text:                   control.intro
            textFormat:             Text.PlainText
            visible:                text !== ""
        }

        QGCListView {
            Layout.preferredWidth:  control._listWidth
            Layout.preferredHeight: Math.min(contentHeight, ScreenTools.defaultFontPixelHeight * 12)
            clip:                   true
            model:                  control.zones

            delegate: QGCLabel {
                required property string modelData

                width:      ListView.view ? ListView.view.width : implicitWidth
                wrapMode:   Text.WordWrap
                textFormat: Text.PlainText
                text:       "\u2022 " + modelData
            }
        }

        QGCLabel {
            Layout.preferredWidth:  control._listWidth
            wrapMode:               Text.WordWrap
            text:                   control.note
            textFormat:             Text.PlainText
            visible:                text !== ""
        }
    }
}
