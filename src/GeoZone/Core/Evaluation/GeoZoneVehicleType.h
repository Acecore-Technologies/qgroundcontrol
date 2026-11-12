/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

/// The autopilot firmware and the type of a vehicle, as far as its parameters, the way it returns, its fence actions
/// and its flight modes depend on them. The application tells them from its vehicle, see
/// GeoZoneMissionReader::returnParameters and GeoZoneFenceWarning::vehicleConfig.
namespace GeoZoneVehicleType {

enum class Firmware {
    Other,
    PX4,
    ArduPilot,
};

enum class VehicleClass {
    Other,          ///< Airships and the types the autopilot doesn't tell
    Multirotor,     ///< Multirotors and helicopters
    FixedWing,
    Vtol,
    Rover,          ///< Rovers and boats
    Sub,
};

} // namespace GeoZoneVehicleType
