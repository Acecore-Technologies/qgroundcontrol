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
import QGroundControl.FlightMap
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Edits the zone the operator draws on the Plan view's map, see GeoZoneOperatorZones: the polygon or circle on the
/// map, with QGroundControl's polygon and circle editing like the fences, and a form of its name, restriction and
/// vertical limits.
Item {
    id: editor

    required property var map      ///< Map of the Plan view
    property real formLeftMargin:   ScreenTools.defaultFontPixelWidth   ///< Of the form, right of the tool strip
    property real formBottomMargin: ScreenTools.defaultFontPixelWidth   ///< Of the form, above the terrain profile
    property real formTopMargin:    ScreenTools.defaultFontPixelWidth   ///< Below which the form scrolls

    readonly property var   _zones:         GeoZoneManager.operatorZones
    readonly property color _color:         GeoZoneStyle.restrictionColor(restrictionCombo.currentIndex)
    readonly property real  _fieldWidth:    ScreenTools.defaultFontPixelWidth * 10

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    function _limitText(meters) {
        return isNaN(meters) ? "" : String(Math.round(meters * 10) / 10)
    }

    function _limitValue(text) {
        const trimmed = text.trim()
        return trimmed === "" ? NaN : Number(trimmed.replace(",", "."))
    }

    function _load() {
        const properties = _zones.editProperties
        nameField.text = properties.name
        restrictionCombo.currentIndex = Math.max(0, Math.min(3, properties.restriction))
        lowerField.text = _limitText(properties.lower)
        upperField.text = _limitText(properties.upper)
        lowerReferenceCombo.currentIndex = properties.lowerReference === "AMSL" ? 1 : 0
        upperReferenceCombo.currentIndex = properties.upperReference === "AMSL" ? 1 : 0
        messageField.text = properties.message
        errorLabel.text = ""
    }

    function _save() {
        const lower = _limitValue(lowerField.text)
        const upper = _limitValue(upperField.text)
        if ((lowerField.text.trim() !== "" && isNaN(lower)) || (upperField.text.trim() !== "" && isNaN(upper))) {
            errorLabel.text = qsTr("Enter the floor and ceiling in meters, or leave them empty.")
            return
        }
        if (!isNaN(lower) && !isNaN(upper) && lowerReferenceCombo.currentIndex === upperReferenceCombo.currentIndex && upper <= lower) {
            errorLabel.text = qsTr("The ceiling must be above the floor.")
            return
        }
        _zones.saveEdit({
            name:           nameField.text,
            restriction:    restrictionCombo.currentIndex,
            message:        messageField.text,
            lower:          lower,
            upper:          upper,
            lowerReference: lowerReferenceCombo.currentText,
            upperReference: upperReferenceCombo.currentText,
        })
    }

    Connections {
        target: editor._zones
        function onEditingChanged() {
            if (editor._zones.editing) {
                editor._load()
            }
        }
        function onFailed(message) { errorLabel.text = message }
    }

    QGCMapPolygonVisuals {
        mapControl:         editor.map
        mapPolygon:         editor._zones.polygon
        interactive:        editor._zones.editing && !editor._zones.editCircle
        visible:            interactive
        borderWidth:        2
        borderColor:        editor._color
        interiorColor:      editor._color
        interiorOpacity:    0.3
    }

    QGCMapCircleVisuals {
        mapControl:         editor.map
        mapCircle:          editor._zones.circle
        interactive:        editor._zones.editing && editor._zones.editCircle
        visible:            interactive
        borderWidth:        2
        borderColor:        editor._color
        interiorColor:      editor._color
        interiorOpacity:    0.3
        radiusLabelVisible: true
    }

    // The form, whose fields scroll on small screens above the buttons
    Rectangle {
        id:                     form
        anchors.left:           parent.left
        anchors.bottom:         parent.bottom
        anchors.leftMargin:     editor.formLeftMargin
        anchors.bottomMargin:   editor.formBottomMargin
        width:                  Math.min(formColumn.implicitWidth + _padding * 2, editor.width - editor.formLeftMargin - ScreenTools.defaultFontPixelWidth)
        height:                 Math.min(formColumn.implicitHeight + footer.implicitHeight + footer.spacing + _padding * 2,
                                         editor.height - editor.formBottomMargin - editor.formTopMargin)
        visible:                editor._zones.editing
        color:                  qgcPal.window
        opacity:                0.95
        radius:                 ScreenTools.defaultFontPixelWidth / 2

        readonly property real _padding: ScreenTools.defaultFontPixelWidth

        // Clicks on the form don't reach the map
        MouseArea {
            anchors.fill:   parent
            onWheel:        (wheel) => wheel.accepted = true
        }

        ColumnLayout {
            id:                 footer
            anchors.fill:       parent
            anchors.margins:    form._padding
            spacing:            ScreenTools.defaultFontPixelHeight / 4

            QGCFlickable {
                Layout.fillWidth:   true
                Layout.fillHeight:  true
                contentHeight:      formColumn.implicitHeight
                contentWidth:       width
                clip:               true

                ColumnLayout {
                    id:         formColumn
                    width:      parent.width
                    spacing:    ScreenTools.defaultFontPixelHeight / 4

                    QGCLabel {
                        text:       editor._zones.editIndex < 0 ? qsTr("New zone") : qsTr("Edit zone")
                        font.bold:  true
                    }

                    QGCLabel {
                        Layout.fillWidth:       true
                        Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 40
                        wrapMode:               Text.WordWrap
                        font.pointSize:         ScreenTools.smallFontPointSize
                        text:                   editor._zones.editCircle ? qsTr("Drag the center or the edge of the circle.") :
                                                                           qsTr("Drag the corners, or the + between them to add corners.")
                    }

                    GridLayout {
                        columns:        3
                        rowSpacing:     ScreenTools.defaultFontPixelHeight / 4
                        columnSpacing:  ScreenTools.defaultFontPixelWidth

                        QGCLabel { text: qsTr("Name") }
                        QGCTextField {
                            id:                     nameField
                            Layout.columnSpan:      2
                            Layout.fillWidth:       true
                            Layout.preferredWidth:  editor._fieldWidth * 2
                        }

                        QGCLabel { text: qsTr("Restriction") }
                        QGCComboBox {
                            id:                 restrictionCombo
                            Layout.columnSpan:  2
                            Layout.fillWidth:   true
                            model:              editor._zones.restrictionNames
                        }

                        QGCLabel { text: qsTr("Floor (m)") }
                        QGCTextField {
                            id:                     lowerField
                            Layout.preferredWidth:  editor._fieldWidth
                            placeholderText:        qsTr("Ground")
                            inputMethodHints:       Qt.ImhFormattedNumbersOnly
                        }
                        QGCComboBox {
                            id:     lowerReferenceCombo
                            model:  [ "AGL", "AMSL" ]
                        }

                        QGCLabel { text: qsTr("Ceiling (m)") }
                        QGCTextField {
                            id:                     upperField
                            Layout.preferredWidth:  editor._fieldWidth
                            placeholderText:        qsTr("Unlimited")
                            inputMethodHints:       Qt.ImhFormattedNumbersOnly
                        }
                        QGCComboBox {
                            id:     upperReferenceCombo
                            model:  [ "AGL", "AMSL" ]
                        }

                        QGCLabel { text: qsTr("Message") }
                        QGCTextField {
                            id:                 messageField
                            Layout.columnSpan:  2
                            Layout.fillWidth:   true
                            placeholderText:    qsTr("Optional")
                        }
                    }
                }
            }

            QGCLabel {
                id:                     errorLabel
                Layout.fillWidth:       true
                Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 40
                wrapMode:               Text.WordWrap
                color:                  qgcPal.warningText
                visible:                text !== ""
            }

            RowLayout {
                Layout.fillWidth: true

                QGCButton {
                    Layout.fillWidth:   true
                    text:               qsTr("Save")
                    onClicked:          editor._save()
                }

                QGCButton {
                    Layout.fillWidth:   true
                    text:               qsTr("Cancel")
                    onClicked:          editor._zones.cancelEdit()
                }
            }
        }
    }
}
