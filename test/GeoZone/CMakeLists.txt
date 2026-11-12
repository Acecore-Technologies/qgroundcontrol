# Builds without geozones (QGC_DISABLE_GEOZONE) use the stub of the QML module, which must declare what the QML of
# QGroundControl uses of the module: checked in all builds, so that a new hook can't break only those builds. They have
# none of the other tests of the module.
add_test(NAME GeoZoneStubCheck COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/GeoZoneStubCheck.cmake)
set_tests_properties(GeoZoneStubCheck PROPERTIES LABELS "geozone;core")
if(QGC_DISABLE_GEOZONE)
    return()
endif()

# The check of the built-in feed catalogue (GEOZONE_FEEDS_FILE) at configuration accepts the built-in catalogue and
# refuses catalogues which QGroundControl couldn't read
add_test(NAME GeoZoneFeedsCheck COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/GeoZoneFeedsCheck
                                        -P ${CMAKE_CURRENT_SOURCE_DIR}/GeoZoneFeedsCheckTest.cmake)
set_tests_properties(GeoZoneFeedsCheck PROPERTIES LABELS "geozone;core")

# The geozone test classes, each in <Class>.cc and <Class>.h, which registers itself with GEOZONE_TEST(<Class>), see
# GeoZoneTestBase.h. These lists are the only list of them: they choose where each class is built and runs, and each
# class is a CTest test labeled geozone.
#   GEOZONE_SHARED_TESTS run in QGroundControl and headless in GeoZoneCoreTest, which leaves out their parts needing
#                        the application (GEOZONE_CORE_TEST)
#   GEOZONE_CORE_TESTS   only need the GeoZoneCore library and only run headless
#   GEOZONE_APP_TESTS    need the application and only run in QGroundControl
# The tests in QGroundControl are labeled app and named after their class, like GeoZoneParserTest, the headless ones are
# labeled core and named GeoZoneCore.<Class>.
set(GEOZONE_SHARED_TESTS
    GeoZoneGeometryTest
    GeoZoneIntentTest
    GeoZoneMissionPathTest
    GeoZoneParserTest
    GeoZoneReturnTest
)
set(GEOZONE_CORE_TESTS
    GeoZoneBriefingTest
    GeoZoneEvaluatorTest
    GeoZoneRouteTest
)
set(GEOZONE_APP_TESTS
    GeoZoneFenceImportTest
    GeoZoneFlightTest
    GeoZoneManagerTest
    GeoZoneMonitorTest
    GeoZonePackTest
    GeoZonePlanningTest
    GeoZoneRepositoryTest
    GeoZoneRoutePlannerTest
    GeoZoneServiceTest
)

target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZoneTestBase.h
        GeoZoneTestHelpers.cc
        GeoZoneTestHelpers.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Like add_qgc_test(), with the application on the offscreen platform. The tests share the settings of the application,
# so they don't run in parallel.
foreach(test IN LISTS GEOZONE_SHARED_TESTS GEOZONE_APP_TESTS)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${test}.cc ${test}.h)
    add_qgc_test(${test})
    set_tests_properties(${test} PROPERTIES
        LABELS "geozone;app"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        RESOURCE_LOCK geozone_app
    )
endforeach()

set(GEOZONE_TEST_DATA
    data/at_ed269_test.json
    data/ch_zurich_ed269.json
    data/dipul_test.json
    data/ed269_test.json
    data/ed318_test.json
    data/es_enaire_ed318_test.json
    data/es_servais_ed318_test.json
    data/faa_airspace_test.json
    data/faa_tfr_test.json
    data/france_uas_test.json
    data/openaip_test.json
    data/uk_aixm_test.xml
)

add_subdirectory(tools)

qt_add_resources(${CMAKE_PROJECT_NAME} "geozone_test_resources"
    PREFIX "/unittest"
    BASE data
    FILES ${GEOZONE_TEST_DATA}
)

# The shared and core tests run headless, linked with the GeoZoneCore library only
qt_add_executable(GeoZoneCoreTest
    GeoZoneCoreTestMain.cc
    GeoZoneTestBase.h
    GeoZoneTestHelpers.cc
    GeoZoneTestHelpers.h
)

# The helpers have a QObject for the tests in QGroundControl only, which the headless targets of this directory leave
# out with GEOZONE_CORE_TEST: moc has nothing to generate for them
set_source_files_properties(GeoZoneTestHelpers.h PROPERTIES SKIP_AUTOMOC ON)

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

# A test per class, which GeoZoneCoreTest runs alone when named as its first argument
foreach(test IN LISTS GEOZONE_SHARED_TESTS GEOZONE_CORE_TESTS)
    target_sources(GeoZoneCoreTest PRIVATE ${test}.cc ${test}.h)
    add_test(NAME GeoZoneCore.${test} COMMAND GeoZoneCoreTest ${test})
    set_tests_properties(GeoZoneCore.${test} PROPERTIES LABELS "geozone;core" RESOURCE_LOCK geozone_core)
endforeach()
add_dependencies(check GeoZoneCoreTest)

# Benchmarks of the core with 10000 zones, a test of their own labeled benchmark which takes several seconds
qt_add_executable(GeoZoneBenchmark
    GeoZoneBenchmark.cc
    GeoZoneBenchmark.h
    GeoZoneTestHelpers.cc
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
set_tests_properties(GeoZoneBenchmark PROPERTIES LABELS "geozone;benchmark")

option(GEOZONE_FUZZ "Build the fuzz targets of the geozone parsers, libFuzzer targets with clang" OFF)
if(GEOZONE_FUZZ)
    add_subdirectory(fuzz)
endif()
