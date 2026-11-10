# Headless geozone library: zones and their geometry (Model), the formats and parsers (Formats), the sources with their
# downloads and storage (Sources), the evaluation of positions and paths (Evaluation), the route search (Route), the
# briefings and operational intents (Report) and the geoid (Geoid). It only depends on Qt, QGroundControl connects it to
# the application through GeoZoneEnvironment.
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
    Model/GeoZoneGeometry.h
    Model/GeoZoneGreatCircle.cc
    Model/GeoZoneGreatCircle.h
    Model/GeoZoneIndex.cc
    Model/GeoZoneIndex.h
    Model/GeoZoneInfo.cc
    Model/GeoZoneInfo.h
    Model/GeoZonePolicy.cc
    Model/GeoZonePolicy.h
    Model/GeoZonePredicates.cc
    Model/GeoZonePredicates.h
    Model/GeoZoneProjection.cc
    Model/GeoZoneProjection.h
    Model/GeoZoneSchedule.cc
    Model/GeoZoneSchedule.h
    Model/GeoZoneShape.cc
    Model/GeoZoneShape.h
    Model/GeoZoneSimplify.cc
    Model/GeoZoneSimplify.h
    Model/GeoZoneText.cc
    Model/GeoZoneText.h
    Model/GeoZoneVector.h
    Model/GeoZoneVertical.cc
    Model/GeoZoneVertical.h
    Formats/GeoZoneAixmFormat.cc
    Formats/GeoZoneDipulFormat.cc
    Formats/GeoZoneEd269Format.cc
    Formats/GeoZoneEd318Format.cc
    Formats/GeoZoneEd318Writer.cc
    Formats/GeoZoneEd318Writer.h
    Formats/GeoZoneFaaAirspaceFormat.cc
    Formats/GeoZoneFaaTfrFormat.cc
    Formats/GeoZoneFormat.cc
    Formats/GeoZoneFormat.h
    Formats/GeoZoneFranceUasFormat.cc
    Formats/GeoZoneOpenAipFormat.cc
    Formats/GeoZoneParser.cc
    Formats/GeoZoneParser.h
    Formats/GeoZoneParserUtils.cc
    Formats/GeoZoneParserUtils.h
    Formats/GeoZoneZip.cc
    Formats/GeoZoneZip.h
    Sources/GeoZoneAirac.cc
    Sources/GeoZoneAirac.h
    Sources/GeoZoneAreaOfInterest.cc
    Sources/GeoZoneAreaOfInterest.h
    Sources/GeoZoneAuthorisations.cc
    Sources/GeoZoneAuthorisations.h
    Sources/GeoZoneCredentials.cc
    Sources/GeoZoneCredentials.h
    Sources/GeoZoneDownloader.cc
    Sources/GeoZoneDownloader.h
    Sources/GeoZoneFeedCatalogue.cc
    Sources/GeoZoneFeedCatalogue.h
    Sources/GeoZoneFeedCatalogueLoader.cc
    Sources/GeoZoneFeedCatalogueLoader.h
    Sources/GeoZoneFeedFetch.cc
    Sources/GeoZoneFeedFetch.h
    Sources/GeoZoneHash.h
    Sources/GeoZoneLoader.cc
    Sources/GeoZoneLoader.h
    Sources/GeoZoneOrigin.h
    Sources/GeoZonePack.cc
    Sources/GeoZonePack.h
    Sources/GeoZonePackExchange.cc
    Sources/GeoZonePackExchange.h
    Sources/GeoZonePackSignature.cc
    Sources/GeoZonePackSignature.h
    Sources/GeoZoneParseCache.cc
    Sources/GeoZoneParseCache.h
    Sources/GeoZoneProvenance.cc
    Sources/GeoZoneProvenance.h
    Sources/GeoZoneProviders.cc
    Sources/GeoZoneProviders.h
    Sources/GeoZoneRefreshScheduler.cc
    Sources/GeoZoneRefreshScheduler.h
    Sources/GeoZoneService.cc
    Sources/GeoZoneService.h
    Sources/GeoZoneServiceRegistry.cc
    Sources/GeoZoneServiceRegistry.h
    Sources/GeoZoneSource.cc
    Sources/GeoZoneSource.h
    Sources/GeoZoneSourceIndex.cc
    Sources/GeoZoneSourceIndex.h
    Sources/GeoZoneSourceRepository.cc
    Sources/GeoZoneSourceRepository.h
    Sources/GeoZoneSourceVersion.cc
    Sources/GeoZoneSourceVersion.h
    Sources/GeoZoneStore.cc
    Sources/GeoZoneStore.h
    Evaluation/GeoZoneActivationTracker.cc
    Evaluation/GeoZoneActivationTracker.h
    Evaluation/GeoZoneAlertTracker.cc
    Evaluation/GeoZoneAlertTracker.h
    Evaluation/GeoZoneFencePrediction.cc
    Evaluation/GeoZoneFencePrediction.h
    Evaluation/GeoZoneAlertThresholds.cc
    Evaluation/GeoZoneAlertThresholds.h
    Evaluation/GeoZoneFences.cc
    Evaluation/GeoZoneFences.h
    Evaluation/GeoZoneMissionItem.h
    Evaluation/GeoZoneMissionJumps.cc
    Evaluation/GeoZoneMissionJumps.h
    Evaluation/GeoZoneMissionPath.cc
    Evaluation/GeoZoneMissionPath.h
    Evaluation/GeoZoneMissionPathBuilder.cc
    Evaluation/GeoZoneMissionPathBuilder.h
    Evaluation/GeoZonePathEvaluator.cc
    Evaluation/GeoZonePathEvaluator.h
    Evaluation/GeoZoneReturnDestinations.cc
    Evaluation/GeoZoneReturnDestinations.h
    Evaluation/GeoZoneReturnModel.cc
    Evaluation/GeoZoneReturnModel.h
    Evaluation/GeoZoneTerrain.cc
    Evaluation/GeoZoneTerrain.h
    Route/GeoZoneRouteDetour.cc
    Route/GeoZoneRouteDetour.h
    Route/GeoZoneRouteGraph.cc
    Route/GeoZoneRouteGraph.h
    Route/GeoZoneRouteObstacles.cc
    Route/GeoZoneRouteObstacles.h
    Route/GeoZoneRouteSearch.cc
    Route/GeoZoneRouteSearch.h
    Route/GeoZoneRouteVertical.cc
    Route/GeoZoneRouteVertical.h
    Report/GeoZoneBriefing.cc
    Report/GeoZoneBriefing.h
    Report/GeoZoneOperationalIntent.cc
    Report/GeoZoneOperationalIntent.h
    Report/GeoZoneOperatorDetails.cc
    Report/GeoZoneOperatorDetails.h
    Report/GeoZonePlanAudit.cc
    Report/GeoZonePlanAudit.h
    Geoid/GeoZoneGeoid.cc
    Geoid/GeoZoneGeoid.h
)

