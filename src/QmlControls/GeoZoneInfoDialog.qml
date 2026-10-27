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
import QtQuick.Dialogs
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.ScreenTools

/// Details of a GeoZone loaded from an ED-269 / ED-318 file
QGCPopupDialog {
    title:      geoZone.name !== "" ? geoZone.name : geoZone.identifier
    buttons:    Dialog.Close

    required property var geoZone

    readonly property real _labelWidth: ScreenTools.defaultFontPixelWidth * 14
    readonly property real _valueWidth: ScreenTools.defaultFontPixelWidth * 45

    component InfoRow: RowLayout {
        id:         infoRow
        spacing:    ScreenTools.defaultFontPixelWidth
        visible:    value !== ""

        property string label
        property string value

        QGCLabel {
            Layout.preferredWidth:  _labelWidth
            Layout.alignment:       Qt.AlignTop
            text:                   infoRow.label
        }

        QGCLabel {
            Layout.preferredWidth:  _valueWidth
            text:                   infoRow.value
            wrapMode:               Text.WordWrap
        }
    }

    ColumnLayout {
        spacing: ScreenTools.defaultFontPixelHeight / 4

        InfoRow {
            label:  qsTr("Restriction")
            value:  geoZone.restrictionText
        }

        InfoRow {
            label:  qsTr("Status")
            value:  geoZone.active ? qsTr("Active now") : qsTr("Not active now")
        }

        InfoRow {
            label:  qsTr("Conditions")
            value:  geoZone.restrictionConditions
        }

        InfoRow {
            label:  qsTr("Altitude")
            value:  geoZone.verticalLimitsText
        }

        InfoRow {
            label:  qsTr("Applicability")
            value:  geoZone.applicabilityText
        }

        InfoRow {
            label:  qsTr("Reason")
            value:  geoZone.reasonText
        }

        InfoRow {
            label:  qsTr("Message")
            value:  geoZone.message
        }

        InfoRow {
            label:  qsTr("Authority")
            value:  geoZone.authorityText
        }

        InfoRow {
            label:  qsTr("Identifier")
            value:  geoZone.country !== "" ? qsTr("%1 (%2)").arg(geoZone.identifier).arg(geoZone.country) : geoZone.identifier
        }
    }
}
