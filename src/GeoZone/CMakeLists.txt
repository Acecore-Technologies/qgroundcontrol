add_subdirectory(Core)

# The application layer: zones on the maps (Map), mission checks (Planning) and their exports (Export), flight
# monitoring (Flight) and settings (Settings), each directory listing its C++. GeoZoneManager publishes the zones to
# C++ and QML, GeoZoneModule registers the types.
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZoneManager.cc
        GeoZoneManager.h
        GeoZoneModule.cc
        GeoZoneModule.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE GeoZoneCore)

add_subdirectory(Map)
add_subdirectory(Planning)
add_subdirectory(Export)
add_subdirectory(Flight)
add_subdirectory(Settings)

# The QML of all the directories, next to their C++, in one module. The application refers to some of the files by
# their qrc URL, like qrc:/qml/QGroundControl/GeoZone/Settings/GeoZoneSettingsPage.qml.
qt_add_library(GeoZoneModule STATIC)

set_source_files_properties(Map/GeoZoneStyle.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(GeoZoneModule
    URI QGroundControl.GeoZone
    VERSION 1.0
    RESOURCE_PREFIX /qml
    QML_FILES
        Map/GeoZoneBadge.qml
        Map/GeoZoneInfoDialog.qml
        Map/GeoZoneLegend.qml
        Map/GeoZoneMapPanel.qml
        Map/GeoZoneMapVisuals.qml
        Map/GeoZoneSearch.qml
        Map/GeoZoneSourceNote.qml
        Map/GeoZoneStyle.qml
        Map/GeoZoneSummary.qml
        Planning/GeoZoneFenceGroup.qml
        Planning/GeoZoneFenceImportDialog.qml
        Planning/GeoZoneMissionBanner.qml
        Planning/GeoZoneOperatorZoneEditor.qml
        Planning/GeoZoneOperatorZonesSection.qml
        Planning/GeoZonePlanViewOverlay.qml
        Planning/GeoZoneProfileOverlay.qml
        Planning/GeoZoneRouteDialog.qml
        Planning/GeoZoneToolStripAction.qml
        Flight/GeoZoneAlertPopup.qml
        Flight/GeoZoneAltitudeStrip.qml
        Flight/GeoZoneConformanceStatus.qml
        Flight/GeoZoneFlyMapLayer.qml
        Flight/GeoZoneGuidedActionWarning.qml
        Flight/GeoZoneIndicator.qml
        Flight/GeoZoneIndicatorPage.qml
        Flight/GeoZoneMapClickButton.qml
        Flight/GeoZonePreFlightCheckButton.qml
        Flight/GeoZonePreFlightCheckList.qml
        Settings/GeoZoneAuthorisationsPanel.qml
        Settings/GeoZoneChoiceComboBox.qml
        Settings/GeoZonePackSigning.qml
        Settings/GeoZoneServiceEditor.qml
        Settings/GeoZoneSettingsPage.qml
        Settings/GeoZoneSourceList.qml
        Settings/GeoZoneSourceSwitches.qml
        Settings/GeoZoneSourcesPanel.qml
    RESOURCES
        GeoZone.svg
    NO_PLUGIN # Required by Qt 6.6.3. TODO: Remove when no longer supported.
)

# Explicitly linked like the other QML modules, needed by Qt 6.6.3
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE GeoZoneModule)
