# Checks that a click on the Plan View map which neither adds nor selects an item shows the geozones there in every
# layer: each case of the layer switch of onMapClicked in src/QmlControls/PlanView.qml calls
# geoZoneOverlay.showZonesAt(). The layers of some builds, like the UTMSP one, can't be tried in the others, so a layer
# without the call, added upstream or left by a merge, would go unnoticed.
#   cmake -DSOURCE_DIR=<source dir> -P GeoZonePlanViewClicksCheck.cmake
# The check compares text, so it runs without building anything.

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "Usage: cmake -DSOURCE_DIR=<source dir> -P GeoZonePlanViewClicksCheck.cmake")
endif()

file(READ "${SOURCE_DIR}/src/QmlControls/PlanView.qml" planView)
string(FIND "${planView}" "onMapClicked:" handlerStart)
if(handlerStart EQUAL -1)
    message(FATAL_ERROR "PlanView.qml has no onMapClicked handler")
endif()
string(SUBSTRING "${planView}" ${handlerStart} -1 handler)
string(FIND "${handler}" "switch (_editingLayer)" switchStart)
# The handler ends with the first closing brace at its own indentation
string(FIND "${handler}" "\n            }\n" handlerEnd)
if(switchStart EQUAL -1 OR handlerEnd EQUAL -1 OR handlerEnd LESS switchStart)
    message(FATAL_ERROR "The layer switch of onMapClicked wasn't found in PlanView.qml")
endif()
math(EXPR switchLength "${handlerEnd} - ${switchStart}")
string(SUBSTRING "${handler}" ${switchStart} ${switchLength} rest)

set(layers 0)
set(missing "")
while(TRUE)
    string(FIND "${rest}" "case _layer" caseStart)
    if(caseStart EQUAL -1)
        break()
    endif()
    math(EXPR nameStart "${caseStart} + 5")
    string(SUBSTRING "${rest}" ${nameStart} -1 rest)
    string(FIND "${rest}" ":" nameEnd)
    string(SUBSTRING "${rest}" 0 ${nameEnd} layer)
    # Up to the next case, or the end of the switch
    string(FIND "${rest}" "case _layer" nextCase)
    string(SUBSTRING "${rest}" 0 ${nextCase} block)
    string(FIND "${block}" "geoZoneOverlay.showZonesAt(" shown)
    if(shown EQUAL -1)
        string(APPEND missing " ${layer}")
    endif()
    math(EXPR layers "${layers} + 1")
endwhile()

if(layers LESS 3)
    message(FATAL_ERROR "Only ${layers} layer(s) found in the layer switch of onMapClicked in PlanView.qml")
endif()
if(missing)
    message(FATAL_ERROR "A map click in these layers of PlanView.qml doesn't show the geozones (geoZoneOverlay.showZonesAt):${missing}")
endif()
message(STATUS "The map clicks of the ${layers} layers of PlanView.qml show the geozones")
