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
import QGroundControl.Palette
import QGroundControl.ScreenTools
import QGroundControl.GeoZone

/// Geozone files and feeds: lists the loaded sources and adds, refreshes and removes them
ColumnLayout {
    spacing:    _margin

    readonly property var   _geoZoneManager:    GeoZoneManager
    readonly property var   _geoZoneSettings:   GeoZoneManager.settings
    readonly property real  _margin:            ScreenTools.defaultFontPixelWidth / 2

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        text:               qsTr("Add EUROCAE ED-269 or ED-318 files or national feeds to show UAS geographical zones on the maps.")
        visible:            _geoZoneManager.sources.count === 0
    }

    Repeater {
        model: _geoZoneManager.sources

        ColumnLayout {
            Layout.fillWidth:   true
            spacing:            _margin / 2

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
                    text:       qsTr("Remove")
                    onClicked:  _geoZoneManager.removeSource(index)
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
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               object.provenanceText
                textFormat:         Text.PlainText
                visible:            text !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               object.areaText
                visible:            object.followsArea && text !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.warningText
                text:               qsTr("The last download did not match the SHA-256 its publisher gave and was not loaded.")
                visible:            object.integrityFailed
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
                        _geoZoneManager.setCredentials(object.key, sourceSecretField.text)
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
                    wrapMode:           Text.WordWrap
                    font.pointSize:     ScreenTools.smallFontPointSize
                    text:               object.changesText
                    textFormat:         Text.PlainText
                }

                QGCLabel {
                    font.pointSize:     ScreenTools.smallFontPointSize
                    font.underline:     true
                    color:              qgcPal.buttonHighlight
                    text:               changesDetailsLabel.visible ? qsTr("Hide") : qsTr("Details")

                    MouseArea {
                        anchors.fill:   parent
                        onClicked:      changesDetailsLabel.visible = !changesDetailsLabel.visible
                    }
                }
            }

            QGCLabel {
                id:                 changesDetailsLabel
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                text:               object.changesDetails
                textFormat:         Text.PlainText
                visible:            false
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.warningText
                text:               object.warningText
                visible:            text !== ""
            }

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.warningText
                text:               qsTr("Last update failed: %1").arg(object.errorString)
                textFormat:         Text.PlainText
                visible:            object.errorString !== ""
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth:   true
        spacing:            _margin / 2
        visible:            _geoZoneManager.busy

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            text:               _geoZoneManager.progressText
        }

        Rectangle {
            id:                 progressBar
            Layout.fillWidth:   true
            implicitHeight:     ScreenTools.defaultFontPixelHeight / 2
            radius:             height / 2
            color:              qgcPal.button

            readonly property bool _indeterminate: _geoZoneManager.progress < 0

            Rectangle {
                x:          parent._indeterminate ? _indeterminateX : 0
                width:      parent._indeterminate ? parent.width / 4 : parent.width * _geoZoneManager.progress
                height:     parent.height
                radius:     parent.radius
                color:      qgcPal.buttonHighlight

                property real _indeterminateX: 0

                // Parsing and unknown download sizes have no measurable progress
                SequentialAnimation on _indeterminateX {
                    running:    progressBar.visible && progressBar._indeterminate
                    loops:      Animation.Infinite
                    NumberAnimation { from: 0; to: progressBar.width * 0.75; duration: 800; easing.type: Easing.InOutQuad }
                    NumberAnimation { from: progressBar.width * 0.75; to: 0; duration: 800; easing.type: Easing.InOutQuad }
                }
            }
        }
    }

    // Both button rows split at the middle, whatever the text lengths
    RowLayout {
        Layout.fillWidth:   true
        spacing:            _margin
        uniformCellSizes:   true

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Add File...")
            onClicked:          fileDialog.openForLoad()
        }

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Add Feed...")
            checkable:          true
            checked:            feedEntry.visible
            onClicked: {
                feedEntry.visible = !feedEntry.visible
                serviceEntry.visible = false
            }
        }

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Add Service...")
            checkable:          true
            checked:            serviceEntry.visible
            onClicked: {
                serviceEntry.visible = !serviceEntry.visible
                feedEntry.visible = false
            }
        }
    }

    ColumnLayout {
        id:                 feedEntry
        Layout.fillWidth:   true
        spacing:            _margin
        visible:            false

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            text:               qsTr("Feeds are checked for updates every %1 hours, temporary flight restrictions every %2 minutes.")
                                    .arg(_geoZoneSettings.feedRefreshHours.rawValue)
                                    .arg(_geoZoneSettings.liveFeedRefreshMinutes.rawValue)
        }

        QGCComboBox {
            id:                 feedPresetCombo
            Layout.fillWidth:   true
            model:              _geoZoneManager.feedPresets.map(preset => preset.name).concat([ qsTr("Other URL") ])

            readonly property bool customUrl: currentIndex === _geoZoneManager.feedPresets.length
        }

        QGCTextField {
            id:                 feedUrlField
            Layout.fillWidth:   true
            placeholderText:    "https://"
            visible:            feedPresetCombo.customUrl
        }

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Download")
            enabled:            !feedPresetCombo.customUrl || feedUrlField.text.trim() !== ""
            onClicked: {
                _geoZoneManager.addUrl(feedPresetCombo.customUrl ? feedUrlField.text : _geoZoneManager.feedPresets[feedPresetCombo.currentIndex].url)
                feedEntry.visible = false
            }
        }
    }

    // Services taking credentials, ED-318 queries or the area around the mission and the vehicle
    ColumnLayout {
        id:                 serviceEntry
        Layout.fillWidth:   true
        spacing:            _margin
        visible:            false

        readonly property var   _templates: _geoZoneManager.serviceTemplates
        readonly property var   _template:  serviceTemplateCombo.currentIndex < _templates.length ? _templates[serviceTemplateCombo.currentIndex] : null
        readonly property var   _authTypes: [ "none", "apiKeyHeader", "apiKeyQuery", "bearer", "basic", "oauth2ClientCredentials" ]
        readonly property int   _auth:      authCombo.currentIndex
        // Bindings follow the fields serviceMap reads
        readonly property var   _otherOrigins: visible ? _geoZoneManager.serviceOtherOrigins(serviceUrlField.text, serviceMap()) : []

        function applyTemplate() {
            var service = _template ? _template.service : {}
            serviceUrlField.text = _template ? _template.url : ""
            methodCombo.currentIndex = service.method === "ed318Post" ? 1 : 0
            authCombo.currentIndex = Math.max(0, _authTypes.indexOf(service.auth || "none"))
            authParameterField.text = service.authParameter || ""
            userField.text = service.user || ""
            tokenUrlField.text = service.tokenUrl || ""
            responseMemberField.text = service.responseMember || ""
            sha256UrlField.text = service.sha256Url || ""
            areaCheckBox.checked = service.areaOfInterest || false
            serviceSecretField.text = ""
        }

        function serviceMap() {
            var service = _template ? Object.assign({}, _template.service) : {}
            service.method = methodCombo.currentIndex === 1 ? "ed318Post" : "get"
            service.auth = _authTypes[authCombo.currentIndex]
            service.authParameter = authParameterField.text
            service.user = userField.text
            service.tokenUrl = tokenUrlField.text
            service.responseMember = responseMemberField.text
            service.sha256Url = sha256UrlField.text
            service.areaOfInterest = areaCheckBox.checked
            return service
        }

        Component.onCompleted: applyTemplate()

        QGCComboBox {
            id:                 serviceTemplateCombo
            Layout.fillWidth:   true
            model:              serviceEntry._templates.map(serviceTemplate => serviceTemplate.name).concat([ qsTr("Other service") ])
            onActivated:        serviceEntry.applyTemplate()
        }

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            text:               serviceEntry._template ? serviceEntry._template.note : qsTr("Enter the address of the service and how it is accessed.")
        }

        QGCTextField {
            id:                 serviceUrlField
            Layout.fillWidth:   true
            placeholderText:    "https://"
        }

        GridLayout {
            Layout.fillWidth:   true
            columns:            2
            columnSpacing:      _margin
            rowSpacing:         _margin / 2

            QGCLabel { text: qsTr("Request") }
            QGCComboBox {
                id:                 methodCombo
                Layout.fillWidth:   true
                model:              [ qsTr("GET"), qsTr("ED-318 query (POST)") ]
            }

            QGCLabel { text: qsTr("Authentication") }
            QGCComboBox {
                id:                 authCombo
                Layout.fillWidth:   true
                model:              [ qsTr("None"), qsTr("API key header"), qsTr("API key query parameter"), qsTr("Bearer token"), qsTr("User and password"), qsTr("OAuth2 client credentials") ]
            }

            QGCLabel {
                text:       serviceEntry._auth === 1 ? qsTr("Header") : qsTr("Parameter")
                visible:    authParameterField.visible
            }
            QGCTextField {
                id:                 authParameterField
                Layout.fillWidth:   true
                placeholderText:    serviceEntry._auth === 1 ? "X-API-Key" : "api_key"
                visible:            serviceEntry._auth === 1 || serviceEntry._auth === 2
            }

            QGCLabel {
                text:       serviceEntry._auth === 5 ? qsTr("Client id") : qsTr("User")
                visible:    userField.visible
            }
            QGCTextField {
                id:                 userField
                Layout.fillWidth:   true
                visible:            serviceEntry._auth === 4 || serviceEntry._auth === 5
            }

            QGCLabel {
                text:       qsTr("Token URL")
                visible:    tokenUrlField.visible
            }
            QGCTextField {
                id:                 tokenUrlField
                Layout.fillWidth:   true
                placeholderText:    "https://"
                visible:            serviceEntry._auth === 5
            }

            QGCLabel {
                text:       [ "", qsTr("API key"), qsTr("API key"), qsTr("Token"), qsTr("Password"), qsTr("Client secret") ][serviceEntry._auth]
                visible:    serviceSecretField.visible
            }
            QGCTextField {
                id:                 serviceSecretField
                Layout.fillWidth:   true
                echoMode:           TextInput.Password
                visible:            serviceEntry._auth > 0
            }

            QGCLabel { text: qsTr("Zones member") }
            QGCTextField {
                id:                 responseMemberField
                Layout.fillWidth:   true
                placeholderText:    qsTr("Optional, like data.geojson")
            }

            QGCLabel { text: qsTr("SHA-256 URL") }
            QGCTextField {
                id:                 sha256UrlField
                Layout.fillWidth:   true
                placeholderText:    qsTr("Optional, like {url}.sha256")
            }
        }

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            text:               qsTr("Secrets are kept unencrypted in the QGroundControl settings, never in the list of sources, logs or exported packs.")
            visible:            serviceEntry._auth > 0
        }

        QGCCheckBox {
            id:     areaCheckBox
            text:   qsTr("Download the zones around the mission and the vehicle")
        }

        // The client secret or the area of interest would go to another server than the feed's
        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            color:              qgcPal.warningText
            text:               qsTr("The token or SHA-256 address is on another server than the feed: %1. It receives the client secret or the area of interest.")
                                    .arg(serviceEntry._otherOrigins.join(", "))
            textFormat:         Text.PlainText
            visible:            serviceEntry._otherOrigins.length > 0
        }

        QGCCheckBox {
            id:         trustOtherOriginsCheckBox
            text:       qsTr("I trust these servers")
            visible:    serviceEntry._otherOrigins.length > 0
        }

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Download")
            enabled:            serviceUrlField.text.trim() !== "" && (!serviceEntry._template || serviceEntry._template.supported) &&
                                (serviceEntry._otherOrigins.length === 0 || trustOtherOriginsCheckBox.checked)
            onClicked: {
                _geoZoneManager.addService(serviceUrlField.text, serviceEntry.serviceMap(), serviceSecretField.text, trustOtherOriginsCheckBox.checked)
                serviceSecretField.text = ""
                trustOtherOriginsCheckBox.checked = false
                serviceEntry.visible = false
            }
        }
    }

    // Offline packs and the provenance of the sources
    RowLayout {
        Layout.fillWidth:   true
        spacing:            _margin
        uniformCellSizes:   true

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Import Pack...")
            onClicked:          importPackDialog.openForLoad()
        }

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Export Pack...")
            enabled:            _geoZoneManager.sources.count > _geoZoneManager.disabledSourceCount
            onClicked:          exportPackDialog.openForSave()
        }

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Export Provenance...")
            enabled:            _geoZoneManager.sources.count > 0
            onClicked:          provenanceDialog.openForSave()
        }
    }

    RowLayout {
        Layout.fillWidth:   true
        spacing:            _margin
        uniformCellSizes:   true
        visible:            _geoZoneManager.sources.count > 0

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Refresh Feeds")
            enabled:            _hasFeeds && !_geoZoneManager.busy
            onClicked:          _geoZoneManager.refreshFeeds()

            readonly property bool _hasFeeds: {
                for (var i = 0; i < _geoZoneManager.sources.count; i++) {
                    if (_geoZoneManager.sources.get(i).url !== "") {
                        return true
                    }
                }
                return false
            }
        }

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Remove All")
            onClicked:          _geoZoneManager.clear()
        }
    }

    QGCFileDialog {
        id:             fileDialog
        folder:         QGroundControl.settingsManager.appSettings.missionSavePath
        title:          qsTr("Select Geozone File")
        nameFilters:    _geoZoneManager.fileDialogFilters

        onAcceptedForLoad: (file) => {
            _geoZoneManager.addFile(file)
            close()
        }
    }

    QGCFileDialog {
        id:             importPackDialog
        folder:         QGroundControl.settingsManager.appSettings.missionSavePath
        title:          qsTr("Import Geozone Pack")
        nameFilters:    [ qsTr("Geozone Packs (*.zip)") ]

        onAcceptedForLoad: (file) => {
            _geoZoneManager.importPack(file)
            close()
        }
    }

    QGCFileDialog {
        id:             exportPackDialog
        folder:         QGroundControl.settingsManager.appSettings.missionSavePath
        title:          qsTr("Export Geozone Pack of the Sources Turned On")
        nameFilters:    [ qsTr("Geozone Packs (*.zip)") ]
        defaultSuffix:  "zip"

        onAcceptedForSave: (file) => {
            _geoZoneManager.exportPack(file)
            close()
        }
    }

    QGCFileDialog {
        id:             provenanceDialog
        folder:         QGroundControl.settingsManager.appSettings.missionSavePath
        title:          qsTr("Export Geozone Provenance")
        nameFilters:    [ qsTr("JSON Files (*.json)") ]
        defaultSuffix:  "json"

        onAcceptedForSave: (file) => {
            _geoZoneManager.exportProvenance(file)
            close()
        }
    }
}
