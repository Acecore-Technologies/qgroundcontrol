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

/// Details of a GeoZone loaded from an ED-269 / ED-318 file, with a choice of zones where several overlap.
/// Closed when the zones are reloaded or removed. Texts from the files are shown as plain text.
QGCPopupDialog {
    id:         root
    title:      qsTr("Geozone")
    buttons:    Dialog.Close

    required property var geoZone
    property var zones: []      ///< GeoZone objects to choose from, like those at a clicked point

    readonly property real _labelWidth: ScreenTools.defaultFontPixelWidth * 14
    readonly property real _valueWidth: ScreenTools.defaultFontPixelWidth * 45
    readonly property var  _authorisations: GeoZoneManager.authorisations

    property int _authorisationRevision: 0  ///< Re-reads the authorisation when the authorisations change
    readonly property var _authorisation: (_authorisationRevision, geoZone ? _authorisations.authorisationOf(geoZone) : ({}))
    readonly property bool _authorised: _authorisation.zoneKey !== undefined

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    Connections {
        target:             root._authorisations
        function onChanged() { root._authorisationRevision++ }
    }

    onGeoZoneChanged: {
        if (!geoZone) {
            close()
        }
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

        QGCLabel {
            Layout.preferredWidth:  infoRow.valueWidth
            text:                   infoRow.value
            textFormat:             Text.PlainText
            wrapMode:               Text.WordWrap
        }
    }

    ColumnLayout {
        spacing: ScreenTools.defaultFontPixelHeight / 4

        QGCComboBox {
            Layout.fillWidth:   true
            visible:            root.zones.length > 1
            model:              root.zones.map(zone => qsTr("%1 (%2)").arg(zone.displayName).arg(zone.restrictionText))
            currentIndex:       root.zones.findIndex(zone => zone.key === root.geoZone.key)
            onActivated:        (index) => root.geoZone = root.zones[index]
        }

        QGCLabel {
            Layout.preferredWidth:  root._labelWidth + root._valueWidth
            text:                   root.geoZone ? root.geoZone.displayName : ""
            textFormat:             Text.PlainText
            wrapMode:               Text.WordWrap
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

        InfoRow {
            label:      qsTr("Conditions")
            value:      root.geoZone ? root.geoZone.restrictionConditions : ""
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
            value:      root.geoZone ? root.geoZone.applicabilityText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Reason")
            value:      root.geoZone ? root.geoZone.reasonText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Message")
            value:      root.geoZone ? root.geoZone.message : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        InfoRow {
            label:      qsTr("Authority")
            value:      root.geoZone ? root.geoZone.authorityText : ""
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
            value:      root.geoZone ? root.geoZone.sourceText : ""
            labelWidth: root._labelWidth
            valueWidth: root._valueWidth
        }

        // The operator's authorisation stops the warnings and alerts about the zone while it is valid
        ColumnLayout {
            Layout.preferredWidth:  root._labelWidth + root._valueWidth
            spacing:                ScreenTools.defaultFontPixelHeight / 4
            visible:                root.geoZone && root.geoZone.restriction !== GeoZone.NoRestriction

            InfoRow {
                label:      qsTr("Authorisation")
                value:      !root._authorised ? "" : (root._authorisation.permit !== "" ? qsTr("%1, permit %2").arg(root._authorisation.statusText).arg(root._authorisation.permit) :
                                                                                         root._authorisation.statusText)
                labelWidth: root._labelWidth
                valueWidth: root._valueWidth
            }

            RowLayout {
                spacing:    ScreenTools.defaultFontPixelWidth
                visible:    !authorisationForm.visible

                QGCButton {
                    text:       root._authorised ? qsTr("Edit Authorisation...") : qsTr("Add Authorisation...")
                    onClicked: {
                        permitField.text = root._authorised ? root._authorisation.permit : ""
                        validFromField.text = root._authorised ? root._authorisation.validFrom : ""
                        validToField.text = root._authorised ? root._authorisation.validTo : ""
                        authorisationError.text = ""
                        authorisationForm.visible = true
                    }
                }

                QGCButton {
                    text:       qsTr("Remove Authorisation")
                    visible:    root._authorised
                    onClicked:  root._authorisations.revoke(root._authorisation.zoneKey)
                }
            }

            GridLayout {
                id:             authorisationForm
                columns:        2
                columnSpacing:  ScreenTools.defaultFontPixelWidth
                visible:        false

                QGCLabel { text: qsTr("Permit") }
                QGCTextField {
                    id:                     permitField
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 30
                    placeholderText:        qsTr("Number or reference")
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

                QGCLabel {
                    id:                     authorisationError
                    Layout.columnSpan:      2
                    Layout.preferredWidth:  root._labelWidth + root._valueWidth
                    wrapMode:               Text.WordWrap
                    color:                  qgcPal.warningText
                    visible:                text !== ""
                }

                QGCLabel {
                    Layout.columnSpan:      2
                    Layout.preferredWidth:  root._labelWidth + root._valueWidth
                    wrapMode:               Text.WordWrap
                    font.pointSize:         ScreenTools.smallFontPointSize
                    text:                   qsTr("While the authorisation is valid, the mission check, flight alerts and guided command checks leave out the zone. Plans entering the zone keep the authorisation.")
                }

                RowLayout {
                    Layout.columnSpan:  2
                    spacing:            ScreenTools.defaultFontPixelWidth

                    QGCButton {
                        text:       qsTr("Save")
                        onClicked: {
                            authorisationError.text = root._authorisations.authorise(root.geoZone, permitField.text, validFromField.text, validToField.text)
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
    }
}
