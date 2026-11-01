add_subdirectory(Core)

# The application layer: zones on the maps, mission checks, flight monitoring, settings and QML
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZoneManager.cc
        GeoZoneManager.h
        GeoZoneModule.cc
        GeoZoneModule.h
        GeoZoneSettings.cc
        GeoZoneSettings.h
        Map/GeoZoneDisplay.cc
        Map/GeoZoneDisplay.h
        Map/GeoZoneMapLayer.cc
        Map/GeoZoneMapLayer.h
        Map/GeoZoneMapModel.cc
        Map/GeoZoneMapModel.h
        Map/GeoZoneNearbyModel.cc
        Map/GeoZoneNearbyModel.h
        Planning/GeoZoneBriefingExport.cc
        Planning/GeoZoneBriefingExport.h
        Planning/GeoZoneFenceImporter.cc
        Planning/GeoZoneFenceImporter.h
        Planning/GeoZoneFenceTags.cc
        Planning/GeoZoneFenceTags.h
        Planning/GeoZoneGuidedCheck.cc
        Planning/GeoZoneGuidedCheck.h
        Planning/GeoZoneMissionCheck.cc
        Planning/GeoZoneMissionCheck.h
        Planning/GeoZoneMissionPath.cc
        Planning/GeoZoneMissionPath.h
        Planning/GeoZonePlanData.cc
        Planning/GeoZonePlanData.h
        Planning/GeoZonePlanning.cc
        Planning/GeoZonePlanning.h
        Planning/GeoZoneProfile.cc
        Planning/GeoZoneProfile.h
        Planning/GeoZoneRoutePlanner.cc
        Planning/GeoZoneRoutePlanner.h
        Flight/GeoZoneAlertArbiter.cc
        Flight/GeoZoneAlertArbiter.h
        Flight/GeoZoneAlertHistory.cc
        Flight/GeoZoneAlertHistory.h
        Flight/GeoZoneAlertLog.cc
        Flight/GeoZoneAlertLog.h
        Flight/GeoZoneMonitor.cc
        Flight/GeoZoneMonitor.h
        Flight/GeoZoneVehicleMonitor.cc
        Flight/GeoZoneVehicleMonitor.h
)

target_include_directories(${CMAKE_PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Map
        ${CMAKE_CURRENT_SOURCE_DIR}/Planning
        ${CMAKE_CURRENT_SOURCE_DIR}/Flight
)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE GeoZoneCore)

# Settings metadata
qt_add_resources(${CMAKE_PROJECT_NAME} "geozone_resources"
    PREFIX "/json"
    FILES GeoZone.SettingsGroup.json
)

# GeoZoneMapLayer triangulates the zone polygons with the triangulator of Qt Quick
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::GuiPrivate)

qt_add_library(GeoZoneModule STATIC)

set_source_files_properties(Map/GeoZoneStyle.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(GeoZoneModule
    URI QGroundControl.GeoZone
    VERSION 1.0
    RESOURCE_PREFIX /qml
    QML_FILES
        Map/GeoZoneInfoDialog.qml
        Map/GeoZoneLegend.qml
        Map/GeoZoneMapPanel.qml
        Map/GeoZoneMapVisuals.qml
        Map/GeoZoneSearch.qml
        Map/GeoZoneStyle.qml
        Map/GeoZoneSummary.qml
        Planning/GeoZoneFenceImportDialog.qml
        Planning/GeoZoneFenceList.qml
        Planning/GeoZoneGuidedActionCheck.qml
        Planning/GeoZoneMissionBanner.qml
        Planning/GeoZonePlanViewOverlay.qml
        Planning/GeoZoneProfileOverlay.qml
        Planning/GeoZoneRouteDialog.qml
        Planning/GeoZoneToolStripAction.qml
        Flight/GeoZoneAlertPopup.qml
        Flight/GeoZoneAltitudeStrip.qml
        Flight/GeoZoneIndicator.qml
        Settings/GeoZoneAuthorisationsPanel.qml
        Settings/GeoZoneSettingsPage.qml
        Settings/GeoZoneSourceSwitches.qml
        Settings/GeoZoneSourcesPanel.qml
    RESOURCES
        GeoZone.svg
    NO_PLUGIN # Required by Qt 6.6.3. TODO: Remove when no longer supported.
)

# Explicitly linked like the other QML modules, needed by Qt 6.6.3
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE GeoZoneModule)
