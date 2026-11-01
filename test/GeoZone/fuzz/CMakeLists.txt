# Fuzz targets of the geozone parsers, which read untrusted files and feeds. With clang they are libFuzzer targets
# built with AddressSanitizer and UndefinedBehaviorSanitizer, with other compilers they replay their corpus once,
# which runs as a test. See test/GeoZone/README.md.
#
# Configured standalone, only the GeoZoneCore library and the targets are built, the library also instrumented:
#   cmake -S test/GeoZone/fuzz -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
#         -DCMAKE_PREFIX_PATH=<Qt>/gcc_64
cmake_minimum_required(VERSION 3.22)

set(GEOZONE_FUZZ_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(GeoZoneFuzz LANGUAGES C CXX)
    set(GEOZONE_FUZZ_STANDALONE ON)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_AUTOMOC ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo)
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Network Positioning)
    find_package(ZLIB REQUIRED)
    if(NOT TARGET ZLIB::ZLIBSTATIC)
        add_library(ZLIB::ZLIBSTATIC INTERFACE IMPORTED)
        target_link_libraries(ZLIB::ZLIBSTATIC INTERFACE ZLIB::ZLIB)
    endif()
    enable_testing()
endif()

set(GEOZONE_LIBFUZZER OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(GEOZONE_LIBFUZZER ON)
endif()

if(GEOZONE_FUZZ_STANDALONE)
    if(GEOZONE_LIBFUZZER)
        # The library is instrumented too, so that libFuzzer follows the coverage of the parsers
        add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    endif()
    # The parsers don't sign, and the library builds without OpenSSL like on platforms without it
    set(GEOZONE_PACK_SIGNING OFF)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/GeoZone/Core ${CMAKE_CURRENT_BINARY_DIR}/GeoZoneCore)
endif()

# The seed corpora: the test files, a few small documents, and zip packages of the test files
set(GEOZONE_FUZZ_DATA ${CMAKE_CURRENT_SOURCE_DIR}/../data)
set(GEOZONE_FUZZ_CORPUS ${CMAKE_CURRENT_BINARY_DIR}/corpus)
file(GLOB GEOZONE_FUZZ_JSON_SEEDS ${GEOZONE_FUZZ_DATA}/*.json ${CMAKE_CURRENT_SOURCE_DIR}/seeds/json/*)
file(GLOB GEOZONE_FUZZ_XML_SEEDS ${GEOZONE_FUZZ_DATA}/*.xml ${CMAKE_CURRENT_SOURCE_DIR}/seeds/xml/*)
file(COPY ${GEOZONE_FUZZ_JSON_SEEDS} DESTINATION ${GEOZONE_FUZZ_CORPUS}/json)
file(COPY ${GEOZONE_FUZZ_XML_SEEDS} DESTINATION ${GEOZONE_FUZZ_CORPUS}/xml)
file(MAKE_DIRECTORY ${GEOZONE_FUZZ_CORPUS}/zip)
foreach(package IN ITEMS "ed269_test.json" "uk_aixm_test.xml" "ed269_test.json;ch_zurich_ed269.json" "ed318_test.json;dipul_test.json")
    string(REPLACE ";" "+" archive "${package}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar cf ${GEOZONE_FUZZ_CORPUS}/zip/${archive}.zip --format=zip ${package}
        WORKING_DIRECTORY ${GEOZONE_FUZZ_DATA}
        COMMAND_ERROR_IS_FATAL ANY
    )
endforeach()

foreach(target IN ITEMS Json Xml Zip)
    set(name GeoZoneFuzz${target})
    string(TOLOWER ${target} corpus)
    if(GEOZONE_LIBFUZZER)
        add_executable(${name} ${name}.cc GeoZoneFuzz.h)
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    else()
        add_executable(${name} ${name}.cc GeoZoneFuzz.h GeoZoneFuzzDriver.cc)
    endif()
    target_link_libraries(${name} PRIVATE GeoZoneCore Qt6::Core)
    set_target_properties(${name} PROPERTIES MACOSX_BUNDLE FALSE WIN32_EXECUTABLE FALSE)
    # Runs the seeds once, without fuzzing
    add_test(NAME ${name} COMMAND ${name} -runs=0 ${GEOZONE_FUZZ_CORPUS}/${corpus})
    set_tests_properties(${name} PROPERTIES LABELS fuzz)
endforeach()
