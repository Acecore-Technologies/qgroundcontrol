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
import QGroundControl.ScreenTools

/// Suggestions for flying the legs of the mission around the restricted geozones they enter
QGCPopupDialog {
    id:         root
    title:      qsTr("Route Around Geozones")
    buttons:    Dialog.Close

    required property var missionController

    readonly property real _width: ScreenTools.defaultFontPixelWidth * 60

    GeoZoneRoutePlanner {
        id:                 planner
        missionController:  root.missionController
        active:             root.visible
    }

    ColumnLayout {
        spacing: ScreenTools.defaultFontPixelHeight / 2

        QGCLabel {
            Layout.preferredWidth:  root._width
            wrapMode:               Text.WordWrap
            text:                   planner.suggestions.length === 0 ?
                                        qsTr("No leg between waypoints enters a restricted geozone.") :
                                        qsTr("Detours keep %1 from the zones at the altitudes of the leg. Check the new waypoints, terrain and obstacles before flying.")
                                            .arg(Math.round(QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnits(planner.margin)) + " " +
                                                 QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString)
        }

        Repeater {
            model: planner.suggestions

            RowLayout {
                id:                     suggestionRow
                Layout.preferredWidth:  root._width
                spacing:                ScreenTools.defaultFontPixelWidth

                required property var   modelData
                required property int   index

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    textFormat:         Text.PlainText
                    text:               suggestionRow.modelData.text
                }

                QGCButton {
                    text:       qsTr("Insert")
                    visible:    suggestionRow.modelData.canApply
                    onClicked:  planner.applySuggestion(suggestionRow.index)
                }
            }
        }
    }
}
