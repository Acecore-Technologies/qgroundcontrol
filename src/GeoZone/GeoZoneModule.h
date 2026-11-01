/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

/// Entry points of the geozone module for the application. The module keeps its settings, resources and QML types,
/// so outside src/GeoZone it only needs these hooks, kept small to ease rebasing:
/// - QGCApplication: registerQmlTypes() and init()
/// - CMake: add_subdirectory(GeoZone); tests in test/GeoZone, test/CMakeLists.txt and test/UnitTestList.cc
/// - QGCCorePlugin::toolBarIndicators: GeoZoneIndicator.qml
/// - SettingsPagesModel.qml: the Geozones page
/// - MainWindow.qml: GeoZoneAlertPopup
/// - PlanView.qml: GeoZoneMapVisuals, GeoZoneMissionBanner, the Geozones tool strip panel with the briefing export, map
///   clicks showing zones and the upload confirmation
/// - QGCFileDialog.qml: the fileName proposed when saving, for the briefing export
/// - TerrainStatus.qml: GeoZoneProfileOverlay
/// - FlyViewMap.qml: GeoZoneMapVisuals, map clicks showing zones and the go to confirmation
/// - FlyViewWidgetLayer.qml: GeoZoneAltitudeStrip
/// - GuidedActionsController.qml: GeoZoneGuidedActionCheck; GuidedActionConfirm.qml shows its warning
/// - PlanMasterController: GeoZonePlanData in plan files
/// - Fence names: QGCFencePolygon, QGCFenceCircle, GeoFenceController::deleteFences, GeoFenceEditor.qml, GeoFenceList.qml
///
/// Changes outside the module which don't depend on it and could go upstream on their own: fence names with the
/// fence list and deleting many fences at once, GuidedValueSlider::value, and Vehicle clearing altitudeAboveTerr when
/// the terrain query fails.
namespace GeoZoneModule {

/// Connects the geozone core to the settings, units, terrain and downloads of QGroundControl, see GeoZoneEnvironment,
/// and registers the C++ types of the QGroundControl.GeoZone QML module, with GeoZoneManager and GeoZoneMonitor as
/// singletons. Called first, also when running unit tests.
void registerQmlTypes();

/// Loads the stored sources, starts monitoring the vehicles and shows the module's messages to the user
void init();

} // namespace GeoZoneModule
