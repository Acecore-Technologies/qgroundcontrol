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
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.ScreenTools
import QGroundControl.Palette

/// Toolbar icon showing whether the active vehicle is in or near restricted geozones, colored by the most restrictive
/// zone, with badges counting the unacknowledged alerts and marking the nearby zones stale while the vehicle's position
/// is lost. Clicking it opens GeoZoneIndicatorPage.
Item {
    id:             control
    width:          geoZoneIcon.width * 1.1
    anchors.top:    parent.top
    anchors.bottom: parent.bottom

    property bool showIndicator: GeoZoneManager.zoneCount > 0

    readonly property var _monitor:         GeoZoneMonitor
    readonly property var _active:          GeoZoneMonitor.activeVehicle     ///< GeoZoneVehicleMonitor, null without
    readonly property var _history:         GeoZoneMonitor.alertHistory
    readonly property bool _stale:          _monitor.monitoring && _active?.positionUnavailable === true

    function _restrictionColor(restriction) {
        return restriction < 0 ? qgcPal.text : GeoZoneStyle.restrictionColor(restriction)
    }

    QGCPalette { id: qgcPal }

    QGCColoredImage {
        id:                 geoZoneIcon
        width:              height
        anchors.top:        parent.top
        anchors.bottom:     parent.bottom
        source:             "qrc:/qml/QGroundControl/GeoZone/GeoZone.svg"
        fillMode:           Image.PreserveAspectFit
        sourceSize.height:  height
        color:              control._active?.fenceBreached ? qgcPal.colorRed : control._restrictionColor(control._monitor.monitoring ? control._active?.alertRestriction ?? -1 : -1)
        opacity:            (control._monitor.monitoring && control._active?.positionUnavailable !== true) ? 1 : 0.5
    }

    // The nearby zones are those of the last known position
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.right:  geoZoneIcon.right
        height:         ScreenTools.defaultFontPixelHeight * 0.9
        width:          height
        radius:         height / 2
        color:          qgcPal.colorOrange
        visible:        control._stale

        QGCLabel {
            anchors.centerIn:   parent
            text:               "?"
            color:              "black"
            font.bold:          true
            font.pointSize:     ScreenTools.smallFontPointSize
        }
    }

    // Alerts not acknowledged in the alert history
    Rectangle {
        anchors.top:    parent.top
        anchors.right:  geoZoneIcon.right
        height:         ScreenTools.defaultFontPixelHeight * 0.9
        width:          Math.max(height, badgeLabel.contentWidth + (height / 2))
        radius:         height / 2
        color:          qgcPal.colorRed
        visible:        control._history.unacknowledgedCount > 0

        QGCLabel {
            id:                 badgeLabel
            anchors.centerIn:   parent
            text:               control._history.unacknowledgedCount
            color:              "white"
            font.pointSize:     ScreenTools.smallFontPointSize
        }
    }

    MouseArea {
        anchors.fill:   parent
        onClicked:      mainWindow.showIndicatorDrawer(geoZonePage, control)
    }

    Component {
        id: geoZonePage

        GeoZoneIndicatorPage { }
    }
}
