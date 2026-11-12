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
/// - CMake: add_subdirectory(GeoZone) in src and test, the tests register themselves in test/GeoZone
/// - QGCCorePlugin::toolBarIndicators: GeoZoneIndicator.qml
/// - SettingsPagesModel.qml: the Geozones page
/// - MainWindow.qml: GeoZoneAlertPopup
/// - PlanView.qml: GeoZonePlanViewOverlay with the zones, the mission banner and the Geozones panel, the
///   GeoZoneToolStripAction opening the panel, and calls to the overlay for map clicks and the upload confirmation
/// - QGCFileDialog.qml: the fileName proposed when saving, for the briefing export, and selectMultiple with
///   acceptedForLoadFiles, to add several geozone files or packs at once
/// - TerrainStatus.qml: GeoZoneProfileOverlay, over a distance axis in km or miles for long missions
/// - ToolStripDropPanel.qml: the panel placed before it is resized, which keeps the scroll position of the Geozones
///   panel
/// - FlyViewMap.qml: GeoZoneFlyMapLayer with the zones and the zones at map clicks, the GeoZoneMapClickButton of the
///   map click panel, and the layer's check making go to locations entering zones always confirmed
/// - FlyViewWidgetLayer.qml: GeoZoneAltitudeStrip
/// - GuidedActionConfirm.qml: GeoZoneGuidedActionWarning
/// - FlyViewPreFlightChecklistPopup.qml: destroyOnClose off, the checklist with its Geozones group is kept
/// - PlanMasterController: GeoZonePlanData in plan files, with the fence names of GeoZoneFenceTags, and following the
///   Plan View plan, see GeoZonePlanData::follow
/// - GeoFenceEditor.qml: GeoZoneFenceGroup, beside the lists of polygon and of circular fences
/// - QGCOptions::preFlightChecklistUrl: GeoZonePreFlightCheckList.qml, the checklist of the vehicle type with the
///   geozone checks
///
/// Changes outside the module which don't depend on it and could go upstream on their own: those of QGCFileDialog.qml,
/// ToolStripDropPanel.qml and FlyViewPreFlightChecklistPopup.qml, the distance axis of TerrainStatus.qml, Vehicle
/// clearing altitudeAboveTerr when the terrain query fails, QGCMapPolygon and QGCMapPolyline destroyed after the
/// application, and the modes of the ArduPilot vehicles of MockLink. docs/en/qgc-dev-guide/geozones.md lists them all
/// with their lines, see Upstream Footprint.
///
/// Builds with -DQGC_DISABLE_GEOZONE=ON leave the module out: the C++ hooks are within #ifndef QGC_DISABLE_GEOZONE and
/// the QML ones get the hidden no-op types of the stub module in src/GeoZone/Stub.
namespace GeoZoneModule {

/// Connects the geozone core to the settings, units, terrain and downloads of QGroundControl, see GeoZoneEnvironment,
/// and registers the C++ types of the QGroundControl.GeoZone QML module, with GeoZoneManager and GeoZoneMonitor as
/// singletons. Called first, also when running unit tests.
void registerQmlTypes();

/// Loads the stored sources, starts monitoring the vehicles and shows the module's messages to the user
void init();

} // namespace GeoZoneModule
