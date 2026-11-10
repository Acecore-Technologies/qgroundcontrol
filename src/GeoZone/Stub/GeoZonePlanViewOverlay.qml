/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick

/// Stub of builds without geozones, see CMakeLists.txt: map clicks show nothing and missions upload unchecked
Item {
    property var    map
    property var    planMasterController
    property real   leftLimit:  0
    property real   rightLimit: 0

    visible: false

    function showZonesAt(coordinate) {
        return false
    }

    function selectFenceAt(coordinate) {
        return false
    }

    function confirmUpload(uploadFunction) {
        uploadFunction()
    }
}
