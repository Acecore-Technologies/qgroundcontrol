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

import QGroundControl
import QGroundControl.GeoZone

/// Colors of the geozones on the maps and texts of distances and heights, shared by the geozone views
QtObject {
    readonly property color prohibitedColor:        "red"
    readonly property color reqAuthorisationColor:  "orange"
    readonly property color conditionalColor:       "yellow"
    readonly property color noRestrictionColor:     "limegreen"
    readonly property color uSpaceColor:            "deepskyblue"
    readonly property color unknownColor:           "gray"
    readonly property color highlightColor:         "white"    ///< Border of zones crossed by the mission

    readonly property color terrainColor:           Qt.rgba(0.45, 0.35, 0.2, 0.8)  ///< Ground below altitude scales

    // Alert levels after EN 4709-003 Annex D, see GeoZoneAlertLevel, readable on the dark alert popup and the map
    readonly property color warningColor:   "#ff3b30"   ///< Red
    readonly property color cautionColor:   "#ffb000"   ///< Amber
    readonly property color advisoryColor:  "#b0bec5"   ///< Neutral

    readonly property real activeFillAlpha:     0.25
    readonly property real inactiveFillAlpha:   0.08    ///< Zones not active at the current time

    /// @return Color of a GeoZoneAlertLevel, the default color when it is undefined or -1
    function levelColor(level, defaultColor) {
        switch (level) {
        case GeoZoneAlertLevel.Warning:     return warningColor
        case GeoZoneAlertLevel.Caution:     return cautionColor
        case GeoZoneAlertLevel.Advisory:    return advisoryColor
        default:                            return defaultColor === undefined ? "transparent" : defaultColor
        }
    }

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

    /// @return Letter of the restriction, shown with its color so that the color isn't its only cue
    function restrictionLetter(restriction) {
        switch (restriction) {
        case GeoZone.Prohibited:        return qsTr("P", "Prohibited zone")
        case GeoZone.ReqAuthorisation:  return qsTr("A", "Zone requiring an authorisation")
        case GeoZone.Conditional:       return qsTr("C", "Conditional zone")
        case GeoZone.NoRestriction:     return "\u2713"
        case GeoZone.USpace:            return qsTr("U", "U-space zone")
        default:                        return "?"
        }
    }

    /// @return Color of texts on the restriction color, like the letter of a badge
    function restrictionTextColor(restriction) {
        return (restriction === GeoZone.Prohibited || restriction === GeoZone.Unknown || restriction === undefined) ? "white" : "black"
    }

    function fillColor(restriction, active) {
        const color = restrictionColor(restriction)
        return Qt.rgba(color.r, color.g, color.b, active ? activeFillAlpha : inactiveFillAlpha)
    }

    /// @return Horizontal distance in meters as rounded text in the distance units of the settings, like "120 m"
    function horizontalText(meters) {
        if (isNaN(meters)) {
            return ""
        }
        return Math.round(QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnits(meters)) + " " +
               QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString
    }

    /// @return Height or altitude in meters as rounded text in the height units of the settings, followed by the
    ///         reference when given, like "120 m" or "450 m AMSL"
    function verticalText(meters, reference) {
        if (isNaN(meters)) {
            return ""
        }
        const text = Math.round(QGroundControl.unitsConversion.metersToAppSettingsVerticalDistanceUnits(meters)) + " " +
                     QGroundControl.unitsConversion.appSettingsVerticalDistanceUnitsString
        return reference ? text + " " + reference : text
    }
}
