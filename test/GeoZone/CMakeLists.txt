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
        GeoZoneReturnTest.cc
        GeoZoneReturnTest.h
        GeoZoneTestBase.h
        GeoZoneTestHelpers.h
        GeoZoneUnitTests.cc
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Registered with UnitTest in GeoZoneUnitTests.cc
add_qgc_test(GeoZoneFlightTest)
add_qgc_test(GeoZoneGeometryTest)
add_qgc_test(GeoZoneManagerTest)
add_qgc_test(GeoZoneMonitorTest)
add_qgc_test(GeoZoneParserTest)
add_qgc_test(GeoZonePlanningTest)
add_qgc_test(GeoZoneReturnTest)

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

# The tests of the parsers, the geometry and the return model also run headless, linked with the GeoZoneCore library only
qt_add_executable(GeoZoneCoreTest
    GeoZoneCoreTestMain.cc
    GeoZoneGeometryTest.cc
    GeoZoneGeometryTest.h
    GeoZoneParserTest.cc
    GeoZoneParserTest.h
    GeoZoneReturnTest.cc
    GeoZoneReturnTest.h
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

# Benchmarks of the core with 10000 zones, a test of their own labeled benchmark which takes several seconds
qt_add_executable(GeoZoneBenchmark
    GeoZoneBenchmark.cc
    GeoZoneBenchmark.h
    GeoZoneTestHelpers.h
)

target_compile_definitions(GeoZoneBenchmark PRIVATE GEOZONE_CORE_TEST)
target_link_libraries(GeoZoneBenchmark
    PRIVATE
        GeoZoneCore
        Qt6::Concurrent
        Qt6::Test
)
set_target_properties(GeoZoneBenchmark PROPERTIES
    MACOSX_BUNDLE FALSE
    WIN32_EXECUTABLE FALSE
    QT_EXCLUDE_FROM_TRANSLATION ON
)

add_test(NAME GeoZoneBenchmark COMMAND GeoZoneBenchmark)
set_tests_properties(GeoZoneBenchmark PROPERTIES LABELS benchmark)

# Qt Quick tests of the components which don't need the application, run with the offscreen platform
find_package(Qt6 QUIET COMPONENTS QuickTest)
if(TARGET Qt6::QuickTest)
    qt_add_executable(tst_geozone_qml GeoZoneQmlTestMain.cc)
    target_compile_definitions(tst_geozone_qml PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(tst_geozone_qml
        PRIVATE
            GeoZoneCore
            Qt6::Qml
            Qt6::Quick
            Qt6::QuickTest
    )
    set_target_properties(tst_geozone_qml PROPERTIES
        MACOSX_BUNDLE FALSE
        WIN32_EXECUTABLE FALSE
        QT_EXCLUDE_FROM_TRANSLATION ON
    )
    qt_add_resources(tst_geozone_qml "geozone_qml_test_components"
        PREFIX "/geozone"
        BASE ${CMAKE_SOURCE_DIR}/src/GeoZone/Map
        FILES ${CMAKE_SOURCE_DIR}/src/GeoZone/Map/GeoZoneStyle.qml
    )
    add_test(NAME tst_geozone_qml COMMAND tst_geozone_qml -platform offscreen)
    add_dependencies(check tst_geozone_qml)
endif()

option(GEOZONE_FUZZ "Build the fuzz targets of the geozone parsers, libFuzzer targets with clang" OFF)
if(GEOZONE_FUZZ)
    add_subdirectory(fuzz)
endif()
