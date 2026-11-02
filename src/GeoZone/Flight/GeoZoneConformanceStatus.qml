/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette

/// Conformance of the active vehicle to the operational intent of its mission, see GeoZoneConformance, shown while the
/// planned flight has volumes
QGCLabel {
    wrapMode:   Text.WordWrap
    color:      GeoZoneConformance.conforming ? qgcPal.text : qgcPal.colorOrange
    font.bold:  !GeoZoneConformance.conforming
    text:       GeoZoneConformance.stateText
    visible:    GeoZoneConformance.active

    QGCPalette { id: qgcPal }
}
