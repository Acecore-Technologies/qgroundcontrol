target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZoneFlightTest.cc
        GeoZoneFlightTest.h
        GeoZoneGeometryTest.cc
        GeoZoneGeometryTest.h
        GeoZoneManagerTest.cc
        GeoZoneManagerTest.h
        GeoZoneMonitorTest.cc
        GeoZoneMonitorTest.h
        GeoZoneParserTest.cc
        GeoZoneParserTest.h
        GeoZonePlanningTest.cc
        GeoZonePlanningTest.h
        GeoZoneTestHelpers.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
