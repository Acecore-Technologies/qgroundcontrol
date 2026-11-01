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

qt_add_resources(${CMAKE_PROJECT_NAME} "geozone_test_resources"
    PREFIX "/unittest"
    FILES
        ch_zurich_ed269.json
        dipul_test.json
        ed269_test.json
        ed318_test.json
        faa_tfr_test.json
        france_uas_test.json
        uk_aixm_test.xml
)
