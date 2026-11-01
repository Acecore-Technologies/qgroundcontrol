# Headless geozone library: zones and their geometry, the formats and parsers, the sources with their downloads and
# storage, and the evaluation of positions and paths. It only depends on Qt, QGroundControl connects it to the
# application through GeoZoneEnvironment.
qt_add_library(GeoZoneCore STATIC
    GeoZoneCancelToken.h
    GeoZoneEnvironment.cc
    GeoZoneEnvironment.h
    Model/GeoZone.cc
    Model/GeoZone.h
    Model/GeoZoneBox.cc
    Model/GeoZoneBox.h
    Model/GeoZoneCatalog.cc
    Model/GeoZoneCatalog.h
    Model/GeoZoneDataStream.h
    Model/GeoZoneGeometry.cc
    Model/GeoZoneGeometry.h
    Model/GeoZoneIndex.cc
    Model/GeoZoneIndex.h
    Model/GeoZoneInfo.cc
    Model/GeoZoneInfo.h
    Model/GeoZonePolicy.cc
    Model/GeoZonePolicy.h
    Model/GeoZoneSchedule.cc
    Model/GeoZoneSchedule.h
    Model/GeoZoneShape.cc
    Model/GeoZoneShape.h
    Formats/GeoZoneAixmFormat.cc
    Formats/GeoZoneDipulFormat.cc
    Formats/GeoZoneEd269Format.cc
    Formats/GeoZoneEd318Format.cc
    Formats/GeoZoneFaaTfrFormat.cc
    Formats/GeoZoneFormat.cc
    Formats/GeoZoneFormat.h
    Formats/GeoZoneFranceUasFormat.cc
    Formats/GeoZoneParser.cc
    Formats/GeoZoneParser.h
    Formats/GeoZoneParserUtils.cc
    Formats/GeoZoneParserUtils.h
    Formats/GeoZoneZip.cc
    Formats/GeoZoneZip.h
    Sources/GeoZoneCredentials.cc
    Sources/GeoZoneCredentials.h
    Sources/GeoZoneDownloader.cc
    Sources/GeoZoneDownloader.h
    Sources/GeoZoneFeedFetch.cc
    Sources/GeoZoneFeedFetch.h
    Sources/GeoZoneLoader.cc
    Sources/GeoZoneLoader.h
    Sources/GeoZoneParseCache.cc
    Sources/GeoZoneParseCache.h
    Sources/GeoZoneProviders.cc
    Sources/GeoZoneProviders.h
    Sources/GeoZoneRefreshScheduler.cc
    Sources/GeoZoneRefreshScheduler.h
    Sources/GeoZoneService.cc
    Sources/GeoZoneService.h
    Sources/GeoZoneSource.cc
    Sources/GeoZoneSource.h
    Sources/GeoZoneSourceIndex.cc
    Sources/GeoZoneSourceIndex.h
    Sources/GeoZoneSourceRepository.cc
    Sources/GeoZoneSourceRepository.h
    Sources/GeoZoneStore.cc
    Sources/GeoZoneStore.h
    Evaluation/GeoZoneAlertTracker.cc
    Evaluation/GeoZoneAlertTracker.h
    Evaluation/GeoZoneAuthorisations.cc
    Evaluation/GeoZoneAuthorisations.h
    Evaluation/GeoZoneBriefing.cc
    Evaluation/GeoZoneBriefing.h
    Evaluation/GeoZoneFences.cc
    Evaluation/GeoZoneFences.h
    Evaluation/GeoZonePathEvaluator.cc
    Evaluation/GeoZonePathEvaluator.h
    Evaluation/GeoZonePlanAudit.cc
    Evaluation/GeoZonePlanAudit.h
    Evaluation/GeoZoneReturnModel.cc
    Evaluation/GeoZoneReturnModel.h
    Evaluation/GeoZoneSourceVersion.cc
    Evaluation/GeoZoneSourceVersion.h
    Evaluation/GeoZoneTerrain.cc
    Evaluation/GeoZoneTerrain.h
)

target_include_directories(GeoZoneCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Model
        ${CMAKE_CURRENT_SOURCE_DIR}/Formats
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources
        ${CMAKE_CURRENT_SOURCE_DIR}/Evaluation
)

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
