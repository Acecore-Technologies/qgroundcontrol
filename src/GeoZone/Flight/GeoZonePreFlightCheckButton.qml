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

/// Automatic check of the pre-flight checklist showing an item of GeoZonePreflightCheck: its text whatever its state.
/// The pilot can accept a warning by clicking it, not a failure.
PreFlightCheckButton {
    property var item: ({})   ///< state and text, see GeoZonePreflightCheck::Item

    readonly property int _state: item.state !== undefined ? item.state : GeoZonePreflightCheck.Checking

    telemetryFailure:               _state === GeoZonePreflightCheck.Checking || _state === GeoZonePreflightCheck.Warning || _state === GeoZonePreflightCheck.Failed
    telemetryTextFailure:           item.text || ""
    allowTelemetryFailureOverride:  _state === GeoZonePreflightCheck.Warning

    _text: "<b>" + name + "</b>: " + (item.text || "") + ((telemetryFailure && _telemetryState === _statePassed) ? qsTr(" (accepted)") : "")
}
