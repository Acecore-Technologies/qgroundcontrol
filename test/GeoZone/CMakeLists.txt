# The geozone test classes, each in <Class>.cc and <Class>.h. These lists are the only list of them: the classes are
# registered with QGroundControl's UnitTest (GeoZoneUnitTests.cc) and run by GeoZoneCoreTest (GeoZoneCoreTestMain.cc)
# through the headers generated from them, and each is a CTest test labeled geozone.
#   GEOZONE_SHARED_TESTS run in QGroundControl and headless in GeoZoneCoreTest, which leaves out their parts needing
#                        the application (GEOZONE_CORE_TEST, see GeoZoneTestBase.h)
#   GEOZONE_CORE_TESTS   only need the GeoZoneCore library and only run headless
#   GEOZONE_APP_TESTS    need the application and only run in QGroundControl
# The tests in QGroundControl are labeled app and named after their class, like GeoZoneParserTest, the headless ones are
# labeled core and named GeoZoneCore.<Class>.
set(GEOZONE_SHARED_TESTS
    GeoZoneGeometryTest
    GeoZoneIntentTest
    GeoZoneParserTest
    GeoZoneReturnTest
)
set(GEOZONE_CORE_TESTS
    GeoZoneBriefingTest
    GeoZoneEvaluatorTest
    GeoZoneRouteTest
)
set(GEOZONE_APP_TESTS
    GeoZoneFlightTest
    GeoZoneManagerTest
    GeoZoneMonitorTest
    GeoZonePlanningTest
)

# Writes a header including the classes, whose macro calls X(Class) for each
function(geozone_test_list header macro)
    set(includes "")
    set(entries "")
    foreach(test IN LISTS ARGN)
        string(APPEND includes "#include \"${test}.h\"\n")
        string(APPEND entries " \\\n    X(${test})")
    endforeach()
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/${header} CONTENT
"// Generated from the lists of test/GeoZone/CMakeLists.txt, edit them instead
#pragma once

${includes}
#define ${macro}(X)${entries}
")
endfunction()

# @return The sources of the classes
function(geozone_test_sources variable)
    set(sources "")
    foreach(test IN LISTS ARGN)
        list(APPEND sources ${test}.cc ${test}.h)
    endforeach()
    set(${variable} ${sources} PARENT_SCOPE)
endfunction()

geozone_test_list(GeoZoneAppTests.h GEOZONE_APP_TESTS ${GEOZONE_SHARED_TESTS} ${GEOZONE_APP_TESTS})
geozone_test_list(GeoZoneCoreTests.h GEOZONE_CORE_TESTS ${GEOZONE_SHARED_TESTS} ${GEOZONE_CORE_TESTS})
geozone_test_sources(GEOZONE_APP_TEST_SOURCES ${GEOZONE_SHARED_TESTS} ${GEOZONE_APP_TESTS})
geozone_test_sources(GEOZONE_CORE_TEST_SOURCES ${GEOZONE_SHARED_TESTS} ${GEOZONE_CORE_TESTS})

target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        ${GEOZONE_APP_TEST_SOURCES}
        ${CMAKE_CURRENT_BINARY_DIR}/generated/GeoZoneAppTests.h
        GeoZoneTestBase.h
        GeoZoneTestHelpers.h
        GeoZoneUnitTests.cc
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Like add_qgc_test(), with the application on the offscreen platform. The tests share the settings of the application,
# so they don't run in parallel.
foreach(test IN LISTS GEOZONE_SHARED_TESTS GEOZONE_APP_TESTS)
    add_qgc_test(${test})
    set_tests_properties(${test} PROPERTIES
        LABELS "geozone;app"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        RESOURCE_LOCK geozone_app
    )
endforeach()

set(GEOZONE_TEST_DATA
    data/ch_zurich_ed269.json
    data/dipul_test.json
    data/ed269_test.json
    data/ed318_test.json
    data/faa_tfr_test.json
    data/france_uas_test.json
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
    ${GEOZONE_CORE_TEST_SOURCES}
    ${CMAKE_CURRENT_BINARY_DIR}/generated/GeoZoneCoreTests.h
    GeoZoneCoreTestMain.cc
    GeoZoneTestBase.h
    GeoZoneTestHelpers.h
)

target_compile_definitions(GeoZoneCoreTest PRIVATE GEOZONE_CORE_TEST)
target_include_directories(GeoZoneCoreTest PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
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
    add_test(NAME GeoZoneCore.${test} COMMAND GeoZoneCoreTest ${test})
    set_tests_properties(GeoZoneCore.${test} PROPERTIES LABELS "geozone;core" RESOURCE_LOCK geozone_core)
endforeach()
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
set_tests_properties(GeoZoneBenchmark PROPERTIES LABELS "geozone;benchmark")

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
    set_tests_properties(tst_geozone_qml PROPERTIES LABELS "geozone;qml")
    add_dependencies(check tst_geozone_qml)
endif()

option(GEOZONE_FUZZ "Build the fuzz targets of the geozone parsers, libFuzzer targets with clang" OFF)
if(GEOZONE_FUZZ)
    add_subdirectory(fuzz)
endif()
