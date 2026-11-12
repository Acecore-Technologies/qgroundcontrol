# Tests the check of the built-in geozone feed catalogue, src/GeoZone/Core/GeoZoneFeedsCheck.cmake, which fails the
# configuration for a GEOZONE_FEEDS_FILE QGroundControl couldn't read: the built-in catalogue passes, and each
# malformed catalogue is refused with its reason.
#   cmake -DSOURCE_DIR=<source dir> -DWORK_DIR=<folder for the test files> -P GeoZoneFeedsCheckTest.cmake

if(NOT SOURCE_DIR OR NOT WORK_DIR)
    message(FATAL_ERROR "Usage: cmake -DSOURCE_DIR=<source dir> -DWORK_DIR=<folder> -P GeoZoneFeedsCheckTest.cmake")
endif()
set(check "${SOURCE_DIR}/src/GeoZone/Core/GeoZoneFeedsCheck.cmake")
set(errors "")

# Runs the check on a file, sets result to its exit code and output to its messages
function(run_check file result output)
    execute_process(COMMAND ${CMAKE_COMMAND} "-DFEEDS_FILE=${file}" -P "${check}" RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${result} "${code}" PARENT_SCOPE)
    set(${output} "${out}${err}" PARENT_SCOPE)
endfunction()

# The catalogue with the content must be refused with a message holding the reason
function(expect_refused name content reason)
    set(file "${WORK_DIR}/${name}.json")
    file(WRITE "${file}" "${content}")
    run_check("${file}" code output)
    string(REGEX REPLACE "[ \t\r\n]+" " " output "${output}")
    string(FIND "${output}" "${reason}" found)
    if(code EQUAL 0)
        set(errors "${errors}\n${name}: accepted, expected: ${reason}" PARENT_SCOPE)
    elseif(found EQUAL -1)
        set(errors "${errors}\n${name}: refused with \"${output}\", expected: ${reason}" PARENT_SCOPE)
    endif()
endfunction()

# The catalogue with the content must pass
function(expect_accepted name content)
    set(file "${WORK_DIR}/${name}.json")
    file(WRITE "${file}" "${content}")
    run_check("${file}" code output)
    if(NOT code EQUAL 0)
        set(errors "${errors}\n${name}: refused with ${output}" PARENT_SCOPE)
    endif()
endfunction()

run_check("${SOURCE_DIR}/src/GeoZone/Core/Sources/GeoZoneFeeds.json" code output)
if(NOT code EQUAL 0)
    string(APPEND errors "\nThe built-in catalogue is refused: ${output}")
endif()
run_check("${WORK_DIR}/missing.json" code output)
if(code EQUAL 0)
    string(APPEND errors "\nA missing file is accepted")
endif()

set(feed "{ \"id\": \"a\", \"name\": \"A\", \"url\": \"https://zones.example.com/a.json\" }")
expect_accepted(minimal "{ \"feeds\": [ ${feed} ] }")
expect_accepted(disabled "{ \"fileType\": \"GeoZoneFeeds\", \"version\": 1, \"feeds\": [ ${feed}, { \"id\": \"b\", \"enabled\": false } ], \"services\": [] }")
expect_accepted(area "{ \"feeds\": [ { \"id\": \"a\", \"name\": \"A\", \"areaUrl\": \"https://zones.example.com/a?bbox={bbox}\" } ] }")
expect_accepted(service "{ \"services\": [ { \"id\": \"s\", \"name\": \"S\", \"service\": { \"areaOfInterest\": true } } ] }")

expect_refused(truncated "{ \"feeds\": [ ${feed}, " "invalid JSON")
expect_refused(list "[ ${feed} ]" "isn't a JSON object")
expect_refused(empty "{}" "it has no feeds or services")
expect_refused(file-type "{ \"fileType\": \"FactMetaData\", \"feeds\": [ ${feed} ] }" "its fileType is FactMetaData rather than GeoZoneFeeds")
expect_refused(newer "{ \"version\": 2, \"feeds\": [ ${feed} ] }" "its version is 2, this QGroundControl reads version 1")
expect_refused(version-text "{ \"version\": \"1\", \"feeds\": [ ${feed} ] }" "its version is 1, this QGroundControl reads version 1")
expect_refused(feeds-object "{ \"feeds\": { \"a\": ${feed} } }" "its feeds must be a list")
expect_refused(entry-text "{ \"feeds\": [ \"a\" ] }" "entry 1 of its feeds isn't an object")
expect_refused(no-id "{ \"feeds\": [ ${feed}, { \"name\": \"B\", \"url\": \"https://zones.example.com/b.json\" } ] }" "entry 2 of its feeds has no id")
expect_refused(twice "{ \"feeds\": [ ${feed}, ${feed} ] }" "a is listed twice in its feeds")
expect_refused(no-name "{ \"feeds\": [ { \"id\": \"a\", \"url\": \"https://zones.example.com/a.json\" } ] }" "a has no name")
expect_refused(no-url "{ \"feeds\": [ { \"id\": \"a\", \"name\": \"A\" } ] }" "a has neither a url nor an areaUrl")
expect_refused(address "{ \"feeds\": [ { \"id\": \"a\", \"name\": \"A\", \"url\": \"zones.example.com/a.json\" } ] }" "isn't an https, http or file address")
expect_refused(enabled-text "{ \"feeds\": [ { \"id\": \"a\", \"enabled\": \"false\" } ] }" "the enabled of a must be true or false, without quotes")
expect_refused(service-name "{ \"services\": [ { \"id\": \"s\" } ] }" "s has no name")

if(errors)
    message(FATAL_ERROR "The check of the geozone feed catalogue fails:${errors}")
endif()
message(STATUS "The check of the geozone feed catalogue refuses the malformed catalogues and accepts the built-in one")
