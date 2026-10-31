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
import QGroundControl.GeoZone
import QGroundControl.ScreenTools
import QGroundControl.Palette

/// Shows whether the active vehicle is in or near restricted geozones
Item {
    id:             control
    width:          geoZoneIcon.width * 1.1
    anchors.top:    parent.top
    anchors.bottom: parent.bottom

    property bool showIndicator: _monitor.monitoring

    readonly property var _monitor: QGroundControl.geoZoneMonitor

    function _restrictionColor(restriction) {
        switch (restriction) {
        case GeoZone.Prohibited:        return qgcPal.colorRed
        case GeoZone.ReqAuthorisation:  return qgcPal.colorOrange
        case GeoZone.Conditional:       return qgcPal.colorYellow
        case GeoZone.USpace:            return qgcPal.colorBlue
        default:                        return qgcPal.text
        }
    }

    function _distanceText(meters) {
        return Math.round(QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnits(meters)) + " " +
                QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString
    }

    QGCColoredImage {
        id:                 geoZoneIcon
        width:              height
        anchors.top:        parent.top
        anchors.bottom:     parent.bottom
        source:             "qrc:/qml/QGroundControl/GeoZone/GeoZone.svg"
        fillMode:           Image.PreserveAspectFit
        sourceSize.height:  height
        color:              control._restrictionColor(_monitor.alertRestriction)
    }

    MouseArea {
        anchors.fill:   parent
        onClicked:      mainWindow.showIndicatorDrawer(geoZonePage, control)
    }

    Component {
        id: geoZonePage

        ToolIndicatorPage {
            contentComponent: Component {
                ColumnLayout {
                    spacing: ScreenTools.defaultFontPixelHeight / 2

                    SettingsGroupLayout {
                        heading:            qsTr("Geozones")
                        headingDescription: qsTr("Active prohibited, authorisation required, conditional and U-space zones within %1, considering altitude limits.").arg(control._distanceText(QGroundControl.settingsManager.geoZoneSettings.approachDistance.rawValue))

                        QGCLabel {
                            text:       qsTr("No zones nearby")
                            visible:    _monitor.nearbyZones.length === 0
                        }

                        Repeater {
                            model: _monitor.nearbyZones

                            RowLayout {
                                spacing: ScreenTools.defaultFontPixelWidth

                                Rectangle {
                                    width:  ScreenTools.defaultFontPixelHeight * 0.75
                                    height: width
                                    radius: width / 2
                                    color:  control._restrictionColor(modelData.zone.restriction)
                                }

                                QGCLabel {
                                    Layout.fillWidth:   true
                                    text:               modelData.zone.name !== "" ? modelData.zone.name : modelData.zone.identifier
                                }

                                QGCLabel {
                                    text: modelData.inside ? qsTr("Inside") : control._distanceText(modelData.distance)
                                }

                                QGCLabel {
                                    font.pointSize: ScreenTools.smallFontPointSize
                                    text:           modelData.zone.restrictionText + "\n" + modelData.zone.verticalLimitsText
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
