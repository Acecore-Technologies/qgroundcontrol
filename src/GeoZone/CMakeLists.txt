add_subdirectory(Core)

# The application layer: zones on the maps, mission checks, flight monitoring, settings and QML
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZoneAlertHistory.cc
        GeoZoneAlertHistory.h
        GeoZoneAlertLog.cc
        GeoZoneAlertLog.h
        GeoZoneDisplay.cc
        GeoZoneDisplay.h
        GeoZoneFenceImporter.cc
        GeoZoneFenceImporter.h
        GeoZoneGuidedCheck.cc
        GeoZoneGuidedCheck.h
        GeoZoneManager.cc
        GeoZoneManager.h
        GeoZoneMapLayer.cc
        GeoZoneMapLayer.h
        GeoZoneMapModel.cc
        GeoZoneMapModel.h
        GeoZoneMissionCheck.cc
        GeoZoneMissionCheck.h
        GeoZoneMissionPath.cc
        GeoZoneMissionPath.h
        GeoZoneModule.cc
        GeoZoneModule.h
        GeoZoneMonitor.cc
        GeoZoneMonitor.h
        GeoZoneNearbyModel.cc
        GeoZoneNearbyModel.h
        GeoZonePlanData.cc
        GeoZonePlanData.h
        GeoZonePlanning.cc
        GeoZonePlanning.h
        GeoZoneProfile.cc
        GeoZoneProfile.h
        GeoZoneRoutePlanner.cc
        GeoZoneRoutePlanner.h
        GeoZoneSettings.cc
        GeoZoneSettings.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE GeoZoneCore)

# Settings metadata
qt_add_resources(${CMAKE_PROJECT_NAME} "geozone_resources"
    PREFIX "/json"
    FILES GeoZone.SettingsGroup.json
)

# GeoZoneMapLayer triangulates the zone polygons with the triangulator of Qt Quick
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::GuiPrivate)

qt_add_library(GeoZoneModule STATIC)

set_source_files_properties(GeoZoneStyle.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(GeoZoneModule
    URI QGroundControl.GeoZone
    VERSION 1.0
    RESOURCE_PREFIX /qml
    QML_FILES
        GeoZoneAlertPopup.qml
        GeoZoneAltitudeStrip.qml
        GeoZoneAuthorisationsPanel.qml
        GeoZoneFenceImportDialog.qml
        GeoZoneGuidedActionCheck.qml
        GeoZoneIndicator.qml
        GeoZoneInfoDialog.qml
        GeoZoneLegend.qml
        GeoZoneMapPanel.qml
        GeoZoneMapVisuals.qml
        GeoZoneMissionBanner.qml
        GeoZoneProfileOverlay.qml
        GeoZoneRouteDialog.qml
        GeoZoneSearch.qml
        GeoZoneSettingsPage.qml
        GeoZoneSourceSwitches.qml
        GeoZoneSourcesPanel.qml
        GeoZoneStyle.qml
        GeoZoneSummary.qml
    RESOURCES
        GeoZone.svg
    NO_PLUGIN # Required by Qt 6.6.3. TODO: Remove when no longer supported.
)

# Explicitly linked like the other QML modules, needed by Qt 6.6.3
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE GeoZoneModule)
