pragma Singleton

import QtQml

QtObject {
    readonly property QtObject settings: QtObject {
        readonly property bool visible: false
    }
}
