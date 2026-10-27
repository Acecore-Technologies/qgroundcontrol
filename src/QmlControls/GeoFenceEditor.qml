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

    readonly property real  _editFieldWidth:    Math.min(width - _margin * 2, ScreenTools.defaultFontPixelWidth * 15)
    readonly property real  _margin:            ScreenTools.defaultFontPixelWidth / 2
    readonly property real  _radius:            ScreenTools.defaultFontPixelWidth / 2
    readonly property var   _geoZoneManager:    QGroundControl.geoZoneManager
    readonly property bool  _geoZonesLoaded:    _geoZoneManager.zones.count > 0

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
                        text:           qsTr("Polygon Fences")
                    }

                    QGCLabel {
                        text:       qsTr("None")
                        visible:    polygonSection.checked && myGeoFenceController.polygons.count === 0
                    }

                    GridLayout {
                        Layout.fillWidth:   true
                        columns:            3
                        flow:               GridLayout.TopToBottom
                        visible:            polygonSection.checked && myGeoFenceController.polygons.count > 0

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

                    SectionHeader {
                        id:             circleSection
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        text:           qsTr("Circular Fences")
                    }

                    QGCLabel {
                        text:       qsTr("None")
                        visible:    circleSection.checked && myGeoFenceController.circles.count === 0
                    }

                    GridLayout {
                        anchors.left:       parent.left
                        anchors.right:      parent.right
                        columns:            4
                        flow:               GridLayout.TopToBottom
                        visible:            polygonSection.checked && myGeoFenceController.circles.count > 0

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
                        text:           _geoZonesLoaded ?
                                            qsTr("%1 zones loaded from %2 file: %3").arg(_geoZoneManager.zones.count).arg(_geoZoneManager.formatName).arg(_geoZoneManager.sourceName) :
                                            qsTr("Load an EUROCAE ED-269 or ED-318 file to show UAS geographical zones on the map.")
                    }

                    QGCLabel {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        wrapMode:       Text.WordWrap
                        font.pointSize: ScreenTools.smallFontPointSize
                        text:           qsTr("%1 features without a supported geometry were skipped.").arg(_geoZoneManager.skippedCount)
                        visible:        _geoZonesLoaded && _geoZoneManager.skippedCount > 0
                    }

                    QGCButton {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        text:           qsTr("Load Geozone File...")
                        onClicked:      geoZoneFileDialog.openForLoad()
                    }

                    QGCButton {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        text:           qsTr("Remove Geozones")
                        visible:        _geoZonesLoaded
                        onClicked:      _geoZoneManager.clear()
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
                        }

                        QGCCheckBox {
                            id:         importReqAuthorisationCheck
                            text:       qsTr("Authorisation required")
                            checked:    true
                        }

                        QGCCheckBox {
                            id:         importConditionalCheck
                            text:       qsTr("Conditional")
                            checked:    false
                        }

                        QGCCheckBox {
                            id:         importMapViewOnlyCheck
                            text:       qsTr("Only zones in current map view")
                            checked:    true
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

    function _importGeoZones() {
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
        var area = importMapViewOnlyCheck.checked ? flightMap.visibleRegion.boundingGeoRectangle() : QtPositioning.rectangle()
        var fenceCount = myGeoFenceController.importGeoZones(restrictions, area)
        mainWindow.showMessageDialog(qsTr("Geozones"),
                                     fenceCount > 0 ?
                                         qsTr("%1 exclusion fences added.").arg(fenceCount) :
                                         qsTr("No new matching zones found."))
    }

    QGCFileDialog {
        id:             geoZoneFileDialog
        folder:         QGroundControl.settingsManager.appSettings.missionSavePath
        title:          qsTr("Select Geozone File")
        nameFilters:    _geoZoneManager.fileDialogFilters

        onAcceptedForLoad: (file) => {
            if (_geoZoneManager.loadFile(file)) {
                var mapRegion = flightMap.visibleRegion.boundingGeoRectangle()
                if (!mapRegion.intersects(_geoZoneManager.boundingBox)) {
                    flightMap.fitViewportToGeoShape(_geoZoneManager.boundingBox, ScreenTools.defaultFontPixelHeight)
                }
            }
            close()
        }
    }
}
