# geozone-validate checks geozone files and zip packages like QGroundControl loads them, with the GeoZoneCore library
# only, see the Testing section of docs/en/qgc-dev-guide/geozones.md
qt_add_executable(geozone-validate
    GeoZoneInputFiles.h
    GeoZoneValidate.cc
)

target_link_libraries(geozone-validate PRIVATE GeoZoneCore Qt6::Core)
set_target_properties(geozone-validate PROPERTIES
    MACOSX_BUNDLE FALSE
    WIN32_EXECUTABLE FALSE
    QT_EXCLUDE_FROM_TRANSLATION ON
)

# The test fixtures load, a document which isn't geozones doesn't, and the fixture with a feature without geometry fails
# with --strict. The output tells which, since it decides the result of the tests expecting failures.
add_test(NAME geozone-validate.fixtures COMMAND geozone-validate ${CMAKE_CURRENT_SOURCE_DIR}/../data)
add_test(NAME geozone-validate.invalid COMMAND geozone-validate ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
add_test(NAME geozone-validate.strict COMMAND geozone-validate --strict ${CMAKE_CURRENT_SOURCE_DIR}/../data/ed269_test.json)
# The formats are named like in the feed catalogue, other names are refused
add_test(NAME geozone-validate.format COMMAND geozone-validate --format openaip ${CMAKE_CURRENT_SOURCE_DIR}/../data/openaip_test.json)
add_test(NAME geozone-validate.unknown-format COMMAND geozone-validate --format geojson ${CMAKE_CURRENT_SOURCE_DIR}/../data/openaip_test.json)
set_tests_properties(geozone-validate.fixtures geozone-validate.invalid geozone-validate.strict geozone-validate.format geozone-validate.unknown-format
                     PROPERTIES LABELS "geozone;core")
set_tests_properties(geozone-validate.format PROPERTIES PASS_REGULAR_EXPRESSION "openaip_test.json: OpenAIP, 6 zones.*1 file, 0 errors")
set_tests_properties(geozone-validate.unknown-format PROPERTIES PASS_REGULAR_EXPRESSION "Unknown format geojson")
set_tests_properties(geozone-validate.invalid PROPERTIES PASS_REGULAR_EXPRESSION "CMakeLists.txt: error: .*1 file, 1 error")
set_tests_properties(geozone-validate.strict PROPERTIES PASS_REGULAR_EXPRESSION "Skipped: 1 feature.*which fail with --strict")
