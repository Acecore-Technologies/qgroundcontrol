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
import QtQuick.Layouts

import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools
import QGroundControl.GeoZone

/// The geozone sources with their details, turned on and off, and their secrets and publishers set. Each source has
/// a menu to refresh a feed, show the details of its data and remove it after a confirmation.
ColumnLayout {
    id:         root
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
                                         for (let i = 0; i < root._geoZoneManager.sources.count; i++) {
                                             if (root._geoZoneManager.sources.get(i).key === key) {
                                                 root._repository.removeSource(i)
                                                 return
                                             }
                                         }
                                     })
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    Repeater {
        model: root._geoZoneManager.sources

        ColumnLayout {
            id:                 sourceColumn
            Layout.fillWidth:   true
            spacing:            root._margin / 2

            required property var object    ///< GeoZoneSource

            property bool _expanded: false  ///< Shows the provenance, area and changed zones

            RowLayout {
                Layout.fillWidth:   true
                spacing:            root._margin

                QGCCheckBox {
                    checked:    sourceColumn.object.enabled
                    onClicked:  sourceColumn.object.enabled = checked
                }

                QGCLabel {
                    Layout.fillWidth:   true
                    elide:              Text.ElideRight
                    text:               sourceColumn.object.name
                    textFormat:         Text.PlainText
                }

                // Outdated data treated as invalid, whose zones still apply
                Rectangle {
                    Layout.preferredWidth:  expiredLabel.implicitWidth + ScreenTools.defaultFontPixelWidth
                    Layout.preferredHeight: expiredLabel.implicitHeight + (ScreenTools.defaultFontPixelHeight / 4)
                    radius:                 height / 4
                    color:                  qgcPal.colorRed
                    visible:                sourceColumn.object.invalid && sourceColumn.object.enabled

                    QGCLabel {
                        id:                 expiredLabel
                        anchors.centerIn:   parent
                        text:               qsTr("Expired")
                        color:              "white"
                        font.bold:          true
                    }
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
                            enabled:        sourceColumn.object.url !== "" && !sourceColumn.object.busy
                            onTriggered:    root._repository.refreshSource(sourceColumn.object.key)
                        }

                        QGCMenuItem {
                            text:           sourceColumn._expanded ? qsTr("Hide Details") : qsTr("Details")
                            onTriggered:    sourceColumn._expanded = !sourceColumn._expanded
                        }

                        QGCMenuItem {
                            text:           qsTr("Remove...")
                            onTriggered:    root._confirmRemove(sourceColumn.object.key, sourceColumn.object.name, sourceColumn.object.zoneCount)
                        }
                    }
                }
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               sourceColumn.object.detailsText !== "" ? sourceColumn.object.detailsText + (sourceColumn.object.nextCheckText !== "" ? ", " + sourceColumn.object.nextCheckText : "")
                                                              : (sourceColumn.object.busy ? qsTr("Loading...") : qsTr("Not loaded"))
                textFormat:         Text.PlainText
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               sourceColumn.object.provenanceText
                textFormat:         Text.PlainText
                visible:            sourceColumn._expanded && text !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               sourceColumn.object.areaText
                visible:            sourceColumn._expanded && sourceColumn.object.followsArea && text !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.warningText
                text:               qsTr("The last download did not match the SHA-256 its publisher gave and was not loaded.")
                visible:            sourceColumn.object.integrityFailed
            }

            // A pack signed by a publisher the user may trust
            RowLayout {
                Layout.fillWidth:   true
                spacing:            root._margin
                visible:            sourceColumn.object.untrustedPack

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.smallFontPointSize
                    color:              qgcPal.warningText
                    text:               qsTr("The pack it was imported from is signed by a publisher who is not trusted.")
                }

                QGCButton {
                    text:       qsTr("Trust Publisher")
                    onClicked:  root._geoZoneManager.trustSourcePublisher(sourceColumn.object.key)
                }
            }

            // The secret of a service, like after importing a pack on another computer
            RowLayout {
                Layout.fillWidth:   true
                spacing:            root._margin
                visible:            sourceColumn.object.needsCredentials

                QGCLabel {
                    font.pointSize: ScreenTools.smallFontPointSize
                    color:          sourceColumn.object.credentialsMissing ? qgcPal.warningText : qgcPal.text
                    text:           sourceColumn.object.credentialsMissing ? qsTr("Credentials missing") : qsTr("Credentials")
                }

                QGCTextField {
                    id:                 sourceSecretField
                    Layout.fillWidth:   true
                    echoMode:           TextInput.Password
                    placeholderText:    sourceColumn.object.credentialsMissing ? qsTr("Enter the secret") : qsTr("Replace the secret")
                }

                QGCButton {
                    text:       qsTr("Set")
                    enabled:    sourceSecretField.text !== ""
                    onClicked: {
                        root._repository.setSecret(sourceColumn.object.key, sourceSecretField.text)
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
                visible:            !sourceColumn.object.enabled
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                font.italic:        true
                text:               qsTr("Updating...")
                visible:            sourceColumn.object.busy && sourceColumn.object.detailsText !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              sourceColumn.object.invalid ? qgcPal.colorRed : qgcPal.warningText
                font.bold:          sourceColumn.object.invalid
                text:               sourceColumn.object.invalid ? qsTr("%1, treated as invalid: uploads ask for confirmation, its zones still apply. Refresh or replace it.").arg(sourceColumn.object.validityText) :
                                                     sourceColumn.object.validityText
                visible:            sourceColumn.object.validityText !== ""
            }

            // The zones the last reload added, changed and removed, their names on demand
            RowLayout {
                Layout.fillWidth:   true
                spacing:            root._margin
                visible:            sourceColumn.object.changesText !== ""

                QGCLabel {
                    Layout.fillWidth:   true
                    wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text:               sourceColumn.object.changesText
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
                text:               sourceColumn.object.changesDetails
                textFormat:         Text.PlainText
                visible:            sourceColumn._expanded && text !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.warningText
                text:               sourceColumn.object.warningText
                visible:            text !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WrapAtWordBoundaryOrAnywhere
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.warningText
                text:               qsTr("Last update failed: %1").arg(sourceColumn.object.errorString)
                textFormat:         Text.PlainText
                visible:            sourceColumn.object.errorString !== ""
            }
        }
    }
}
