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

import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Combo box whose long texts wrap in the list and are elided in the box. Some choices can be grayed out with
/// disabledText, like the feeds already added; the model is kept when they change, so the current choice stays.
QGCComboBox {
    id: control

    property var    choicesEnabled: []      ///< false for the choices which can't be chosen, missing ones can
    property string disabledText:   "%1"    ///< Text of the choices which can't be chosen, %1 being the model's text

    readonly property bool currentEnabled: choiceEnabled(currentIndex)  ///< The current choice can be chosen

    /// @return true unless choicesEnabled has false for the choice
    function choiceEnabled(index) {
        return !(index >= 0 && index < choicesEnabled.length && choicesEnabled[index] === false)
    }

    /// Selects the first choice which can be chosen when the current one can't
    function selectEnabledChoice() {
        if (currentEnabled) {
            return
        }
        for (let i = 0; i < count; i++) {
            if (choiceEnabled(i)) {
                currentIndex = i
                return
            }
        }
    }

    function _text(index, text) {
        return choiceEnabled(index) ? text : disabledText.arg(text)
    }

    alternateText: currentIndex >= 0 && !currentEnabled ? _text(currentIndex, currentText) : ""

    // The texts may come from feed catalogues: no markup, in the box and in the list
    Component.onCompleted: {
        contentItem.elide = Text.ElideRight
        contentItem.textFormat = Text.PlainText
    }

    QGCPalette { id: choicePalette; colorGroupEnabled: true }

    delegate: ItemDelegate {
        id:             choice
        width:          control._popupWidth
        enabled:        control.choiceEnabled(index)
        highlighted:    control.highlightedIndex === index
        topPadding:     ScreenTools.defaultFontPixelHeight / 2
        bottomPadding:  topPadding

        required property int   index
        required property var   modelData

        contentItem: Text {
            text:               control._text(choice.index, choice.modelData)
            textFormat:         Text.PlainText
            font:               control.font
            wrapMode:           Text.WordWrap
            verticalAlignment:  Text.AlignVCenter
            color:              control.currentIndex === choice.index ? choicePalette.buttonHighlightText : choicePalette.buttonText
            opacity:            choice.enabled ? 1 : 0.5
        }

        background: Rectangle {
            color: control.currentIndex === choice.index ? choicePalette.buttonHighlight : choicePalette.button
        }
    }
}
