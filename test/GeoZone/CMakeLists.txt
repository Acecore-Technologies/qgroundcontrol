# Builds without geozones (QGC_DISABLE_GEOZONE) use the stub of the QML module, which must declare what the QML of
# QGroundControl uses of the module: checked in all builds, so that a new hook can't break only those builds. They have
# none of the other tests of the module.
add_test(NAME GeoZoneStubCheck COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/GeoZoneStubCheck.cmake)
set_tests_properties(GeoZoneStubCheck PROPERTIES LABELS "geozone;core")

# A map click shows the geozones in every layer of the Plan View, also the layers this build doesn't have
add_test(NAME GeoZonePlanViewClicksCheck COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/GeoZonePlanViewClicksCheck.cmake)
set_tests_properties(GeoZonePlanViewClicksCheck PROPERTIES LABELS "geozone;core")

# GeoZoneStubLoad checks the same uses with the QML engine, which knows the members of the base types of the stubs: it
# loads the stub types from their files, needing no display and nothing of the application but ToolStripAction, which
# the stub of GeoZoneToolStripAction derives from. Built in all builds like the check above. Its test builds it first,
# so that it also runs where only some targets were built.
qt_add_executable(GeoZoneStubLoad
    GeoZoneStubLoad.cc
    ${CMAKE_SOURCE_DIR}/src/QmlControls/ToolStripAction.cc
    ${CMAKE_SOURCE_DIR}/src/QmlControls/ToolStripAction.h
)
target_include_directories(GeoZoneStubLoad PRIVATE ${CMAKE_SOURCE_DIR}/src/QmlControls)
target_link_libraries(GeoZoneStubLoad PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
set_target_properties(GeoZoneStubLoad PROPERTIES
    MACOSX_BUNDLE FALSE
    WIN32_EXECUTABLE FALSE
    QT_EXCLUDE_FROM_TRANSLATION ON
)
add_dependencies(check GeoZoneStubLoad)
add_test(NAME GeoZoneStubLoad.build COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target GeoZoneStubLoad)
add_test(NAME GeoZoneStubLoad COMMAND GeoZoneStubLoad ${CMAKE_SOURCE_DIR})

# Both checks report what a stub lacks: test/GeoZone/stubcheck is a stub module with QML using it, Good.qml what the
# stubs declare and Bad.qml what they lack, through a qualified import. Each use of Bad.qml is reported, in the order
# of the regular expressions, and none of Good.qml.
add_test(NAME GeoZoneStubCheck.reports COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/stubcheck -P ${CMAKE_CURRENT_SOURCE_DIR}/GeoZoneStubCheck.cmake)
set_tests_properties(GeoZoneStubCheck.reports PROPERTIES
    PASS_REGULAR_EXPRESSION "Bad.qml:12: GeoZoneThing.text.*Bad.qml:13: GeoZoneThing.missing.*Bad.qml:14: GeoZoneThing.gone.*Bad.qml:18: GeoZonePart.visible.*Bad.qml:21: type GeoZoneUnknown.*Bad.qml: GeoZoneThing.absentFunction.*Bad.qml: type GeoZone[^A-Za-z].*Bad.qml: GeoZoneManager.absent.*Bad.qml: GeoZoneThing.Turbo"
)
add_test(NAME GeoZoneStubLoad.reports COMMAND GeoZoneStubLoad ${CMAKE_CURRENT_SOURCE_DIR}/stubcheck)
set_tests_properties(GeoZoneStubLoad.reports PROPERTIES
    PASS_REGULAR_EXPRESSION "Bad.qml:12: GeoZoneThing.text.*Bad.qml:13: GeoZoneThing.missing.*Bad.qml:14: GeoZoneThing.onGone.*Bad.qml:18: GeoZonePart.visible.*Bad.qml:21: type GeoZoneUnknown.*Bad.qml: thing.absentFunction.*Bad.qml: GZ.GeoZone.Prohibited.*Bad.qml: GZ.GeoZoneManager.settings.absent.*Bad.qml: GZ.GeoZoneThing.Turbo"
)
set_tests_properties(GeoZoneStubCheck.reports GeoZoneStubLoad.reports PROPERTIES FAIL_REGULAR_EXPRESSION "Good.qml")
set_tests_properties(GeoZoneStubLoad.build PROPERTIES FIXTURES_SETUP GeoZoneStubLoad)
set_tests_properties(GeoZoneStubLoad GeoZoneStubLoad.reports PROPERTIES FIXTURES_REQUIRED GeoZoneStubLoad ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(GeoZoneStubCheck.reports GeoZoneStubLoad.build GeoZoneStubLoad GeoZoneStubLoad.reports PROPERTIES LABELS "geozone;core")
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
