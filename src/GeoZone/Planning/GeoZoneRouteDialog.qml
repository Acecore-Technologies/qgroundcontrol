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
import QtQuick.Dialogs
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Ways for the legs of the mission to go around, over or under the restricted geozones they enter
QGCPopupDialog {
    id:         root
    title:      qsTr("Route Around Geozones")
    buttons:    Dialog.Close

    required property var missionController

    readonly property real _width: ScreenTools.defaultFontPixelWidth * 64

    function _horizontalText(meters) {
        return Math.round(QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnits(meters)) + " " +
               QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString
    }

    function _verticalText(meters) {
        return Math.round(QGroundControl.unitsConversion.metersToAppSettingsVerticalDistanceUnits(meters)) + " " +
               QGroundControl.unitsConversion.appSettingsVerticalDistanceUnitsString
    }

    GeoZoneRoutePlanner {
        id:                 planner
        missionController:  root.missionController
        active:             root.visible
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    ColumnLayout {
        spacing: ScreenTools.defaultFontPixelHeight / 2

        QGCLabel {
            Layout.preferredWidth:  root._width
            wrapMode:               Text.WordWrap
            visible:                !planner.busy && (planner.statusText === "")
            text:                   planner.suggestions.length === 0 ?
                                        qsTr("No leg between waypoints enters a restricted geozone.") :
                                        qsTr("Detours keep %1 from the zones at the altitudes of the leg, climbs and descents keep %2 from their limits. The cheapest way of each leg is recommended. Check the new waypoints, altitudes, terrain and obstacles before flying.")
                                            .arg(root._horizontalText(planner.margin)).arg(root._verticalText(planner.verticalMargin))
        }

        RowLayout {
            Layout.preferredWidth:  root._width
            spacing:                ScreenTools.defaultFontPixelWidth

            QGCLabel { text: qsTr("Climb at most to") }

            QGCTextField {
                id:                     maxAltitudeField
                Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 8
                text:                   Math.round(QGroundControl.unitsConversion.metersToAppSettingsVerticalDistanceUnits(planner.maxAltitude))
                inputMethodHints:       Qt.ImhDigitsOnly
                validator:              IntValidator { bottom: 1; top: 100000 }
                onEditingFinished:      planner.maxAltitude = QGroundControl.unitsConversion.appSettingsVerticalDistanceUnitsToMeters(parseInt(text))
            }

            QGCLabel {
                Layout.fillWidth:   true
                text:               qsTr("%1 above the terrain").arg(QGroundControl.unitsConversion.appSettingsVerticalDistanceUnitsString)
            }
        }

        RowLayout {
            Layout.preferredWidth:  root._width
            spacing:                ScreenTools.defaultFontPixelWidth
            visible:                planner.busy

            QGCLabel { text: qsTr("Searching routes") }

            ProgressBar {
                Layout.fillWidth:   true
                from:               0
                to:                 1
                value:              planner.progress
            }

            QGCButton {
                text:       qsTr("Cancel")
                onClicked:  planner.cancel()
            }
        }

        RowLayout {
            Layout.preferredWidth:  root._width
            spacing:                ScreenTools.defaultFontPixelWidth
            visible:                !planner.busy && (planner.statusText !== "")

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                text:               planner.statusText
            }

            QGCButton {
                text:       qsTr("Search Again")
                onClicked:  planner.startUpdate()
            }
        }

        Repeater {
            model: planner.suggestions

            ColumnLayout {
                id:                     suggestionColumn
                Layout.preferredWidth:  root._width
                spacing:                ScreenTools.defaultFontPixelHeight / 4

                required property var   modelData
                required property int   index

                QGCLabel {
                    Layout.fillWidth:   true
                    Layout.topMargin:   suggestionColumn.index > 0 ? ScreenTools.defaultFontPixelHeight / 2 : 0
                    visible:            suggestionColumn.modelData.first
                    font.bold:          true
                    text:               suggestionColumn.modelData.legText
                }

                RowLayout {
                    Layout.fillWidth:   true
                    spacing:            ScreenTools.defaultFontPixelWidth

                    QGCLabel {
                        Layout.fillWidth:   true
                        wrapMode:           Text.WordWrap
                        textFormat:         Text.PlainText
                        text:               suggestionColumn.modelData.text
                        color:              suggestionColumn.modelData.kind === "none" ? qgcPal.warningText : qgcPal.text
                    }

                    QGCLabel {
                        visible:    suggestionColumn.modelData.recommended
                        text:       qsTr("Recommended")
                        font.bold:  true
                    }

                    QGCButton {
                        text:       suggestionColumn.modelData.applyText
                        visible:    suggestionColumn.modelData.canApply
                        onClicked:  planner.applySuggestion(suggestionColumn.index)
                    }
                }

                Repeater {
                    model: suggestionColumn.modelData.notes

                    QGCLabel {
                        required property string modelData

                        Layout.fillWidth:   true
                        Layout.leftMargin:  ScreenTools.defaultFontPixelWidth * 2
                        wrapMode:           Text.WordWrap
                        textFormat:         Text.PlainText
                        font.pointSize:     ScreenTools.smallFontPointSize
                        text:               modelData
                    }
                }
            }
        }
    }
}
