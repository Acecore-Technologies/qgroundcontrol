target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZone.cc
        GeoZone.h
        GeoZoneGeometry.cc
        GeoZoneGeometry.h
        GeoZoneManager.cc
        GeoZoneManager.h
        GeoZoneMapModel.cc
        GeoZoneMapModel.h
        GeoZoneMissionCheck.cc
        GeoZoneMissionCheck.h
        GeoZoneMonitor.cc
        GeoZoneMonitor.h
        GeoZoneParser.cc
        GeoZoneParser.h
        GeoZoneShape.cc
        GeoZoneShape.h
        GeoZoneSource.cc
        GeoZoneSource.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
