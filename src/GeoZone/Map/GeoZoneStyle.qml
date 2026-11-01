/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

pragma Singleton

import QtQuick

import QGroundControl.GeoZone

/// Colors of the geozones on the maps, shared by the map visuals, legend and indicator
QtObject {
    readonly property color prohibitedColor:        "red"
    readonly property color reqAuthorisationColor:  "orange"
    readonly property color conditionalColor:       "yellow"
    readonly property color noRestrictionColor:     "limegreen"
    readonly property color uSpaceColor:            "deepskyblue"
    readonly property color unknownColor:           "gray"
    readonly property color highlightColor:         "white"    ///< Border of zones crossed by the mission

    readonly property real activeFillAlpha:     0.25
    readonly property real inactiveFillAlpha:   0.08    ///< Zones not active at the current time

    function restrictionColor(restriction) {
        switch (restriction) {
        case GeoZone.Prohibited:        return prohibitedColor
        case GeoZone.ReqAuthorisation:  return reqAuthorisationColor
        case GeoZone.Conditional:       return conditionalColor
        case GeoZone.NoRestriction:     return noRestrictionColor
        case GeoZone.USpace:            return uSpaceColor
        default:                        return unknownColor
        }
    }

    function fillColor(restriction, active) {
        const color = restrictionColor(restriction)
        return Qt.rgba(color.r, color.g, color.b, active ? activeFillAlpha : inactiveFillAlpha)
    }
}
