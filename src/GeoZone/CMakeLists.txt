target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZone.cc
        GeoZone.h
        GeoZoneManager.cc
        GeoZoneManager.h
        GeoZoneMapModel.cc
        GeoZoneMapModel.h
        GeoZoneParser.cc
        GeoZoneParser.h
        GeoZoneShape.cc
        GeoZoneShape.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
