# Builds without geozones (-DQGC_DISABLE_GEOZONE=ON) leave out the module and use this stub of its QML module instead,
# so that the QML of QGroundControl importing QGroundControl.GeoZone loads unchanged: the types placed in its views,
# hidden and doing nothing, and the GeoZoneManager singleton hiding the Geozones settings page, whose hidden button still
# loads the module's icon. The GeoZoneStubCheck test (test/GeoZone/GeoZoneStubCheck.cmake) checks that the stubs declare
# what that QML uses.
qt_add_library(GeoZoneStubModule STATIC)

set_source_files_properties(GeoZoneManager.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)
set_source_files_properties(../GeoZone.svg PROPERTIES
    QT_RESOURCE_ALIAS GeoZone.svg
)

qt_add_qml_module(GeoZoneStubModule
    URI QGroundControl.GeoZone
    VERSION 1.0
    RESOURCE_PREFIX /qml
    QML_FILES
        GeoZoneAlertPopup.qml
        GeoZoneAltitudeStrip.qml
        GeoZoneFenceGroup.qml
        GeoZoneFlyMapLayer.qml
        GeoZoneGuidedActionWarning.qml
        GeoZoneManager.qml
        GeoZoneMapClickButton.qml
        GeoZonePlanViewOverlay.qml
        GeoZoneProfileOverlay.qml
        GeoZoneToolStripAction.qml
    RESOURCES
        ../GeoZone.svg
    NO_PLUGIN # Required by Qt 6.6.3. TODO: Remove when no longer supported.
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE GeoZoneStubModule)
