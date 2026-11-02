# Plan View: the mission path and its checks, the profile, the route planner, the fence import and upload, the
# operator zones and the geozone data of plan files
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GeoZoneFenceChoiceModel.cc
        GeoZoneFenceChoiceModel.h
        GeoZoneFenceImporter.cc
        GeoZoneFenceImporter.h
        GeoZoneFenceTags.cc
        GeoZoneFenceTags.h
        GeoZoneFenceUpload.cc
        GeoZoneFenceUpload.h
        GeoZoneMissionCheck.cc
        GeoZoneMissionCheck.h
        GeoZoneMissionJumps.cc
        GeoZoneMissionJumps.h
        GeoZoneMissionPath.cc
        GeoZoneMissionPath.h
        GeoZoneMissionWorker.cc
        GeoZoneMissionWorker.h
        GeoZoneOperatorZones.cc
        GeoZoneOperatorZones.h
        GeoZonePlanData.cc
        GeoZonePlanData.h
        GeoZonePlanning.cc
        GeoZonePlanning.h
        GeoZoneProfile.cc
        GeoZoneProfile.h
        GeoZoneRoutePlanner.cc
        GeoZoneRoutePlanner.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
