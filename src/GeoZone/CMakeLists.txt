target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZone.cc
        GeoZone.h
        GeoZoneManager.cc
        GeoZoneManager.h
        GeoZoneMapShape.h
        GeoZoneParser.cc
        GeoZoneParser.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
