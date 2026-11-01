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

import QGroundControl.Controls

/// Button of the panel FlyViewMap.qml opens at map clicks, which shows the geozones at the clicked location
QGCButton {
    Layout.fillWidth:   true
    text:               qsTr("Geozones here (%1)").arg(_zoneCount)
    visible:            _zoneCount > 0

    required property var mapLayer      ///< GeoZoneFlyMapLayer
    required property var coordinate    ///< Clicked location
    required property var dropPanel     ///< Panel with the button, closed when clicked

    readonly property int _zoneCount: mapLayer.zoneCountAt(coordinate)

    onClicked: {
        dropPanel.close()
        mapLayer.showZonesAt(coordinate)
    }
}
