# Fly View: the monitors of the vehicles and their alerts, the checks of guided commands, the conformance, the flight
# record, the preflight check and the Remote ID
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZoneAlertArbiter.cc
        GeoZoneAlertArbiter.h
        GeoZoneAlertHistory.cc
        GeoZoneAlertHistory.h
        GeoZoneAlertLevel.cc
        GeoZoneAlertLevel.h
        GeoZoneAlertModel.cc
        GeoZoneAlertModel.h
        GeoZoneAlertText.cc
        GeoZoneAlertText.h
        GeoZoneAwareness.cc
        GeoZoneAwareness.h
        GeoZoneConformance.cc
        GeoZoneConformance.h
        GeoZoneFenceWarning.cc
        GeoZoneFenceWarning.h
        GeoZoneFlightRecorder.cc
        GeoZoneFlightRecorder.h
        GeoZoneGuidedCheck.cc
        GeoZoneGuidedCheck.h
        GeoZoneMonitor.cc
        GeoZoneMonitor.h
        GeoZoneMonitorPermits.cc
        GeoZonePermits.cc
        GeoZonePermits.h
        GeoZonePreflightCheck.cc
        GeoZonePreflightCheck.h
        GeoZoneRemoteId.cc
        GeoZoneRemoteId.h
        GeoZoneTrafficMonitor.cc
        GeoZoneTrafficMonitor.h
        GeoZoneVehicleMonitor.cc
        GeoZoneVehicleMonitor.h
        GeoZoneVehicleThresholds.cc
        GeoZoneVehicleThresholds.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
