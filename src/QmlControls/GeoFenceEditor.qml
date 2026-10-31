import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtPositioning

import QGroundControl
import QGroundControl.ScreenTools
import QGroundControl.Controls
import QGroundControl.FactSystem
import QGroundControl.FactControls
import QGroundControl.FlightMap

QGCFlickable {
    id:             root
    contentHeight:  geoFenceEditorRect.height
    clip:           true

    property var    myGeoFenceController
    property var    flightMap
    property int    geoZonesHiddenCount:    0   ///< Smallest geozones in the map view left out to limit the number of map items

    readonly property real  _editFieldWidth:    Math.min(width - _margin * 2, ScreenTools.defaultFontPixelWidth * 15)
    readonly property real  _margin:            ScreenTools.defaultFontPixelWidth / 2
    readonly property real  _radius:            ScreenTools.defaultFontPixelWidth / 2
    readonly property var   _geoZoneManager:    QGroundControl.geoZoneManager
    readonly property bool  _geoZonesLoaded:    _geoZoneManager.zones.count > 0
    readonly property real  _maxFenceListHeight: ScreenTools.defaultFontPixelHeight * 16

    Rectangle {
        id:     geoFenceEditorRect
        anchors.left:   parent.left
        anchors.right:  parent.right
        height: geoFenceItems.y + geoFenceItems.height + (_margin * 2)
        radius: _radius
        color:  qgcPal.missionItemEditor

        QGCLabel {
            id:                 geoFenceLabel
            anchors.margins:    _margin
            anchors.left:       parent.left
            anchors.top:        parent.top
            text:               qsTr("GeoFence")
            anchors.leftMargin: ScreenTools.defaultFontPixelWidth
        }

        Rectangle {
            id:                 geoFenceItems
            anchors.margins:    _margin
            anchors.left:       parent.left
            anchors.right:      parent.right
            anchors.top:        geoFenceLabel.bottom
            height:             fenceColumn.y + fenceColumn.height + (_margin * 2)
            color:              qgcPal.windowShadeDark
            radius:             _radius

            Column {
                id:                 fenceColumn
                anchors.margins:    _margin
                anchors.top:        parent.top
                anchors.left:       parent.left
                anchors.right:      parent.right
                spacing:            _margin

                QGCLabel {
                    anchors.left:       parent.left
                    anchors.right:      parent.right
                    wrapMode:           Text.WordWrap
                    font.pointSize:     myGeoFenceController.supported ? ScreenTools.smallFontPointSize : ScreenTools.defaultFontPointSize
                    text:               myGeoFenceController.supported ?
                                            qsTr("GeoFencing allows you to set a virtual fence around the area you want to fly in.") :
                                            qsTr("This vehicle does not support GeoFence.")
                }

                Column {
                    anchors.left:       parent.left
                    anchors.right:      parent.right
                    spacing:            _margin
                    visible:            myGeoFenceController.supported

                    Repeater {
                        model: myGeoFenceController.params

                        Item {
                            width:  fenceColumn.width
                            height: textField.height

                            property bool showCombo: modelData.enumStrings.length > 0

                            QGCLabel {
                                id:                 textFieldLabel
                                anchors.baseline:   textField.baseline
                                text:               myGeoFenceController.paramLabels[index]
                            }

                            FactTextField {
                                id:             textField
                                anchors.right:  parent.right
                                width:          _editFieldWidth
                                showUnits:      true
                                fact:           modelData
                                visible:        !parent.showCombo
                            }

                            FactComboBox {
                                id:             comboField
                                anchors.right:  parent.right
                                width:          _editFieldWidth
                                indexModel:     false
                                fact:           showCombo ? modelData : _nullFact
                                visible:        parent.showCombo

                                property var _nullFact: Fact { }
                            }
                        }
                    }

                    SectionHeader {
                        id:             insertSection
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        text:           qsTr("Insert GeoFence")
                    }

                    QGCButton {
                        Layout.fillWidth:   true
                        text:               qsTr("Polygon Fence")

                        onClicked: {
                            var rect = Qt.rect(flightMap.centerViewport.x, flightMap.centerViewport.y, flightMap.centerViewport.width, flightMap.centerViewport.height)
                            var topLeftCoord = flightMap.toCoordinate(Qt.point(rect.x, rect.y), false /* clipToViewPort */)
                            var bottomRightCoord = flightMap.toCoordinate(Qt.point(rect.x + rect.width, rect.y + rect.height), false /* clipToViewPort */)
                            myGeoFenceController.addInclusionPolygon(topLeftCoord, bottomRightCoord)
                        }
                    }

                    QGCButton {
                        Layout.fillWidth:   true
                        text:               qsTr("Circular Fence")

                        onClicked: {
                            var rect = Qt.rect(flightMap.centerViewport.x, flightMap.centerViewport.y, flightMap.centerViewport.width, flightMap.centerViewport.height)
                            var topLeftCoord = flightMap.toCoordinate(Qt.point(rect.x, rect.y), false /* clipToViewPort */)
                            var bottomRightCoord = flightMap.toCoordinate(Qt.point(rect.x + rect.width, rect.y + rect.height), false /* clipToViewPort */)
                            myGeoFenceController.addInclusionCircle(topLeftCoord, bottomRightCoord)
                        }
                    }

                    SectionHeader {
                        id:             polygonSection
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        text:           myGeoFenceController.polygons.count > 0 ?
                                            qsTr("Polygon Fences (%1)").arg(myGeoFenceController.polygons.count) :
                                            qsTr("Polygon Fences")
                    }

                    QGCLabel {
                        text:       qsTr("None")
                        visible:    polygonSection.checked && myGeoFenceController.polygons.count === 0
                    }

                    // Long lists scroll on their own so the sections below stay within reach
                    QGCFlickable {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        height:         Math.min(polygonGrid.implicitHeight, _maxFenceListHeight)
                        contentWidth:   width
                        contentHeight:  polygonGrid.implicitHeight
                        visible:        polygonSection.checked && myGeoFenceController.polygons.count > 0

                        GridLayout {
                            id:                 polygonGrid
                            width:              parent.width
                            columns:            3
                            flow:               GridLayout.TopToBottom

                            QGCLabel {
                                text:               qsTr("Inclusion")
                                Layout.column:      0
                                Layout.alignment:   Qt.AlignHCenter
                            }

                            Repeater {
                                model: myGeoFenceController.polygons

                                QGCCheckBox {
                                    checked:            object.inclusion
                                    onClicked:          object.inclusion = checked
                                    Layout.alignment:   Qt.AlignHCenter
                                }
                            }

                            QGCLabel {
                                text:               qsTr("Edit")
                                Layout.column:      1
                                Layout.alignment:   Qt.AlignHCenter
                            }

                            Repeater {
                                model: myGeoFenceController.polygons

                                QGCRadioButton {
                                    checked:            _interactive
                                    Layout.alignment:   Qt.AlignHCenter

                                    property bool _interactive: object.interactive

                                    on_InteractiveChanged: checked = _interactive

                                    onClicked: {
                                        myGeoFenceController.clearAllInteractive()
                                        object.interactive = checked
                                    }
                                }
                            }

                            QGCLabel {
                                text:               qsTr("Delete")
                                Layout.column:      2
                                Layout.alignment:   Qt.AlignHCenter
                            }

                            Repeater {
                                model: myGeoFenceController.polygons

                                QGCButton {
                                    text:               qsTr("Del")
                                    Layout.alignment:   Qt.AlignHCenter
                                    onClicked:          myGeoFenceController.deletePolygon(index)
                                }
                            }
                        } // GridLayout
                    }

                    QGCButton {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        text:           qsTr("Delete All Polygon Fences")
                        visible:        polygonSection.checked && myGeoFenceController.polygons.count > 1
                        onClicked: {
                            mainWindow.showMessageDialog(text,
                                                         qsTr("Delete all %1 polygon fences?").arg(myGeoFenceController.polygons.count),
                                                         Dialog.Ok | Dialog.Cancel,
                                                         function() {
                                                             for (var i = myGeoFenceController.polygons.count - 1; i >= 0; i--) {
                                                                 myGeoFenceController.deletePolygon(i)
                                                             }
                                                         })
                        }
                    }

                    SectionHeader {
                        id:             circleSection
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        text:           myGeoFenceController.circles.count > 0 ?
                                            qsTr("Circular Fences (%1)").arg(myGeoFenceController.circles.count) :
                                            qsTr("Circular Fences")
                    }

                    QGCLabel {
                        text:       qsTr("None")
                        visible:    circleSection.checked && myGeoFenceController.circles.count === 0
                    }

                    QGCFlickable {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        height:         Math.min(circleGrid.implicitHeight, _maxFenceListHeight)
                        contentWidth:   width
                        contentHeight:  circleGrid.implicitHeight
                        visible:        circleSection.checked && myGeoFenceController.circles.count > 0

                        GridLayout {
                            id:                 circleGrid
                            width:              parent.width
                            columns:            4
                            flow:               GridLayout.TopToBottom

                            QGCLabel {
                                text:               qsTr("Inclusion")
                                Layout.column:      0
                                Layout.alignment:   Qt.AlignHCenter
                            }

                            Repeater {
                                model: myGeoFenceController.circles

                                QGCCheckBox {
                                    checked:            object.inclusion
                                    onClicked:          object.inclusion = checked
                                    Layout.alignment:   Qt.AlignHCenter
                                }
                            }

                            QGCLabel {
                                text:               qsTr("Edit")
                                Layout.column:      1
                                Layout.alignment:   Qt.AlignHCenter
                            }

                            Repeater {
                                model: myGeoFenceController.circles

                                QGCRadioButton {
                                    checked:            _interactive
                                    Layout.alignment:   Qt.AlignHCenter

                                    property bool _interactive: object.interactive

                                    on_InteractiveChanged: checked = _interactive

                                    onClicked: {
                                        myGeoFenceController.clearAllInteractive()
                                        object.interactive = checked
                                    }
                                }
                            }

                            QGCLabel {
                                text:               qsTr("Radius")
                                Layout.column:      2
                                Layout.alignment:   Qt.AlignHCenter
                            }

                            Repeater {
                                model: myGeoFenceController.circles

                                FactTextField {
                                    fact:               object.radius
                                    Layout.fillWidth:   true
                                    Layout.alignment:   Qt.AlignHCenter
                                }
                            }

                            QGCLabel {
                                text:               qsTr("Delete")
                                Layout.column:      3
                                Layout.alignment:   Qt.AlignHCenter
                            }

                            Repeater {
                                model: myGeoFenceController.circles

                                QGCButton {
                                    text:               qsTr("Del")
                                    Layout.alignment:   Qt.AlignHCenter
                                    onClicked:          myGeoFenceController.deleteCircle(index)
                                }
                            }
                        } // GridLayout
                    }

                    QGCButton {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        text:           qsTr("Delete All Circular Fences")
                        visible:        circleSection.checked && myGeoFenceController.circles.count > 1
                        onClicked: {
                            mainWindow.showMessageDialog(text,
                                                         qsTr("Delete all %1 circular fences?").arg(myGeoFenceController.circles.count),
                                                         Dialog.Ok | Dialog.Cancel,
                                                         function() {
                                                             for (var i = myGeoFenceController.circles.count - 1; i >= 0; i--) {
                                                                 myGeoFenceController.deleteCircle(i)
                                                             }
                                                         })
                        }
                    }

                    SectionHeader {
                        id:             breachReturnSection
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        text:           qsTr("Breach Return Point")
                    }

                    QGCButton {
                        text:               qsTr("Add Breach Return Point")
                        visible:            breachReturnSection.visible && !myGeoFenceController.breachReturnPoint.isValid
                        anchors.left:       parent.left
                        anchors.right:      parent.right

                        onClicked: myGeoFenceController.breachReturnPoint = flightMap.center
                    }

                    QGCButton {
                        text:               qsTr("Remove Breach Return Point")
                        visible:            breachReturnSection.visible && myGeoFenceController.breachReturnPoint.isValid
                        anchors.left:       parent.left
                        anchors.right:      parent.right

                        onClicked: myGeoFenceController.breachReturnPoint = QtPositioning.coordinate()
                    }

                    ColumnLayout {
                        anchors.left:       parent.left
                        anchors.right:      parent.right
                        spacing:            _margin
                        visible:            breachReturnSection.visible && myGeoFenceController.breachReturnPoint.isValid

                        QGCLabel {
                            text: qsTr("Altitude")
                        }

                        FactTextField {
                            fact: myGeoFenceController.breachReturnAltitude
                        }
                    }

                }

                SectionHeader {
                    id:             geoZoneSection
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    text:           qsTr("Geozones (ED-269 / ED-318)")
                }

                Column {
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    spacing:        _margin
                    visible:        geoZoneSection.checked

                    QGCLabel {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        wrapMode:       Text.WordWrap
                        font.pointSize: ScreenTools.smallFontPointSize
                        text:           qsTr("Add EUROCAE ED-269 or ED-318 files or national feeds to show UAS geographical zones on the map.")
                        visible:        _geoZoneManager.sources.count === 0
                    }

                    Repeater {
                        model: _geoZoneManager.sources

                        Column {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            spacing:        _margin / 2

                            RowLayout {
                                anchors.left:   parent.left
                                anchors.right:  parent.right
                                spacing:        _margin

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
                                anchors.left:   parent.left
                                anchors.right:  parent.right
                                wrapMode:       Text.WordWrap
                                font.pointSize: ScreenTools.smallFontPointSize
                                text:           object.detailsText !== "" ? object.detailsText : qsTr("Loading...")
                            }

                            QGCLabel {
                                anchors.left:   parent.left
                                anchors.right:  parent.right
                                wrapMode:       Text.WordWrap
                                font.pointSize: ScreenTools.smallFontPointSize
                                font.italic:    true
                                text:           qsTr("Updating...")
                                visible:        object.busy && object.detailsText !== ""
                            }

                            QGCLabel {
                                anchors.left:   parent.left
                                anchors.right:  parent.right
                                wrapMode:       Text.WordWrap
                                font.pointSize: ScreenTools.smallFontPointSize
                                color:          qgcPal.warningText
                                text:           object.validityText
                                visible:        text !== ""
                            }

                            QGCLabel {
                                anchors.left:   parent.left
                                anchors.right:  parent.right
                                wrapMode:       Text.WordWrap
                                font.pointSize: ScreenTools.smallFontPointSize
                                color:          qgcPal.warningText
                                text:           qsTr("Last update failed: %1").arg(object.errorString)
                                visible:        object.errorString !== ""
                            }
                        }
                    }

                    QGCLabel {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        wrapMode:       Text.WordWrap
                        font.pointSize: ScreenTools.smallFontPointSize
                        text:           qsTr("%1 of the smallest zones in view are not shown. Zoom in to see them.").arg(geoZonesHiddenCount)
                        visible:        _geoZonesLoaded && _geoZoneManager.visible && geoZonesHiddenCount > 0
                    }

                    Column {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        spacing:        _margin / 2
                        visible:        _geoZoneManager.busy

                        QGCLabel {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            wrapMode:       Text.WordWrap
                            font.pointSize: ScreenTools.smallFontPointSize
                            text:           _geoZoneManager.progressText
                        }

                        Rectangle {
                            id:             geoZoneProgressBar
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            height:         ScreenTools.defaultFontPixelHeight / 2
                            radius:         height / 2
                            color:          qgcPal.button

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
                                    running:    geoZoneProgressBar.visible && geoZoneProgressBar._indeterminate
                                    loops:      Animation.Infinite
                                    NumberAnimation { from: 0; to: geoZoneProgressBar.width * 0.75; duration: 800; easing.type: Easing.InOutQuad }
                                    NumberAnimation { from: geoZoneProgressBar.width * 0.75; to: 0; duration: 800; easing.type: Easing.InOutQuad }
                                }
                            }
                        }
                    }

                    RowLayout {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        spacing:        _margin

                        QGCButton {
                            Layout.fillWidth:   true
                            text:               qsTr("Add File...")
                            onClicked:          geoZoneFileDialog.openForLoad()
                        }

                        QGCButton {
                            Layout.fillWidth:   true
                            text:               qsTr("Add Feed...")
                            checkable:          true
                            checked:            feedEntry.visible
                            onClicked:          feedEntry.visible = !feedEntry.visible
                        }
                    }

                    Column {
                        id:             feedEntry
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        spacing:        _margin
                        visible:        false

                        QGCLabel {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            wrapMode:       Text.WordWrap
                            font.pointSize: ScreenTools.smallFontPointSize
                            text:           qsTr("Feeds are checked for updates every %1 hours, temporary flight restrictions every %2 minutes.")
                                            .arg(QGroundControl.settingsManager.geoZoneSettings.feedRefreshHours.rawValue)
                                            .arg(QGroundControl.settingsManager.geoZoneSettings.liveFeedRefreshMinutes.rawValue)
                        }

                        QGCComboBox {
                            id:             feedPresetCombo
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            model:          _geoZoneManager.feedPresets.map(preset => preset.name).concat([ qsTr("Other URL") ])

                            readonly property bool customUrl: currentIndex === _geoZoneManager.feedPresets.length
                        }

                        QGCTextField {
                            id:                 feedUrlField
                            anchors.left:       parent.left
                            anchors.right:      parent.right
                            placeholderText:    "https://"
                            visible:            feedPresetCombo.customUrl
                        }

                        QGCButton {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            text:           qsTr("Download")
                            enabled:        !feedPresetCombo.customUrl || feedUrlField.text.trim() !== ""
                            onClicked: {
                                root._fitMapOnSourceAdded = true
                                _geoZoneManager.addUrl(feedPresetCombo.customUrl ? feedUrlField.text : _geoZoneManager.feedPresets[feedPresetCombo.currentIndex].url)
                                feedEntry.visible = false
                            }
                        }
                    }

                    RowLayout {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        spacing:        _margin
                        visible:        _geoZonesLoaded

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

                    QGCCheckBox {
                        text:       qsTr("Show geozones on map")
                        checked:    _geoZoneManager.visible
                        visible:    _geoZonesLoaded
                        onClicked:  _geoZoneManager.visible = checked
                    }

                    Column {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        spacing:        _margin
                        visible:        _geoZonesLoaded && myGeoFenceController.supported

                        QGCLabel {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            wrapMode:       Text.WordWrap
                            font.pointSize: ScreenTools.smallFontPointSize
                            text:           qsTr("Add the selected zone types as exclusion fences. Altitude limits and polygon holes are not transferred to the fence, and vehicles can only store a limited number of fence points.")
                        }

                        QGCCheckBox {
                            id:         importProhibitedCheck
                            text:       qsTr("Prohibited")
                            checked:    true
                            onCheckedChanged: importPreviewTimer.restart()
                        }

                        QGCCheckBox {
                            id:         importReqAuthorisationCheck
                            text:       qsTr("Authorisation required")
                            checked:    true
                            onCheckedChanged: importPreviewTimer.restart()
                        }

                        QGCCheckBox {
                            id:         importConditionalCheck
                            text:       qsTr("Conditional")
                            checked:    false
                            onCheckedChanged: importPreviewTimer.restart()
                        }

                        QGCCheckBox {
                            id:         importMapViewOnlyCheck
                            text:       qsTr("Only zones in current map view")
                            checked:    true
                            onCheckedChanged: importPreviewTimer.restart()
                        }

                        GridLayout {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            columns:        2
                            columnSpacing:  _margin
                            rowSpacing:     _margin

                            QGCLabel { text: qsTr("Active") }
                            QGCComboBox {
                                id:                 importActiveCombo
                                Layout.fillWidth:   true
                                model:              [ qsTr("Any time"), qsTr("Now"), qsTr("Next 3 hours"), qsTr("Next 24 hours") ]
                                onActivated:        importPreviewTimer.restart()

                                readonly property var hours: [ -1, 0, 3, 24 ]
                            }

                            QGCLabel { text: qsTr("Simplify") }
                            QGCComboBox {
                                id:                 importSimplifyCombo
                                Layout.fillWidth:   true
                                currentIndex:       2
                                model:              tolerances.map(meters => meters === 0 ? qsTr("Off") : root._horizontalDistanceText(meters))
                                onActivated:        importPreviewTimer.restart()

                                readonly property var tolerances: [ 0, 10, 25, 50, 100 ]
                            }
                        }

                        QGCLabel {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            wrapMode:       Text.WordWrap
                            font.pointSize: ScreenTools.smallFontPointSize
                            text:           qsTr("Simplified fences have fewer points and enclose the zone, extending up to twice the tolerance beyond it.")
                            visible:        importSimplifyCombo.currentIndex > 0
                        }

                        QGCLabel {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            wrapMode:       Text.WordWrap
                            text:           _importPreview.fenceCount > 0 ?
                                                qsTr("Adds %1 fences with %2 points (%3 fence points in total).").arg(_importPreview.fenceCount).arg(_importPreview.pointCount).arg(_importPreview.totalPointCount) :
                                                qsTr("No new matching zones.")
                        }

                        QGCLabel {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            wrapMode:       Text.WordWrap
                            color:          qgcPal.warningText
                            text:           _importPreview.limitWarning
                            visible:        text !== ""
                        }

                        QGCButton {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            text:           qsTr("Add Exclusion Fences")
                            enabled:        importProhibitedCheck.checked || importReqAuthorisationCheck.checked || importConditionalCheck.checked
                            onClicked:      root._importGeoZones()
                        }
                    }
                }
            }
        }
    } // Rectangle

    property var _importPreview: ({ fenceCount: 0, pointCount: 0, totalPointCount: 0, limitWarning: "" })
    readonly property bool _importVisible: geoZoneSection.checked && _geoZonesLoaded && myGeoFenceController.supported

    on_ImportVisibleChanged: importPreviewTimer.restart()

    // Previews are computed after changes settle since simplifying many zones takes a moment
    Timer {
        id:             importPreviewTimer
        interval:       300
        onTriggered:    root._updateImportPreview()
    }

    Connections {
        target:                 flightMap
        enabled:                importMapViewOnlyCheck.checked
        function onVisibleRegionChanged() { importPreviewTimer.restart() }
    }

    Connections {
        target:                 _geoZoneManager
        function onZonesChanged() { importPreviewTimer.restart() }
    }

    Connections {
        target:                 myGeoFenceController.polygons
        function onCountChanged() { importPreviewTimer.restart() }
    }

    Connections {
        target:                 myGeoFenceController.circles
        function onCountChanged() { importPreviewTimer.restart() }
    }

    function _horizontalDistanceText(meters) {
        var distance = QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnits(meters)
        return Math.round(distance) + " " + QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString
    }

    function _importRestrictions() {
        var restrictions = []
        if (importProhibitedCheck.checked) {
            restrictions.push(GeoZone.Prohibited)
        }
        if (importReqAuthorisationCheck.checked) {
            restrictions.push(GeoZone.ReqAuthorisation)
        }
        if (importConditionalCheck.checked) {
            restrictions.push(GeoZone.Conditional)
        }
        return restrictions
    }

    function _importArea() {
        return importMapViewOnlyCheck.checked ? flightMap.visibleRegion.boundingGeoRectangle() : QtPositioning.rectangle()
    }

    function _updateImportPreview() {
        if (!_importVisible) {
            return
        }
        _importPreview = myGeoFenceController.geoZoneImportPreview(_importRestrictions(), _importArea(),
                                                                   importActiveCombo.hours[importActiveCombo.currentIndex],
                                                                   importSimplifyCombo.tolerances[importSimplifyCombo.currentIndex])
    }

    function _importGeoZones() {
        var restrictions = _importRestrictions()
        var area = _importArea()
        var activeWithinHours = importActiveCombo.hours[importActiveCombo.currentIndex]
        var tolerance = importSimplifyCombo.tolerances[importSimplifyCombo.currentIndex]
        var doImport = function() {
            var fenceCount = myGeoFenceController.importGeoZones(restrictions, area, activeWithinHours, tolerance)
            mainWindow.showMessageDialog(qsTr("Geozones"),
                                         fenceCount > 0 ?
                                             qsTr("%1 exclusion fences added.").arg(fenceCount) :
                                             qsTr("No new matching zones found."))
        }

        var preview = myGeoFenceController.geoZoneImportPreview(restrictions, area, activeWithinHours, tolerance)
        if (preview.limitWarning !== "") {
            mainWindow.showMessageDialog(qsTr("Geozones"), preview.limitWarning + "\n\n" + qsTr("Add the fences anyway?"),
                                         Dialog.Ok | Dialog.Cancel, doImport)
        } else {
            doImport()
        }
    }

    QGCFileDialog {
        id:             geoZoneFileDialog
        folder:         QGroundControl.settingsManager.appSettings.missionSavePath
        title:          qsTr("Select Geozone File")
        nameFilters:    _geoZoneManager.fileDialogFilters

        onAcceptedForLoad: (file) => {
            root._fitMapOnSourceAdded = true
            _geoZoneManager.addFile(file)
            close()
        }
    }

    property bool _fitMapOnSourceAdded: false

    // Shows zones added by the user when they are outside the map view
    Connections {
        target: _geoZoneManager
        function onSourceAdded(source) {
            if (!root._fitMapOnSourceAdded) {
                return
            }
            root._fitMapOnSourceAdded = false
            var mapRegion = flightMap.visibleRegion.boundingGeoRectangle()
            if (source.boundingBox.isValid && !mapRegion.intersects(source.boundingBox)) {
                flightMap.fitViewportToGeoShape(source.boundingBox, ScreenTools.defaultFontPixelHeight)
            }
        }
    }
}
