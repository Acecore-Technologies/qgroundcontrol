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
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools
import QGroundControl.GeoZone

/// The geozone sources with their details, turned on and off, and their secrets and publishers set. Each source has
/// a menu to refresh a feed, show the details of its data and remove it after a confirmation.
ColumnLayout {
    spacing:    _margin
    visible:    _geoZoneManager.sources.count > 0

    readonly property var   _geoZoneManager:    GeoZoneManager
    readonly property var   _repository:        GeoZoneManager.repository
    readonly property real  _margin:            ScreenTools.defaultFontPixelWidth / 2

    /// Removes the source with the key after a confirmation, it may have moved in the list meanwhile
    function _confirmRemove(key, name, zoneCount) {
        mainWindow.showMessageDialog(qsTr("Remove Geozone Source"),
                                     qsTr("Remove %1 and its %n zone(s)? Its stored copy is deleted.", "", zoneCount).arg(name.replace(/</g, "\u2039").replace(/>/g, "\u203A")),
                                     Dialog.Yes | Dialog.No,
                                     () => {
                                         for (let i = 0; i < _geoZoneManager.sources.count; i++) {
                                             if (_geoZoneManager.sources.get(i).key === key) {
                                                 _repository.removeSource(i)
                                                 return
                                             }
                                         }
                                     })
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    Repeater {
        model: _geoZoneManager.sources

        ColumnLayout {
            id:                 sourceColumn
            Layout.fillWidth:   true
            spacing:            _margin / 2

            property bool _expanded: false  ///< Shows the provenance, area and changed zones

            RowLayout {
                Layout.fillWidth:   true
                spacing:            _margin

                QGCCheckBox {
                    checked:    object.enabled
                    onClicked:  object.enabled = checked
                }

                QGCLabel {
                    Layout.fillWidth:   true
                    elide:              Text.ElideRight
                    text:               object.name
                    textFormat:         Text.PlainText
                }

                QGCButton {
                    id:                     menuButton
                    Layout.preferredWidth:  height
                    text:                   "\u22EF"
                    onClicked:              sourceMenu.popup(menuButton, menuButton.width - sourceMenu.width, menuButton.height)

                    QGCMenu {
                        id: sourceMenu

                        QGCMenuItem {
                            text:           qsTr("Refresh")
                            enabled:        object.url !== "" && !object.busy
                            onTriggered:    _repository.refreshSource(object.key)
                        }

                        QGCMenuItem {
                            text:           sourceColumn._expanded ? qsTr("Hide Details") : qsTr("Details")
                            onTriggered:    sourceColumn._expanded = !sourceColumn._expanded
                        }

                        QGCMenuItem {
                            text:           qsTr("Remove...")
                            onTriggered:    _confirmRemove(object.key, object.name, object.zoneCount)
                        }
                    }
                }
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               object.detailsText !== "" ? object.detailsText : (object.busy ? qsTr("Loading...") : qsTr("Not loaded"))
                textFormat:         Text.PlainText
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               object.provenanceText
                textFormat:         Text.PlainText
                visible:            sourceColumn._expanded && text !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               object.areaText
                visible:            sourceColumn._expanded && object.followsArea && text !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.warningText
                text:               qsTr("The last download did not match the SHA-256 its publisher gave and was not loaded.")
                visible:            object.integrityFailed
            }

            // A pack signed by a publisher the user may trust
            RowLayout {
                Layout.fillWidth:   true
                spacing:            _margin
                visible:            object.untrustedPack

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.smallFontPointSize
                    color:              qgcPal.warningText
                    text:               qsTr("The pack it was imported from is signed by a publisher who is not trusted.")
                }

                QGCButton {
                    text:       qsTr("Trust Publisher")
                    onClicked:  _geoZoneManager.trustSourcePublisher(object.key)
                }
            }

            // The secret of a service, like after importing a pack on another computer
            RowLayout {
                Layout.fillWidth:   true
                spacing:            _margin
                visible:            object.needsCredentials

                QGCLabel {
                    font.pointSize: ScreenTools.smallFontPointSize
                    color:          object.credentialsMissing ? qgcPal.warningText : qgcPal.text
                    text:           object.credentialsMissing ? qsTr("Credentials missing") : qsTr("Credentials")
                }

                QGCTextField {
                    id:                 sourceSecretField
                    Layout.fillWidth:   true
                    echoMode:           TextInput.Password
                    placeholderText:    object.credentialsMissing ? qsTr("Enter the secret") : qsTr("Replace the secret")
                }

                QGCButton {
                    text:       qsTr("Set")
                    enabled:    sourceSecretField.text !== ""
                    onClicked: {
                        _repository.setSecret(object.key, sourceSecretField.text)
                        sourceSecretField.text = ""
                    }
                }
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                font.italic:        true
                text:               qsTr("Turned off: its zones are not shown, checked or alerted.")
                visible:            !object.enabled
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                font.italic:        true
                text:               qsTr("Updating...")
                visible:            object.busy && object.detailsText !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.warningText
                text:               object.validityText
                visible:            text !== ""
            }

            // The zones the last reload added, changed and removed, their names on demand
            RowLayout {
                Layout.fillWidth:   true
                spacing:            _margin
                visible:            object.changesText !== ""

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text:               object.changesText
                    textFormat:         Text.PlainText
                }

                QGCButton {
                    text:       sourceColumn._expanded ? qsTr("Hide Details") : qsTr("Details")
                    onClicked:  sourceColumn._expanded = !sourceColumn._expanded
                }
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               object.changesDetails
                textFormat:         Text.PlainText
                visible:            sourceColumn._expanded && text !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.warningText
                text:               object.warningText
                visible:            text !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.warningText
                text:               qsTr("Last update failed: %1").arg(object.errorString)
                textFormat:         Text.PlainText
                visible:            object.errorString !== ""
            }
        }
    }
}
