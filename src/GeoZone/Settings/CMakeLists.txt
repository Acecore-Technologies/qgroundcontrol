# The geozone settings (GeoZoneSettings, described by GeoZone.SettingsGroup.json) and the Geozones page of the
# Application Settings
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZoneSettings.cc
        GeoZoneSettings.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Settings metadata
qt_add_resources(${CMAKE_PROJECT_NAME} "geozone_resources"
    PREFIX "/json"
    FILES GeoZone.SettingsGroup.json
)
