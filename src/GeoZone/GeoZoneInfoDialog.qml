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

    onGeoZoneChanged: {
        if (!geoZone) {
            close()
        }
    }

    Connections {
        target: QGroundControl.geoZoneManager
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
    }
}
