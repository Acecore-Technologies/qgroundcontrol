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
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Geozones search, legend and display options for a map tool strip drop panel. The sources are managed in the
/// Geozones settings, see manageRequested.
ColumnLayout {
    id:         root
    spacing:    ScreenTools.defaultFontPixelHeight / 3

    property int    hiddenZoneCount:    0       ///< Smallest zones in the map view left out to limit the number of map items
    property var    map:                null    ///< Map whose visible area can be downloaded from providers serving zones by area
    property bool   showCrossedZones:   false   ///< See GeoZoneLegend
    property bool   drawingEnabled:     false   ///< Offers to draw zones of the operator, see GeoZoneOperatorZonesSection

    signal manageRequested  ///< The sources row was clicked, to open the Geozones settings
    signal zoneSelected     ///< A zone picked in the search is shown on the map
    signal drawingStarted   ///< A zone of the operator is drawn on the map

    readonly property var   _geoZoneManager:    GeoZoneManager
    readonly property real  _textWidth:         ScreenTools.defaultFontPixelWidth * 46
    readonly property bool  _planned:           !isNaN(_geoZoneManager.planning.plannedStart.getTime())
    readonly property string _timeFormat:       "yyyy-MM-dd HH:mm"

    property bool _invalidTime: false   ///< The text of the planned start can't be read, the previous start is kept

    // Without a planned start its field is hidden, with what can't be read in it
    on_PlannedChanged: {
        if (!_planned) {
            _invalidTime = false
        }
    }

    /// Sets the planned start from local time text, keeping the previous one when the text is invalid
    /// @return false when the text is invalid
    function _applyPlannedTime(text) {
        const date = Date.fromLocaleString(Qt.locale(), text.trim(), _timeFormat)
        _invalidTime = isNaN(date.getTime())
        if (!_invalidTime) {
            _setPlannedStart(date)
        }
        return !_invalidTime
    }

    function _setPlannedStart(date) {
        _invalidTime = false
        _geoZoneManager.planning.plannedStart = date
        plannedTimeField.text = Qt.formatDateTime(date, _timeFormat)
    }

    /// @return The current time to the minute, like the times entered
    function _now() {
        const now = new Date()
        now.setSeconds(0, 0)
        return now
    }

    function _nowText() {
        return Qt.formatDateTime(_now(), _timeFormat)
    }

    Component.onCompleted: plannedTimeField.text = _planned ? Qt.formatDateTime(_geoZoneManager.planning.plannedStart, _timeFormat) : _nowText()

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    GeoZoneSearch {
        Layout.preferredWidth:  root._textWidth
        onZoneSelected:         root.zoneSelected()
    }

    QGCCheckBoxSlider {
        Layout.fillWidth:   true
        text:               qsTr("Show geozones")
        checked:            root._geoZoneManager.display.visible
        onClicked:          root._geoZoneManager.display.visible = checked
    }

    GeoZoneLegend {
        showCrossedZones: root.showCrossedZones
    }

    QGCLabel {
        Layout.preferredWidth:  root._textWidth
        text:                   qsTr("Click a zone on the map for its details, when not adding items.")
        wrapMode:               Text.WordWrap
        font.pointSize:         ScreenTools.smallFontPointSize
    }

    QGCCheckBoxSlider {
        Layout.fillWidth:   true
        text:               qsTr("Check zones at the flight time")
        checked:            root._planned
        onClicked: {
            if (!checked) {
                root._geoZoneManager.planning.clearPlannedStart()
            } else if (!root._applyPlannedTime(plannedTimeField.text)) {
                // The switch never stays on without a start and its field: a text which can't be read starts from now
                root._setPlannedStart(root._now())
            }
            // The switch shows whether there is a planned start, whatever the click did
            checked = Qt.binding(() => root._planned)
        }
    }

    RowLayout {
        Layout.preferredWidth:  root._textWidth
        visible:                root._planned

        QGCTextField {
            id:                 plannedTimeField
            Layout.fillWidth:   true
            placeholderText:    root._timeFormat
            onEditingFinished:  root._applyPlannedTime(text)
        }

        QGCButton {
            text:       qsTr("Now")
            onClicked:  root._setPlannedStart(root._now())
        }
    }

    QGCLabel {
        Layout.preferredWidth:  root._textWidth
        visible:                root._planned
        wrapMode:               Text.WordWrap
        font.pointSize:         ScreenTools.smallFontPointSize
        color:                  root._invalidTime ? qgcPal.warningText : qgcPal.text
        text:                   root._invalidTime ? qsTr("Enter the local date and time like %1.").arg(root._nowText()) :
                                                    qsTr("Local start time. The mission check and altitude profile leave out zones which aren't active during the mission.")
    }

    GeoZoneSummary {
        Layout.preferredWidth:  root._textWidth
        hiddenZoneCount:        root.hiddenZoneCount
    }

    RowLayout {
        Layout.preferredWidth:  root._textWidth
        spacing:                ScreenTools.defaultFontPixelWidth / 2

        // The sources are turned on and off, refreshed and removed in the Geozones settings
        Rectangle {
            Layout.fillWidth:       true
            Layout.preferredHeight: ScreenTools.implicitButtonHeight
            radius:                 ScreenTools.defaultFontPixelWidth / 2
            color:                  sourcesMouseArea.containsMouse ? qgcPal.buttonHighlight : qgcPal.button

            RowLayout {
                anchors.fill:           parent
                anchors.leftMargin:     ScreenTools.defaultFontPixelWidth
                anchors.rightMargin:    ScreenTools.defaultFontPixelWidth
                spacing:                ScreenTools.defaultFontPixelWidth

                QGCLabel {
                    Layout.fillWidth:   true
                    elide:              Text.ElideRight
                    color:              sourcesMouseArea.containsMouse ? qgcPal.buttonHighlightText : qgcPal.buttonText
                    text:               root._geoZoneManager.disabledSourceCount > 0 ?
                                            qsTr("Sources (%1, %2 off)").arg(root._geoZoneManager.sources.count).arg(root._geoZoneManager.disabledSourceCount) :
                                            qsTr("Sources (%1)").arg(root._geoZoneManager.sources.count)
                }

                QGCLabel {
                    color:          sourcesMouseArea.containsMouse ? qgcPal.buttonHighlightText : qgcPal.buttonText
                    text:           "\u203A"
                    font.pointSize: ScreenTools.largeFontPointSize
                }
            }

            MouseArea {
                id:             sourcesMouseArea
                anchors.fill:   parent
                hoverEnabled:   true
                onClicked:      root.manageRequested()
            }
        }

        // Collapsed, so that the panel fits small screens
        QGCButton {
            id:         areaButton
            text:       qsTr("Map Area") + (checked ? " \u25B4" : " \u25BE")
            checkable:  true
            visible:    root.map !== null
        }
    }

    // Some national services serve their zones by area, the map area is downloaded and kept for offline use
    ColumnLayout {
        id:                     areaColumn
        Layout.preferredWidth:  root._textWidth
        spacing:                ScreenTools.defaultFontPixelHeight / 4
        visible:                root.map !== null && areaButton.checked

        readonly property var   _providers:     root._geoZoneManager.areaProviders
        /// The provider chosen, null when the feed catalogue has none serving zones by area
        readonly property var   _provider:      (areaProviderCombo.currentIndex >= 0 && areaProviderCombo.currentIndex < _providers.length) ?
                                                    _providers[areaProviderCombo.currentIndex] : null
        readonly property var   _mapArea:       root.map ? root.map.visibleRegion.boundingGeoRectangle() : QtPositioning.rectangle()
        readonly property bool  _areaTooLarge:  _mapArea.isValid && (_mapArea.width > 1 || _mapArea.height > 1)
        // Providers whose download covers the map area already, which is extended by moving or zooming out the map
        readonly property var   _downloaded:    (root._geoZoneManager.sources.count, _providers.map(provider => root._geoZoneManager.areaDownloaded(provider.id, _mapArea)))

        QGCLabel {
            Layout.fillWidth:   true
            text:               qsTr("Download zones for the map area")
        }

        RowLayout {
            Layout.fillWidth:   true

            GeoZoneChoiceComboBox {
                id:                 areaProviderCombo
                Layout.fillWidth:   true
                model:              areaColumn._providers.map(provider => provider.name)
                choicesEnabled:     areaColumn._downloaded.map(downloaded => !downloaded)
                disabledText:       qsTr("%1 (downloaded here)")
                // The provider covering the map center
                Component.onCompleted: {
                    const providers = areaColumn._providers
                    const center = areaColumn._mapArea.center
                    for (let i = 0; i < providers.length; i++) {
                        if (providers[i].coverage.contains(center)) {
                            currentIndex = i
                            break
                        }
                    }
                }
            }

            QGCButton {
                text:       qsTr("Download")
                enabled:    areaColumn._provider !== null && !areaColumn._areaTooLarge && !root._geoZoneManager.repository.busy && areaProviderCombo.currentEnabled
                onClicked: {
                    if (areaColumn._provider) {
                        root._geoZoneManager.addArea(areaColumn._provider.id, areaColumn._mapArea)
                    }
                }
            }
        }

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            font.pointSize:     ScreenTools.smallFontPointSize
            color:              (areaColumn._provider === null || areaColumn._areaTooLarge) ? qgcPal.warningText : qgcPal.text
            text:               areaColumn._provider === null ? qsTr("The feed catalogue has no service which serves zones by area.") :
                                areaColumn._areaTooLarge ? qsTr("Zoom in to an area up to about 100 km wide.") :
                                    (!areaProviderCombo.currentEnabled ? qsTr("The zones of the map area are downloaded already. Move the map or zoom out to download more.") :
                                                                         qsTr("Downloaded areas are kept for offline use and refreshed like feeds."))
        }
    }

    GeoZoneOperatorZonesSection {
        Layout.preferredWidth:  root._textWidth
        map:                    root.map
        visible:                root.drawingEnabled && root.map !== null
        onDrawingStarted:       root.drawingStarted()
    }
}
