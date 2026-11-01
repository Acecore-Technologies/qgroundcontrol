target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZone.cc
        GeoZone.h
        GeoZoneAlertLog.cc
        GeoZoneAlertLog.h
        GeoZoneAlertTracker.cc
        GeoZoneAlertTracker.h
        GeoZoneAuthorisations.cc
        GeoZoneAuthorisations.h
        GeoZoneCatalog.cc
        GeoZoneCatalog.h
        GeoZoneDisplay.cc
        GeoZoneDisplay.h
        GeoZoneDownloader.cc
        GeoZoneDownloader.h
        GeoZoneFenceImporter.cc
        GeoZoneFenceImporter.h
        GeoZoneFormat.cc
        GeoZoneFormat.h
        GeoZoneGeometry.cc
        GeoZoneGeometry.h
        GeoZoneGuidedCheck.cc
        GeoZoneGuidedCheck.h
        GeoZoneIndex.cc
        GeoZoneIndex.h
        GeoZoneInfo.cc
        GeoZoneInfo.h
        GeoZoneLoader.cc
        GeoZoneLoader.h
        GeoZoneManager.cc
        GeoZoneManager.h
        GeoZoneMapLayer.cc
        GeoZoneMapLayer.h
        GeoZoneMapModel.cc
        GeoZoneMapModel.h
        GeoZoneMissionCheck.cc
        GeoZoneModule.cc
        GeoZoneModule.h
        GeoZoneMissionCheck.h
        GeoZoneMissionPath.cc
        GeoZoneMissionPath.h
        GeoZoneMonitor.cc
        GeoZoneMonitor.h
        GeoZoneNearbyModel.cc
        GeoZoneNearbyModel.h
        GeoZoneParser.cc
        GeoZoneParser.h
        GeoZoneParserUtils.cc
        GeoZoneParserUtils.h
        GeoZonePlanData.cc
        GeoZonePlanning.cc
        GeoZonePlanning.h
        GeoZonePlanData.h
        GeoZonePolicy.cc
        GeoZonePolicy.h
        GeoZoneProfile.cc
        GeoZoneProfile.h
        GeoZoneProviders.cc
        GeoZoneProviders.h
        GeoZoneRoutePlanner.cc
        GeoZoneRoutePlanner.h
        GeoZoneSchedule.cc
        GeoZoneSchedule.h
        GeoZoneSettings.cc
        GeoZoneSettings.h
        GeoZoneShape.cc
        GeoZoneShape.h
        GeoZoneSource.cc
        GeoZoneSource.h
        GeoZoneSourceRepository.cc
        GeoZoneSourceRepository.h
        GeoZoneStore.cc
        GeoZoneStore.h
        GeoZoneZip.cc
        GeoZoneZip.h
        GeoZoneTerrain.cc
        GeoZoneTerrain.h
        Formats/GeoZoneAixmFormat.cc
        Formats/GeoZoneDipulFormat.cc
        Formats/GeoZoneEd269Format.cc
        Formats/GeoZoneEd318Format.cc
        Formats/GeoZoneFaaTfrFormat.cc
        Formats/GeoZoneFranceUasFormat.cc
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Settings metadata and the EGM96 geoid for heights above the WGS84 ellipsoid
qt_add_resources(${CMAKE_PROJECT_NAME} "geozone_resources"
    PREFIX "/json"
    FILES GeoZone.SettingsGroup.json
)
qt_add_resources(${CMAKE_PROJECT_NAME} "geozone_geoid"
    PREFIX "/geoid"
    BASE Geoid
    FILES Geoid/egm96_1deg.bin
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
