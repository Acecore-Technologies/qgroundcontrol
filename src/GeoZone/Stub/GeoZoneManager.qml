/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

pragma Singleton

import QtQml

/// Stub of builds without geozones, see CMakeLists.txt: the Geozones page of the Application Settings is hidden
QtObject {
    readonly property QtObject settings: QtObject {
        readonly property bool visible: false
    }
}