target_include_directories(GeoZoneCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Model
        ${CMAKE_CURRENT_SOURCE_DIR}/Formats
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources
        ${CMAKE_CURRENT_SOURCE_DIR}/Evaluation
        ${CMAKE_CURRENT_SOURCE_DIR}/Route
        ${CMAKE_CURRENT_SOURCE_DIR}/Report
        ${CMAKE_CURRENT_SOURCE_DIR}/Geoid
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

# Ed25519 signatures of offline packs use the libcrypto of OpenSSL (Qt has no Ed25519). QGroundControl doesn't link
# OpenSSL otherwise: Qt loads it at run time, Android bundles the libraries of android_openssl for Qt. So signing is
# on by default for desktop Linux only, which has the system libcrypto, and opt-in elsewhere. Without libcrypto 1.1.1
# or later the build goes on: packs are exported unsigned and signed imports are taken as unsigned, see
# GeoZonePackSignature.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    set(GEOZONE_PACK_SIGNING_DEFAULT ON)
else()
    set(GEOZONE_PACK_SIGNING_DEFAULT OFF)
endif()
option(GEOZONE_PACK_SIGNING "Sign geozone offline packs and check their signatures with OpenSSL's libcrypto, when found" ${GEOZONE_PACK_SIGNING_DEFAULT})
if(GEOZONE_PACK_SIGNING)
    find_package(OpenSSL QUIET COMPONENTS Crypto)
endif()
if(GEOZONE_PACK_SIGNING AND TARGET OpenSSL::Crypto AND (OPENSSL_VERSION VERSION_GREATER_EQUAL 1.1.1))
    target_link_libraries(GeoZoneCore PRIVATE OpenSSL::Crypto)
    target_compile_definitions(GeoZoneCore PRIVATE GEOZONE_HAVE_OPENSSL)
    message(STATUS "GeoZone: signing offline packs with OpenSSL ${OPENSSL_VERSION}")
elseif(GEOZONE_PACK_SIGNING)
    message(STATUS "GeoZone: OpenSSL libcrypto 1.1.1 or later not found, offline packs can't be signed")
else()
    message(STATUS "GeoZone: signing offline packs is off (GEOZONE_PACK_SIGNING)")
endif()

# The manufacturer's alert thresholds per vehicle class, which custom builds replace, see GeoZoneAlertThresholds
qt_add_resources(GeoZoneCore "geozone_alert_profiles"
    PREFIX "/geozone"
    BASE Evaluation
    FILES Evaluation/GeoZoneAlertProfiles.json
)

# The EGM96 geoid for heights above the WGS84 ellipsoid
qt_add_resources(GeoZoneCore "geozone_geoid"
    PREFIX "/geoid"
    BASE Geoid
    FILES Geoid/egm96_1deg.bin
)

# The built-in feed catalogue, see GeoZoneFeedCatalogue. Custom builds can compile in their own catalogue instead.
set(GEOZONE_FEEDS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Sources/GeoZoneFeeds.json" CACHE FILEPATH "Built-in geozone feed catalogue (GeoZoneFeeds.json format)")
set_source_files_properties("${GEOZONE_FEEDS_FILE}" PROPERTIES QT_RESOURCE_ALIAS GeoZoneFeeds.json)
qt_add_resources(GeoZoneCore "geozone_feeds"
    PREFIX "/geozone"
    FILES "${GEOZONE_FEEDS_FILE}"
)
