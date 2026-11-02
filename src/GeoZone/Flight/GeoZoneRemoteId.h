/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QString>

class Vehicle;

/// The Remote ID of QGroundControl's Remote ID settings and of the vehicle's RemoteIDManager, which flights in U-space
/// airspace need: for the pre-flight checklist and the mission check
namespace GeoZoneRemoteId {

/// Whether the Remote ID is ready for a flight, see RemoteIDManager
struct Status {
    bool vehicle = false;           ///< A vehicle is connected, otherwise only the operator ID is known
    bool available = false;         ///< The vehicle sends OPEN_DRONE_ID_ARM_STATUS
    bool commsGood = false;         ///< It sent it recently
    bool armStatusGood = false;     ///< The Remote ID is ready to arm
    QString armStatusError;         ///< Why it isn't, as the Remote ID reports
    bool operatorIdGood = false;    ///< The operator ID is set, and valid in the EU
};

/// @return Status of the vehicle's Remote ID and the operator ID of the settings, only the latter without vehicle
Status status(Vehicle *vehicle);

/// @return Why the Remote ID isn't ready for a flight in U-space, empty when it is. Without a vehicle, only the
///         operator ID is checked.
QString problem(const Status &status);

} // namespace GeoZoneRemoteId
