import QtQuick
import QtQuick.Layouts
import QGroundControl.GeoZone 1.0 as GZ

// Uses of the stub module which the checks accept: members the stubs declare, members of their base types, and
// enumeration values, through the qualifier of the import
Item {
    property bool settingsVisible: GZ.GeoZoneManager.settings.visible
    property int fast: GZ.GeoZoneThing.Fast
    property int slow: GZ.GeoZoneThing.Mode.Slow

    GZ.GeoZoneThing {
        id:                 thing
        anchors.fill:       parent
        Layout.fillWidth:   true
        visible:            false
        map:                null
        onDone:             thing.run()
        onMapChanged:       console.log(thing.width)
    }

    GZ.GeoZonePart { id: part; count: 2; objectName: "part" }

    function total() {
        return part.count
    }
}
