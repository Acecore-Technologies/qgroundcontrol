# Map display: the zones on the maps (GeoZoneMapModel rendered by GeoZoneMapLayer), the nearby and searched zones
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZoneDisplay.cc
        GeoZoneDisplay.h
        GeoZoneMapLayer.cc
        GeoZoneMapLayer.h
        GeoZoneMapModel.cc
        GeoZoneMapModel.h
        GeoZoneNearbyModel.cc
        GeoZoneNearbyModel.h
        GeoZoneSearchModel.cc
        GeoZoneSearchModel.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# GeoZoneMapLayer triangulates the zone polygons with the triangulator of Qt Quick
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::GuiPrivate)
