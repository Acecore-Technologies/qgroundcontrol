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
/// The pilot can accept a warning by clicking it, not a failure. The warning accepted is the one shown: when its text
/// changes, it is to be accepted again.
PreFlightCheckButton {
    property var item: ({})   ///< state and text, see GeoZonePreflightCheck::Item

    readonly property int       _state:         item.state !== undefined ? item.state : GeoZonePreflightCheck.Checking
    readonly property string    _itemText:      item.text || ""
    readonly property bool      _accepted:      telemetryFailure && _telemetryState === _statePassed
    property string             _acceptedText:  ""  ///< Text of the warning the pilot accepted, empty without

    telemetryFailure:               _state === GeoZonePreflightCheck.Checking || _state === GeoZonePreflightCheck.Warning || _state === GeoZonePreflightCheck.Failed
    telemetryTextFailure:           _itemText
    allowTelemetryFailureOverride:  _state === GeoZonePreflightCheck.Warning

    on_AcceptedChanged: _acceptedText = _accepted ? _itemText : ""
    on_ItemTextChanged: {
        if (_accepted && _itemText !== _acceptedText) {
            _updateTelemetryState()
        }
    }

    // The texts name zones of external data, which mustn't be read as markup
    _text: "<b>" + name + "</b>: " + _itemText.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;") + (_accepted ? qsTr(" (accepted)") : "")
}
