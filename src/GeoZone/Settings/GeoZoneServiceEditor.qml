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

/// Adds a feed from a service taking credentials, an ED-318 query or the area around the mission and the vehicle
ColumnLayout {
    id:         serviceEntry
    spacing:    _margin

    readonly property var   _geoZoneManager:    GeoZoneManager
    readonly property real  _margin:            ScreenTools.defaultFontPixelWidth / 2
    readonly property var   _templates:         _geoZoneManager.serviceTemplates
    readonly property var   _template:          _templates[Math.max(0, serviceTemplateCombo.currentIndex)]
    readonly property int   _auth:              authCombo.currentIndex
    readonly property var   _service:           _geoZoneManager.serviceFromForm(serviceTemplateCombo.currentIndex, {
                                                    method: methodCombo.currentIndex, auth: _auth, authParameter: authParameterField.text,
                                                    user: userField.text, tokenUrl: tokenUrlField.text, responseMember: responseMemberField.text,
                                                    sha256Url: sha256UrlField.text, areaOfInterest: areaCheckBox.checked })
    readonly property var   _otherOrigins:      visible ? _geoZoneManager.serviceOtherOrigins(serviceUrlField.text, _service) : []
    readonly property string _otherOriginsText: _otherOrigins.join(", ")

    /// The other servers shown when the user ticked "I trust these servers", which the download is confirmed for
    property var _confirmedOrigins: []

    // The confirmation was for the servers shown then, not for those the addresses give now
    on_OtherOriginsTextChanged: trustOtherOriginsCheckBox.checked = false

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // The fields of the form start from the template
    function applyTemplate() {
        const form = _template.form
        trustOtherOriginsCheckBox.checked = false
        serviceUrlField.text = _template.url
        methodCombo.currentIndex = form.method
        authCombo.currentIndex = form.auth
        authParameterField.text = form.authParameter
        userField.text = form.user
        tokenUrlField.text = form.tokenUrl
        responseMemberField.text = form.responseMember
        sha256UrlField.text = form.sha256Url
        areaCheckBox.checked = form.areaOfInterest
        serviceSecretField.text = ""
    }

    Component.onCompleted: applyTemplate()

    // The names and notes of the templates may come from a feed catalogue
    GeoZoneChoiceComboBox {
        id:                 serviceTemplateCombo
        Layout.fillWidth:   true
        model:              serviceEntry._templates.map(serviceTemplate => serviceTemplate.name)
        onActivated:        serviceEntry.applyTemplate()
    }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        textFormat:         Text.PlainText
        font.pointSize:     ScreenTools.smallFontPointSize
        text:               serviceEntry._template.note
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
        text:               qsTr("Secrets are kept unencrypted in a file only your user account can read, never in the settings, the list of sources, logs or exported packs.")
        visible:            serviceEntry._auth > 0
    }

    QGCCheckBox {
        id:     areaCheckBox
        text:   qsTr("Download the zones around the mission, the vehicle and the map view")
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
        id:                 trustOtherOriginsCheckBox
        text:               qsTr("I trust these servers")
        visible:            serviceEntry._otherOrigins.length > 0
        onCheckedChanged:   serviceEntry._confirmedOrigins = checked ? serviceEntry._otherOrigins.slice() : []
    }

    QGCButton {
        Layout.fillWidth:   true
        text:               qsTr("Download")
        enabled:            serviceUrlField.text.trim() !== "" && serviceEntry._template.supported &&
                            (serviceEntry._otherOrigins.length === 0 || trustOtherOriginsCheckBox.checked)
        onClicked: {
            // Refused when the service sends requests to a server which isn't one of those confirmed
            _geoZoneManager.addService(serviceUrlField.text, serviceEntry._service, serviceSecretField.text, serviceEntry._confirmedOrigins)
            serviceSecretField.text = ""
            trustOtherOriginsCheckBox.checked = false
            serviceEntry.visible = false
        }
    }
}
