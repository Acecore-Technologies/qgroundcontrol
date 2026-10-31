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

/// Geozone files and feeds: lists the loaded sources and adds, refreshes and removes them
ColumnLayout {
    id:         control
    spacing:    _margin

    readonly property var   _geoZoneManager:    QGroundControl.geoZoneManager
    readonly property var   _geoZoneSettings:   QGroundControl.settingsManager.geoZoneSettings
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

                QGCLabel {
                    Layout.fillWidth:   true
                    elide:              Text.ElideRight
                    text:               object.name
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
                text:               object.detailsText !== "" ? object.detailsText : qsTr("Loading...")
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

            QGCLabel {
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.warningText
                text:               qsTr("Last update failed: %1").arg(object.errorString)
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

    RowLayout {
        Layout.fillWidth:   true
        spacing:            _margin

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
            onClicked:          feedEntry.visible = !feedEntry.visible
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

    RowLayout {
        Layout.fillWidth:   true
        spacing:            _margin
        visible:            _geoZoneManager.sources.count > 0

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Refresh Feeds")
            visible:            _hasFeeds
            enabled:            !_geoZoneManager.busy
            onClicked:          _geoZoneManager.refreshFeeds()

            property bool _hasFeeds: {
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
            visible:            _geoZoneManager.sources.count > 1
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
}
