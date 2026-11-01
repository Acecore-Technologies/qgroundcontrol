# Headless geozone library: zones and their geometry, the formats and parsers, the sources with their downloads and
# storage, and the evaluation of positions and paths. It only depends on Qt, QGroundControl connects it to the
# application through GeoZoneEnvironment.
qt_add_library(GeoZoneCore STATIC
    GeoZone.cc
    GeoZone.h
    GeoZoneAlertTracker.cc
    GeoZoneAlertTracker.h
    GeoZoneAuthorisations.cc
    GeoZoneAuthorisations.h
    GeoZoneBox.cc
    GeoZoneBox.h
    GeoZoneCatalog.cc
    GeoZoneCatalog.h
    GeoZoneDownloader.cc
    GeoZoneDownloader.h
    GeoZoneEnvironment.cc
    GeoZoneEnvironment.h
    GeoZoneFences.cc
    GeoZoneFences.h
    GeoZoneFormat.cc
    GeoZoneFormat.h
    GeoZoneGeometry.cc
    GeoZoneGeometry.h
    GeoZoneIndex.cc
    GeoZoneIndex.h
    GeoZoneInfo.cc
    GeoZoneInfo.h
    GeoZoneLoader.cc
    GeoZoneLoader.h
    GeoZoneParser.cc
    GeoZoneParser.h
    GeoZoneParserUtils.cc
    GeoZoneParserUtils.h
    GeoZonePathEvaluator.cc
    GeoZonePathEvaluator.h
    GeoZonePolicy.cc
    GeoZonePolicy.h
    GeoZoneProviders.cc
    GeoZoneProviders.h
    GeoZoneSchedule.cc
    GeoZoneSchedule.h
    GeoZoneShape.cc
    GeoZoneShape.h
    GeoZoneSource.cc
    GeoZoneSource.h
    GeoZoneSourceRepository.cc
    GeoZoneSourceRepository.h
    GeoZoneStore.cc
    GeoZoneStore.h
    GeoZoneTerrain.cc
    GeoZoneTerrain.h
    GeoZoneZip.cc
    GeoZoneZip.h
    Formats/GeoZoneAixmFormat.cc
    Formats/GeoZoneDipulFormat.cc
    Formats/GeoZoneEd269Format.cc
    Formats/GeoZoneEd318Format.cc
    Formats/GeoZoneFaaTfrFormat.cc
    Formats/GeoZoneFranceUasFormat.cc
)

target_include_directories(GeoZoneCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(GeoZoneCore
    PUBLIC
        Qt6::Core
        Qt6::Positioning
    PRIVATE
        Qt6::Concurrent
        Qt6::Network
        ZLIB::ZLIBSTATIC
)

if(WIN32)
    target_compile_definitions(GeoZoneCore PRIVATE _USE_MATH_DEFINES)
endif()

# The EGM96 geoid for heights above the WGS84 ellipsoid
qt_add_resources(GeoZoneCore "geozone_geoid"
    PREFIX "/geoid"
    BASE Geoid
    FILES Geoid/egm96_1deg.bin
)
