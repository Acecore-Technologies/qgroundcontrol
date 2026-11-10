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

/// Toolbar icon showing whether the active vehicle is in or near restricted geozones, colored by its alert level
/// (GeoZoneMonitor.activeLevel: red warning, amber caution, neutral advisory), with badges counting the unacknowledged
/// alerts, marking the nearby zones stale while the vehicle's position is lost, warning that the vehicle is on the
/// ground inside a restricted zone and that geo-awareness is degraded or unavailable, like with outdated data.
/// Clicking it opens GeoZoneIndicatorPage.
Item {
    id:             control
    width:          geoZoneIcon.width * 1.1
    anchors.top:    parent.top
    anchors.bottom: parent.bottom

    // Also without zones while a vehicle is connected, to tell that geo-awareness is unavailable
    property bool showIndicator: GeoZoneManager.zoneCount > 0 || _outdated || !!QGroundControl.multiVehicleManager.activeVehicle

    readonly property var _monitor:         GeoZoneMonitor
    readonly property var _active:          GeoZoneMonitor.activeVehicle     ///< GeoZoneVehicleMonitor, null without
    readonly property var _history:         GeoZoneMonitor.alertHistory
    readonly property bool _stale:          _monitor.monitoring && _active?.positionUnavailable === true
    readonly property bool _outdated:       GeoZoneManager.repository.validityWarning !== ""
    readonly property int _groundLevel:     _monitor.monitoring && (_active?.groundWarning ?? "") !== "" ? _active.groundLevel : -1
    readonly property int _awareness:       _monitor.awareness.state

    QGCPalette { id: qgcPal }

    QGCColoredImage {
        id:                 geoZoneIcon
        width:              height
        anchors.top:        parent.top
        anchors.bottom:     parent.bottom
        source:             "qrc:/qml/QGroundControl/GeoZone/GeoZone.svg"
        fillMode:           Image.PreserveAspectFit
        sourceSize.height:  height
        color:              GeoZoneStyle.levelColor(control._monitor.activeLevel, qgcPal.text)
        opacity:            (control._monitor.monitoring && control._active?.positionUnavailable !== true && control._awareness !== GeoZoneAwareness.Unavailable) ? 1 : 0.5
    }

    component IconBadge: GeoZoneBadge {
        height:         ScreenTools.defaultFontPixelHeight * 0.9
        minimumWidth:   height
    }

    // The nearby zones are those of the last known position
    IconBadge {
        anchors.bottom: parent.bottom
        anchors.right:  geoZoneIcon.right
        text:           "?"
        color:          qgcPal.colorOrange
        visible:        control._stale
    }

    // Geo-awareness is degraded (orange), like with outdated data, or unavailable (red), like without zones or GNSS fix,
    // or the data is treated as invalid when outdated (red, its zones still apply), see GeoZoneSourceRepository::dataInvalid;
    // the page details them
    IconBadge {
        readonly property bool _red: control._awareness === GeoZoneAwareness.Unavailable || GeoZoneManager.repository.dataInvalid

        anchors.bottom: parent.bottom
        anchors.left:   geoZoneIcon.left
        text:           "!"
        textColor:      _red ? "white" : "black"
        color:          _red ? qgcPal.colorRed : qgcPal.colorOrange
        visible:        control._awareness !== GeoZoneAwareness.Available || control._outdated
    }

    // On the ground inside a prohibited zone (red), or a restricted zone without permission (orange), like a pre-arm
    // message; the page says which
    IconBadge {
        anchors.top:    parent.top
        anchors.left:   geoZoneIcon.left
        text:           "\u2715"
        textColor:      "white"
        color:          control._groundLevel === GeoZoneIntendedLevel.Warning ? qgcPal.colorRed : qgcPal.colorOrange
        visible:        control._groundLevel === GeoZoneIntendedLevel.Warning || control._groundLevel === GeoZoneIntendedLevel.Caution
    }

    // Alerts not acknowledged in the alert history
    IconBadge {
        anchors.top:    parent.top
        anchors.right:  geoZoneIcon.right
        text:           control._history.unacknowledgedCount
        textColor:      "white"
        color:          qgcPal.colorRed
        visible:        control._history.unacknowledgedCount > 0
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
