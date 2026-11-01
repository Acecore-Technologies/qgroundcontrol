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
        GeoZoneTestBase.h
        GeoZoneTestHelpers.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set(GEOZONE_TEST_DATA
    data/ch_zurich_ed269.json
    data/dipul_test.json
    data/ed269_test.json
    data/ed318_test.json
    data/faa_tfr_test.json
    data/france_uas_test.json
    data/uk_aixm_test.xml
)

qt_add_resources(${CMAKE_PROJECT_NAME} "geozone_test_resources"
    PREFIX "/unittest"
    BASE data
    FILES ${GEOZONE_TEST_DATA}
)

# The tests of the parsers and the geometry also run headless, linked with the GeoZoneCore library only
qt_add_executable(GeoZoneCoreTest
    GeoZoneCoreTestMain.cc
    GeoZoneGeometryTest.cc
    GeoZoneGeometryTest.h
    GeoZoneParserTest.cc
    GeoZoneParserTest.h
    GeoZoneTestBase.h
    GeoZoneTestHelpers.h
)

target_compile_definitions(GeoZoneCoreTest PRIVATE GEOZONE_CORE_TEST)
target_link_libraries(GeoZoneCoreTest
    PRIVATE
        GeoZoneCore
        Qt6::Concurrent
        Qt6::Test
        ZLIB::ZLIBSTATIC
)
set_target_properties(GeoZoneCoreTest PROPERTIES
    MACOSX_BUNDLE FALSE
    WIN32_EXECUTABLE FALSE
    QT_EXCLUDE_FROM_TRANSLATION ON
)

qt_add_resources(GeoZoneCoreTest "geozone_core_test_resources"
    PREFIX "/unittest"
    BASE data
    FILES ${GEOZONE_TEST_DATA}
)

add_test(NAME GeoZoneCoreTest COMMAND GeoZoneCoreTest)
add_dependencies(check GeoZoneCoreTest)
