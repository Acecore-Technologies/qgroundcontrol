import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtPositioning

import QGroundControl
import QGroundControl.ScreenTools
import QGroundControl.Controls
import QGroundControl.FactSystem
import QGroundControl.FactControls
import QGroundControl.GeoZone

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
                    text:           qsTr("Geozones")
                }

                GeoZoneFencePanel {
                    anchors.left:       parent.left
                    anchors.right:      parent.right
                    visible:            geoZoneSection.checked
                    geoFenceController: myGeoFenceController
                    flightMap:          root.flightMap
                    hiddenZoneCount:    root.geoZonesHiddenCount
                }
            }
        }
    } // Rectangle
}
