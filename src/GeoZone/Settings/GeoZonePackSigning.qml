/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FactControls
import QGroundControl.Palette
import QGroundControl.ScreenTools
import QGroundControl.GeoZone

/// Signatures of offline packs: the key signing the packs exported, and the publishers whose packs are trusted
ColumnLayout {
    spacing:    _margin

    /// Selects or saves the key files, see the file dialog of GeoZoneSourcesPanel
    required property var dialog

    readonly property var   _geoZoneManager:    GeoZoneManager
    readonly property var   _geoZoneSettings:   GeoZoneManager.settings
    readonly property real  _margin:            ScreenTools.defaultFontPixelWidth / 2

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCButton {
        Layout.fillWidth:   true
        text:               qsTr("Pack Signatures...")
        checkable:          true
        checked:            packSignatures.visible
        onClicked:          packSignatures.visible = !packSignatures.visible
    }

    ColumnLayout {
        id:                 packSignatures
        Layout.fillWidth:   true
        spacing:            _margin
        visible:            false

        // Builds without OpenSSL
        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            color:              qgcPal.warningText
            text:               qsTr("This build of QGroundControl can't sign packs or check their signatures: packs are exported unsigned, and signed packs are imported as unsigned.")
            visible:            !_geoZoneManager.packSigningAvailable
        }

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            visible:            _geoZoneManager.packSigningAvailable
            text:               _geoZoneManager.packPublicKey === "" ?
                                    qsTr("Exported packs are not signed. Select or create an Ed25519 private key to sign them.") :
                                    qsTr("Exported packs are signed with the key %1. Give its public key to those importing them:").arg(_geoZoneManager.packKeyId)
        }

        QGCTextField {
            Layout.fillWidth:   true
            readOnly:           true
            text:               _geoZoneManager.packPublicKey
            visible:            text !== ""
        }

        GridLayout {
            Layout.fillWidth:   true
            columns:            2
            columnSpacing:      _margin
            rowSpacing:         _margin / 2
            enabled:            _geoZoneManager.packSigningAvailable

            QGCLabel { text: qsTr("Signing key") }
            RowLayout {
                Layout.fillWidth:   true
                spacing:            _margin

                FactTextField {
                    Layout.fillWidth:   true
                    fact:               _geoZoneSettings.packSigningKeyFile
                    placeholderText:    qsTr("Private key file (PEM)")
                }

                QGCButton {
                    text:       qsTr("Browse...")
                    onClicked:  dialog.openFor({ title: qsTr("Select Pack Signing Key"), nameFilters: [ qsTr("PEM Files (*.pem)"), qsTr("All Files (*)") ],
                                                     accept: file => _geoZoneSettings.packSigningKeyFile.rawValue = file })
                }

                QGCButton {
                    text:       qsTr("New Key...")
                    onClicked:  dialog.openFor({ title: qsTr("Save New Pack Signing Key"), nameFilters: [ qsTr("PEM Files (*.pem)") ],
                                                     defaultSuffix: "pem", save: true, accept: file => _geoZoneManager.createSigningKey(file) })
                }
            }

            QGCLabel { text: qsTr("Publisher") }
            FactTextField {
                Layout.fillWidth:   true
                fact:               _geoZoneSettings.packPublisherName
                placeholderText:    qsTr("Name given in the packs")
            }
        }

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            visible:            _geoZoneManager.packSigningAvailable || _geoZoneManager.trustedPublishers.length > 0
            text:               _geoZoneManager.trustedPublishers.length === 0 ?
                                    qsTr("No trusted publishers. Imported packs are marked unsigned, or untrusted when signed.") :
                                    qsTr("Packs signed by these publishers are marked as signed by them:")
        }

        Repeater {
            model: _geoZoneManager.trustedPublishers

            RowLayout {
                Layout.fillWidth:   true
                spacing:            _margin

                QGCLabel {
                    Layout.fillWidth:   true
                    elide:              Text.ElideRight
                    text:               qsTr("%1 (key %2)").arg(modelData.name).arg(modelData.keyId)
                    textFormat:         Text.PlainText
                }

                QGCButton {
                    text:       qsTr("Remove")
                    onClicked:  _geoZoneManager.distrustPublisher(modelData.publicKey)
                }
            }
        }

        RowLayout {
            Layout.fillWidth:   true
            spacing:            _margin
            enabled:            _geoZoneManager.packSigningAvailable

            QGCTextField {
                id:                 publisherNameField
                placeholderText:    qsTr("Name")
            }

            QGCTextField {
                id:                 publisherKeyField
                Layout.fillWidth:   true
                placeholderText:    qsTr("Public key (base64 or PEM)")
            }

            QGCButton {
                text:       qsTr("Trust")
                enabled:    publisherNameField.text.trim() !== "" && publisherKeyField.text.trim() !== ""
                onClicked: {
                    if (_geoZoneManager.trustPublisher(publisherNameField.text, publisherKeyField.text)) {
                        publisherNameField.text = ""
                        publisherKeyField.text = ""
                    }
                }
            }
        }
    }
}
