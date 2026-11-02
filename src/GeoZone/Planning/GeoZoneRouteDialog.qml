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

    // Narrower on small screens, so that the texts wrap rather than scroll sideways
    readonly property real _width: Math.min(ScreenTools.defaultFontPixelWidth * 64, mainWindow.width - ScreenTools.defaultFontPixelWidth * 8)


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
                                            .arg(GeoZoneStyle.horizontalText(planner.margin)).arg(GeoZoneStyle.verticalText(planner.verticalMargin))
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

        // One line per way around the zones, or per leg without one, with why the other ways don't work on demand
        Repeater {
            model: planner.suggestions

            ColumnLayout {
                id:                     suggestionColumn
                Layout.preferredWidth:  root._width
                Layout.topMargin:       (suggestionColumn.modelData.first && suggestionColumn.index > 0) ? ScreenTools.defaultFontPixelHeight / 2 : 0
                spacing:                ScreenTools.defaultFontPixelHeight / 4

                required property var   modelData
                required property int   index

                readonly property bool  _noRoute:       modelData.kind === "none"
                // The climb only needs a higher maximum altitude
                readonly property bool  _canAllowClimb: modelData.suggestedMaxAltitude !== undefined
                property bool           _showReasons:   false

                RowLayout {
                    Layout.fillWidth:   true
                    spacing:            ScreenTools.defaultFontPixelWidth

                    QGCLabel {
                        Layout.fillWidth:   true
                        wrapMode:           Text.WordWrap
                        textFormat:         Text.PlainText
                        text:               suggestionColumn.modelData.text
                        color:              suggestionColumn._noRoute ? qgcPal.warningText : qgcPal.text
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

                    QGCButton {
                        text:       qsTr("Allow Climb to %1").arg(GeoZoneStyle.verticalText(suggestionColumn.modelData.suggestedMaxAltitude))
                        visible:    suggestionColumn._noRoute && suggestionColumn._canAllowClimb
                        onClicked:  planner.maxAltitude = suggestionColumn.modelData.suggestedMaxAltitude
                    }

                    QGCButton {
                        text:       qsTr("Why") + (suggestionColumn._showReasons ? " \u25B4" : " \u25BE")
                        visible:    suggestionColumn.modelData.notes.length > 0
                        onClicked:  suggestionColumn._showReasons = !suggestionColumn._showReasons
                    }
                }

                Repeater {
                    model: suggestionColumn._showReasons ? suggestionColumn.modelData.notes : []

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

                QGCButton {
                    Layout.leftMargin:  ScreenTools.defaultFontPixelWidth * 2
                    text:               qsTr("Allow Climb to %1").arg(GeoZoneStyle.verticalText(suggestionColumn.modelData.suggestedMaxAltitude))
                    visible:            suggestionColumn._showReasons && !suggestionColumn._noRoute && suggestionColumn._canAllowClimb
                    onClicked:          planner.maxAltitude = suggestionColumn.modelData.suggestedMaxAltitude
                }
            }
        }
    }
}
