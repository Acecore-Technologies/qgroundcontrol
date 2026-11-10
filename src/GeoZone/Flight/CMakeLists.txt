# Fly View: the monitors of the vehicles and their alerts, the checks of guided commands, the conformance, the flight
# record, the preflight check and the Remote ID
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZoneAlertArbiter.cc
        GeoZoneAlertArbiter.h
        GeoZoneAlertHistory.cc
        GeoZoneAlertHistory.h
        GeoZoneAlertModel.cc
        GeoZoneAlertModel.h
        GeoZoneConformance.cc
        GeoZoneConformance.h
        GeoZoneFlightRecorder.cc
        GeoZoneFlightRecorder.h
        GeoZoneGuidedCheck.cc
        GeoZoneGuidedCheck.h
        GeoZoneIntendedLevel.h
        GeoZoneMonitor.cc
        GeoZoneMonitor.h
        GeoZonePreflightCheck.cc
        GeoZonePreflightCheck.h
        GeoZoneRemoteId.cc
        GeoZoneRemoteId.h
        GeoZoneVehicleMonitor.cc
        GeoZoneVehicleMonitor.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
