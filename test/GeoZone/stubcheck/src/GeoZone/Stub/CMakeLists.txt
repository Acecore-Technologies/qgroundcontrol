# A stub module like src/GeoZone/Stub with the QML using it in ../../App, for the tests of GeoZoneStubCheck.cmake and
# GeoZoneStubLoad (see ../../../../CMakeLists.txt): Good.qml uses what the stubs declare, Bad.qml what they lack. The
# checks read this list like the one of the application, it isn't built.
set_source_files_properties(GeoZoneManager.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(GeoZoneStubFixture
    URI QGroundControl.GeoZone
    VERSION 1.0
    QML_FILES
        GeoZoneManager.qml
        GeoZonePart.qml
        GeoZoneThing.qml
)
