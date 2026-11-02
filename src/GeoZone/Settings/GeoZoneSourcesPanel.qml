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
    readonly property var   _repository:        GeoZoneManager.repository
    readonly property var   _geoZoneSettings:   GeoZoneManager.settings
    readonly property real  _margin:            ScreenTools.defaultFontPixelWidth / 2

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCLabel {
        Layout.fillWidth:   true
        wrapMode:           Text.WordWrap
        text:               qsTr("Add EUROCAE ED-269 or ED-318 files or national feeds to show UAS geographical zones on the maps.")
        visible:            _geoZoneManager.sources.count === 0
    }

    GeoZoneSourceList {
        Layout.fillWidth:   true
    }

    ColumnLayout {
        Layout.fillWidth:   true
        spacing:            _margin / 2
        visible:            _repository.busy

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            text:               _repository.progressText
        }

        Rectangle {
            id:                 progressBar
            Layout.fillWidth:   true
            implicitHeight:     ScreenTools.defaultFontPixelHeight / 2
            radius:             height / 2
            color:              qgcPal.button

            readonly property bool _indeterminate: _repository.progress < 0

            Rectangle {
                x:          parent._indeterminate ? _indeterminateX : 0
                width:      parent._indeterminate ? parent.width / 4 : parent.width * _repository.progress
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
            onClicked:          fileDialog.openFor({ title: qsTr("Select Geozone Files"), nameFilters: _geoZoneManager.fileDialogFilters,
                                                 selectMultiple: true, accept: files => _repository.addFiles(files) })
        }

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Add Feed...")
            checkable:          true
            checked:            feedEntry.visible
            onClicked: {
                feedEntry.visible = !feedEntry.visible
                serviceEntry.visible = false
                feedPresetCombo.selectEnabledChoice()
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

        GeoZoneChoiceComboBox {
            id:                 feedPresetCombo
            Layout.fillWidth:   true
            model:              _geoZoneManager.feedPresets.map(preset => preset.name).concat([ qsTr("Other URL") ])
            // The feeds already added are turned on or refreshed in the list of sources instead
            choicesEnabled:     (_geoZoneManager.sources.count, _geoZoneManager.feedPresets.map(preset => !_repository.hasSource(preset.url)))
            disabledText:       qsTr("%1 (added)")

            readonly property bool customUrl: currentIndex === _geoZoneManager.feedPresets.length
        }

        QGCTextField {
            id:                 feedUrlField
            Layout.fillWidth:   true
            placeholderText:    "https://"
            visible:            feedPresetCombo.customUrl
        }

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            text:               qsTr("This feed is added already. Turn it on or refresh it in the list of sources.")
            visible:            !feedDownloadButton.enabled && (!feedPresetCombo.customUrl || feedUrlField.text.trim() !== "")
        }

        QGCButton {
            id:                 feedDownloadButton
            Layout.fillWidth:   true
            text:               qsTr("Download")
            enabled:            feedPresetCombo.customUrl ? (feedUrlField.text.trim() !== "" && !_feedUrlAdded) : feedPresetCombo.currentEnabled
            onClicked: {
                _repository.addUrl(feedPresetCombo.customUrl ? feedUrlField.text : _geoZoneManager.feedPresets[feedPresetCombo.currentIndex].url)
                feedEntry.visible = false
            }

            readonly property bool _feedUrlAdded: (_geoZoneManager.sources.count, _repository.hasSource(feedUrlField.text))
        }
    }

    // Services taking credentials, ED-318 queries or the area around the mission and the vehicle
    GeoZoneServiceEditor {
        id:                 serviceEntry
        Layout.fillWidth:   true
        visible:            false
    }

    // Offline packs
    RowLayout {
        Layout.fillWidth:   true
        spacing:            _margin
        uniformCellSizes:   true

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Import Pack...")
            onClicked:          fileDialog.openFor({ title: qsTr("Import Geozone Packs"), nameFilters: [ qsTr("Geozone Packs (*.zip)") ],
                                                 selectMultiple: true, accept: files => _geoZoneManager.importPacks(files) })
        }

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Export Pack...")
            enabled:            _geoZoneManager.sources.count > _geoZoneManager.disabledSourceCount
            onClicked:          fileDialog.openFor({ title: qsTr("Export Geozone Pack of the Sources Turned On"), nameFilters: [ qsTr("Geozone Packs (*.zip)") ],
                                                 defaultSuffix: "zip", save: true, accept: file => _geoZoneManager.exportPack(file) })
        }
    }

    GeoZonePackSigning {
        Layout.fillWidth:   true
        dialog:             fileDialog
    }

    RowLayout {
        Layout.fillWidth:   true
        spacing:            _margin
        uniformCellSizes:   true
        visible:            _geoZoneManager.sources.count > 0

        QGCButton {
            Layout.fillWidth:   true
            text:               qsTr("Refresh Feeds")
            enabled:            _hasFeeds && !_repository.busy
            onClicked:          _repository.refreshFeeds()

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
            onClicked:          _repository.clear()
        }
    }

    // The files of the sources, packs and signing keys: a file dialog of each use, made when first used, which then
    // keeps its own folder and file
    Item {
        id:         fileDialog
        visible:    false

        property var _dialogs: ({})

        /// @param mode title, nameFilters, selectMultiple, defaultSuffix, save to save a file, and accept called with
        ///             the file or files
        function openFor(mode) {
            if (!_dialogs[mode.title]) {
                _dialogs[mode.title] = fileDialogComponent.createObject(fileDialog.parent, {
                    title: mode.title, nameFilters: mode.nameFilters, selectMultiple: !!mode.selectMultiple,
                    defaultSuffix: mode.defaultSuffix || "", accept: mode.accept })
            }
            if (mode.save) {
                _dialogs[mode.title].openForSave()
            } else {
                _dialogs[mode.title].openForLoad()
            }
        }

        Component {
            id: fileDialogComponent

            QGCFileDialog {
                folder: QGroundControl.settingsManager.appSettings.missionSavePath

                property var accept

                onAcceptedForLoad:      (file) => { accept(file); close() }
                onAcceptedForLoadFiles: (files) => { accept(files); close() }
                onAcceptedForSave:      (file) => { accept(file); close() }
            }
        }
    }
}
