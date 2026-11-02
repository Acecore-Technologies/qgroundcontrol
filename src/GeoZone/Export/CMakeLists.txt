# Exports of the planned mission to files (GeoZoneMissionExport): the briefing (JSON, web page or PDF) and the
# operational intent
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZoneBriefingExport.cc
        GeoZoneBriefingExport.h
        GeoZoneMissionExport.cc
        GeoZoneMissionExport.h
        GeoZoneOperationalIntentExport.cc
        GeoZoneOperationalIntentExport.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
