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

/// Details of a GeoZone loaded from an ED-269 / ED-318 file, with the zones to choose from as chips where several
/// overlap, and the operator's authorisation for it, added and edited in place.
/// Closed when the zones are reloaded or removed. Texts from the files are shown as plain text.
QGCPopupDialog {
    id:         root
    title:      qsTr("Geozone")
    buttons:    Dialog.Close

    required property var geoZone
    property var zones: []      ///< GeoZone objects to choose from, like those at a clicked point

    // Narrower on small windows, so long texts wrap rather than scroll sideways
    readonly property real _contentWidth:   Math.min(ScreenTools.defaultFontPixelWidth * 60, mainWindow.width - ScreenTools.defaultFontPixelWidth * 8)
    readonly property real _labelWidth:     ScreenTools.defaultFontPixelWidth * 14
    readonly property real _valueWidth:     _contentWidth - _labelWidth - ScreenTools.defaultFontPixelWidth
    readonly property var  _authorisations: GeoZoneManager.planning.authorisations

    property int _authorisationRevision: 0  ///< Re-reads the authorisation when the authorisations change
    readonly property var _authorisation: (_authorisationRevision, geoZone ? _authorisations.authorisationOf(geoZone) : ({}))
    readonly property bool _authorised: _authorisation.zoneKey !== undefined
    readonly property var _notification: (_authorisationRevision, geoZone ? _authorisations.notificationOf(geoZone) : ({}))
    readonly property bool _notified: _notification.zoneKey !== undefined
    readonly property string _notificationRequirement: geoZone ? _authorisations.notificationRequirement(geoZone) : ""
    property bool _notificationForm: false  ///< The form records a notification of the zone's authority rather than an authorisation

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    Connections {
        target:             root._authorisations
        function onChanged() { root._authorisationRevision++ }
    }

    onGeoZoneChanged: {
        if (!geoZone) {
            close()
        }
        // The form was for the zone shown before
        authorisationForm.visible = false
    }

    Connections {
        target: GeoZoneManager
        function onZonesAboutToChange(source) {
            if ([ root.geoZone ].concat(root.zones).some(zone => !zone || !zone.valid || zone.sourceKey === source.key)) {
                root.close()
            }
        }
    }

    component InfoRow: RowLayout {
        id:         infoRow
        spacing:    ScreenTools.defaultFontPixelWidth
        visible:    value !== ""

        required property string label
        required property string value
        required property real labelWidth
        required property real valueWidth

        QGCLabel {
            Layout.preferredWidth:  infoRow.labelWidth
            Layout.alignment:       Qt.AlignTop
            text:                   infoRow.label
        }

        // Addresses and identifiers without spaces are broken anywhere
        QGCLabel {
            Layout.preferredWidth:  infoRow.valueWidth
            text:                   infoRow.value
            textFormat:             Text.PlainText
            wrapMode:               Text.WrapAtWordBoundaryOrAnywhere
        }
    }

    ColumnLayout {
        spacing: ScreenTools.defaultFontPixelHeight / 4

        // The overlapping zones side by side, the one shown highlighted
        QGCLabel {
            text:       qsTr("%n zone(s) here", "", root.zones.length)
            visible:    root.zones.length > 1
        }

        Flow {
            Layout.preferredWidth:  root._contentWidth
            Layout.bottomMargin:    ScreenTools.defaultFontPixelHeight / 2
            spacing:                ScreenTools.defaultFontPixelWidth / 2
            visible:                root.zones.length > 1

            Repeater {
                model: root.zones

                Rectangle {
                    id:             chip
                    width:          chipRow.width + (ScreenTools.defaultFontPixelWidth * 2)
                    height:         ScreenTools.implicitButtonHeight
                    radius:         height / 2
                    color:          _selected ? qgcPal.buttonHighlight : qgcPal.button
                    border.color:   GeoZoneStyle.restrictionColor(modelData.restriction)
                    border.width:   _selected ? 2 : 1

                    required property var modelData

                    readonly property bool _selected: root.geoZone && modelData.key === root.geoZone.key

                    Row {
                        id:                     chipRow
                        anchors.centerIn:       parent
                        spacing:                ScreenTools.defaultFontPixelWidth / 2

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width:                  ScreenTools.defaultFontPixelHeight * 0.6
                            height:                 width
                            radius:                 width / 2
                            color:                  GeoZoneStyle.restrictionColor(chip.modelData.restriction)
                        }

                        QGCLabel {
                            anchors.verticalCenter: parent.verticalCenter
                            width:                  Math.min(implicitWidth, root._contentWidth - (ScreenTools.defaultFontPixelWidth * 4) - (ScreenTools.defaultFontPixelHeight * 0.6))
                            text:                   chip.modelData.displayName
                            textFormat:             Text.PlainText
                            elide:                  Text.ElideRight
                            color:                  chip._selected ? qgcPal.buttonHighlightText : qgcPal.buttonText
                        }
                    }

                    QGCMouseArea {
                        anchors.fill:   parent
                        onClicked:      root.geoZone = chip.modelData
                    }
                }
            }
        }

        QGCLabel {
            Layout.preferredWidth:  root._contentWidth
            text:                   root.geoZone ? root.geoZone.displayName : ""
            textFormat:             Text.PlainText
            wrapMode:               Text.WrapAtWordBoundaryOrAnywhere
            font.bold:              true
        }

        InfoRow {
            label:      qsTr("Restriction")
            value:      root.geoZone ? root.geoZone.restrictionText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Status")
            value:      root.geoZone ? (root.geoZone.active ? qsTr("Active now") : qsTr("Not active now")) : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        // The operator's authorisation stops the warnings and alerts about the zone while it is valid. Added and edited in
        // place, near the top so that the form shows without scrolling.
        ColumnLayout {
            Layout.preferredWidth:  root._contentWidth
            spacing:                ScreenTools.defaultFontPixelHeight / 4
            visible:                root.geoZone && (root.geoZone.restriction !== GeoZone.NoRestriction || root._notificationRequirement !== "")

            InfoRow {
                label:      qsTr("Authorisation")
                value:      !root._authorised ? "" : (root._authorisation.permit !== "" ? qsTr("%1, permit %2").arg(root._authorisation.statusText).arg(root._authorisation.permit) :
                                                                                         root._authorisation.statusText)
                labelWidth: root._labelWidth
                valueWidth: root._valueWidth
            }

            // The notice the zone's authority needs, until the notification is recorded
            InfoRow {
                label:      qsTr("Notification")
                value:      !root._notified ? root._notificationRequirement :
                                              (root._notification.permit !== "" ? qsTr("%1, reference %2").arg(root._notification.statusText).arg(root._notification.permit) :
                                                                                  root._notification.statusText)
                labelWidth: root._labelWidth
                valueWidth: root._valueWidth
            }

            Flow {
                Layout.preferredWidth:  root._contentWidth
                spacing:                ScreenTools.defaultFontPixelWidth
                visible:                !authorisationForm.visible

                QGCButton {
                    text:       root._authorised ? qsTr("Edit Authorisation") : qsTr("Add Authorisation")
                    visible:    root.geoZone && root.geoZone.restriction !== GeoZone.NoRestriction
                    onClicked: {
                        root._notificationForm = false
                        permitField.text = root._authorised ? root._authorisation.permit : ""
                        validFromField.text = root._authorised ? root._authorisation.validFrom : ""
                        validToField.text = root._authorised ? root._authorisation.validTo : ""
                        ceilingField.text = root._authorised ? root._authorisation.ceiling : ""
                        dailyFromField.text = root._authorised ? root._authorisation.dailyFrom : ""
                        dailyToField.text = root._authorised ? root._authorisation.dailyTo : ""
                        aircraftField.text = root._authorised ? root._authorisation.aircraft : ""
                        missionAreaCheckBox.checked = root._authorised ? root._authorisation.limitToMission : false
                        authorisationError.text = ""
                        authorisationForm.visible = true
                    }
                }

                QGCButton {
                    text:       qsTr("Remove Authorisation")
                    visible:    root._authorised
                    onClicked:  root._authorisations.revoke(root._authorisation.zoneKey)
                }

                QGCButton {
                    text:       root._notified ? qsTr("Edit Notification...") : qsTr("Add Notification...")
                    visible:    root._notified || root._notificationRequirement !== ""
                    onClicked: {
                        root._notificationForm = true
                        permitField.text = root._notified ? root._notification.permit : ""
                        validFromField.text = root._notified ? root._notification.validFrom : ""
                        validToField.text = root._notified ? root._notification.validTo : ""
                        authorisationError.text = ""
                        authorisationForm.visible = true
                    }
                }

                QGCButton {
                    text:       qsTr("Remove Notification")
                    visible:    root._notified
                    onClicked:  root._authorisations.revoke(root._notification.zoneKey, true)
                }
            }

            GridLayout {
                id:             authorisationForm
                columns:        2
                columnSpacing:  ScreenTools.defaultFontPixelWidth
                visible:        false

                QGCLabel { text: root._notificationForm ? qsTr("Reference") : qsTr("Permit") }
                QGCTextField {
                    id:                     permitField
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 30
                    placeholderText:        root._notificationForm ? qsTr("Optional") : qsTr("Number or reference")
                }

                QGCLabel { text: qsTr("First day") }
                QGCTextField {
                    id:                     validFromField
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 30
                    placeholderText:        qsTr("yyyy-MM-dd, optional")
                }

                QGCLabel { text: qsTr("Last day") }
                QGCTextField {
                    id:                     validToField
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 30
                    placeholderText:        qsTr("yyyy-MM-dd, optional")
                }

                // The scope, unlimited when empty
                QGCLabel {
                    text:       qsTr("Ceiling (%1)").arg(root.geoZone ? root._authorisations.ceilingUnits(root.geoZone) : "")
                    visible:    !root._notificationForm
                }
                QGCTextField {
                    id:                     ceilingField
                    visible:                !root._notificationForm
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 30
                    placeholderText:        qsTr("Optional")
                    inputMethodHints:       Qt.ImhFormattedNumbersOnly
                }

                QGCLabel {
                    text:       qsTr("Daily hours")
                    visible:    !root._notificationForm
                }
                RowLayout {
                    spacing:    ScreenTools.defaultFontPixelWidth / 2
                    visible:    !root._notificationForm

                    QGCTextField {
                        id:                     dailyFromField
                        Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 14
                        placeholderText:        qsTr("08:00")
                    }
                    QGCLabel { text: "–" }
                    QGCTextField {
                        id:                     dailyToField
                        Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 14
                        placeholderText:        qsTr("18:00")
                    }
                }

                QGCLabel {
                    text:       qsTr("Aircraft")
                    visible:    !root._notificationForm
                }
                QGCTextField {
                    id:                     aircraftField
                    visible:                !root._notificationForm
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 30
                    placeholderText:        qsTr("Registration, optional")
                }

                QGCCheckBox {
                    id:                 missionAreaCheckBox
                    Layout.columnSpan:  2
                    text:               qsTr("Only within the mission area")
                    visible:            !root._notificationForm &&
                                        (GeoZoneManager.planning.missionArea.isValid || (root._authorised && root._authorisation.limitToMission))
                }

                QGCLabel {
                    id:                     authorisationError
                    Layout.columnSpan:      2
                    Layout.preferredWidth:  root._contentWidth
                    wrapMode:               Text.WordWrap
                    color:                  qgcPal.warningText
                    visible:                text !== ""
                }

                QGCLabel {
                    Layout.columnSpan:      2
                    Layout.preferredWidth:  root._contentWidth
                    wrapMode:               Text.WordWrap
                    font.pointSize:         ScreenTools.smallFontPointSize
                    text:                   root._notificationForm ?
                                                qsTr("While the notification is valid, the mission check no longer asks to notify the zone's authority. It doesn't authorise the flight.") :
                                                qsTr("While the authorisation is valid and the flight stays within the ceiling, area and daily hours entered, the mission check, flight alerts and guided command checks leave out the zone. Plans entering the zone keep the authorisation.")
                }

                RowLayout {
                    Layout.columnSpan:  2
                    spacing:            ScreenTools.defaultFontPixelWidth

                    QGCButton {
                        text:       qsTr("Save")
                        onClicked: {
                            authorisationError.text = root._notificationForm ?
                                root._authorisations.recordNotification(root.geoZone, permitField.text, validFromField.text, validToField.text) :
                                root._authorisations.authorise(root.geoZone, permitField.text, validFromField.text, validToField.text, {
                                ceiling:        ceilingField.text,
                                dailyFrom:      dailyFromField.text,
                                dailyTo:        dailyToField.text,
                                aircraft:       aircraftField.text,
                                limitToMission: missionAreaCheckBox.visible && missionAreaCheckBox.checked,
                                missionArea:    GeoZoneManager.planning.missionArea
                            })
                            authorisationForm.visible = authorisationError.text !== ""
                        }
                    }

                    QGCButton {
                        text:       qsTr("Cancel")
                        onClicked:  authorisationForm.visible = false
                    }
                }
            }
        }

        InfoRow {
            label:      qsTr("Conditions")
            value:      root.geoZone ? root.geoZone.details.restrictionConditions : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Altitude")
            value:      root.geoZone ? root.geoZone.verticalLimitsText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Applicability")
            value:      root.geoZone ? root.geoZone.details.applicabilityText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Reason")
            value:      root.geoZone ? root.geoZone.details.reasonText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Message")
            value:      root.geoZone ? root.geoZone.details.message : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Authority")
            value:      root.geoZone ? root.geoZone.details.authorityText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Exemptions")
            value:      root.geoZone ? root.geoZone.details.regulationExemptionText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("U-space class")
            value:      root.geoZone ? root.geoZone.details.uSpaceClass : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Details")
            value:      root.geoZone ? root.geoZone.details.extendedPropertiesText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Data issues")
            value:      root.geoZone ? root.geoZone.details.issuesText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Identifier")
            value:      !root.geoZone ? "" : (root.geoZone.country !== "" ? qsTr("%1 (%2)").arg(root.geoZone.identifier).arg(root.geoZone.country) : root.geoZone.identifier)
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Source")
            value:      root.geoZone ? root.geoZone.details.sourceText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }
    }
}
