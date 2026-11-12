import QtQuick
import QGroundControl.GeoZone as GZ

// Uses of the stub module which the checks report, each on a line of its own
Item {
    property int restriction: GZ.GeoZone.Prohibited
    property bool absent: GZ.GeoZoneManager.settings.absent
    property int turbo: GZ.GeoZoneThing.Turbo

    GZ.GeoZoneThing {
        id:         thing
        text:       "a property of other base types than Item"
        missing:    1
        onGone:     thing.absentFunction()
    }

    GZ.GeoZonePart {
        visible:    false
    }

    GZ.GeoZoneUnknown { }
}
