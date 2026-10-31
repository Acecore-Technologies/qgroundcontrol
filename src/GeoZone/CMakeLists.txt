target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZone.cc
        GeoZone.h
        GeoZoneAlertLog.cc
        GeoZoneAlertLog.h
        GeoZoneDownloader.cc
        GeoZoneDownloader.h
        GeoZoneFenceImporter.cc
        GeoZoneFenceImporter.h
        GeoZoneGeometry.cc
        GeoZoneGeometry.h
        GeoZoneIndex.cc
        GeoZoneIndex.h
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
        GeoZoneProfile.cc
        GeoZoneProfile.h
        GeoZoneSchedule.cc
        GeoZoneSchedule.h
        GeoZoneShape.cc
        GeoZoneShape.h
        GeoZoneSource.cc
        GeoZoneSource.h
        GeoZoneStore.cc
        GeoZoneStore.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
        GeoZoneFenceImportDialog.qml
        GeoZoneIndicator.qml
        GeoZoneInfoDialog.qml
        GeoZoneLegend.qml
        GeoZoneMapPanel.qml
        GeoZoneMapVisuals.qml
        GeoZoneMissionBanner.qml
        GeoZoneProfileOverlay.qml
        GeoZoneSettingsPage.qml
        GeoZoneSourcesPanel.qml
        GeoZoneStyle.qml
        GeoZoneSummary.qml
    RESOURCES
        GeoZone.svg
    NO_PLUGIN # Required by Qt 6.6.3. TODO: Remove when no longer supported.
)

# Explicitly linked like the other QML modules, needed by Qt 6.6.3
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE GeoZoneModule)
