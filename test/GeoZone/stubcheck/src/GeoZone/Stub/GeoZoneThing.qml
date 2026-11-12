import QtQuick

Item {
    enum Mode { Slow, Fast }

    property var map

    signal done

    function run() {
        return true
    }
}
