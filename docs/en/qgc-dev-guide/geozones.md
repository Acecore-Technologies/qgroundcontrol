# Geozones

The geozone module loads UAS geographical zones (ED-269, ED-318, AIXM 5.1, FAA TFRs, DiPUL and IGN), shows them on the maps, checks missions and guided commands against them and alerts in flight.
This page describes how the module is built, for developers changing it. What it does for users is described in the [Geozones user guide](../qgc-user-guide/geozones/geozones.md).

## Architecture {#architecture}

The module has two layers:

| Directory | Contents |
| --------- | -------- |
| `src/GeoZone/Core` | `GeoZoneCore`, a static library which only depends on Qt (Core and Positioning, privately Concurrent, Network and zlib), so it can be tested and fuzzed without _QGroundControl_. |
| `src/GeoZone/Core/Model` | The zones (`GeoZone`, its `Data`, `Volume` and schedule in `GeoZoneSchedule`), the vertical datums of their limits (`GeoZoneVertical`), their translated display texts in the units of the application (`GeoZoneText`, so that the model has no translations or units), their geometry (`GeoZoneGeometry`, `GeoZoneBox`, `GeoZoneShape` for the maps), the spatial index (`GeoZoneIndex`), the `GeoZoneCatalog` of all zones, `GeoZonePolicy` (which zones a check applies to) and `GeoZoneInfo`, the value QML gets. |
| `src/GeoZone/Core/Formats` | `GeoZoneParser`, the `GeoZoneFormat` interface with one class per format, the helpers shared by the formats (`GeoZoneParserUtils`) and the zip reader (`GeoZoneZip`). |
| `src/GeoZone/Core/Sources` | The sources and how they are obtained: `GeoZoneSourceRepository`, `GeoZoneSource`, `GeoZoneFeedFetch`, `GeoZoneDownloader`, `GeoZoneRefreshScheduler`, `GeoZoneLoader`, `GeoZoneStore`, `GeoZoneSourceIndex`, `GeoZoneParseCache`, the known services (`GeoZoneProviders`, `GeoZoneService`), `GeoZoneCredentials`, `GeoZoneProvenance` and offline packs (`GeoZonePack`). |
| `src/GeoZone/Core/Evaluation` | Checks of positions and paths: `GeoZonePathEvaluator`, the autopilot return paths (`GeoZoneReturnModel`) and the return destinations of missions (`GeoZoneReturnDestinations` on `GeoZoneMissionItem`), `GeoZoneRouteSearch`, `GeoZoneAlertTracker`, `GeoZoneBriefing`, `GeoZonePlanAudit`, `GeoZoneFences` (fence storage and priorities), `GeoZoneAuthorisations`, `GeoZoneSourceVersion` and `GeoZoneTerrain`. |
| `src/GeoZone/Core/Geoid` | The EGM96 geoid grid converting heights above the WGS84 ellipsoid, see its `README.md`. |
| `src/GeoZone` | The application layer: `GeoZoneModule` (the entry points), `GeoZoneManager` (the singleton publishing the zones and their queries to C++ and QML), `GeoZoneSettings` with `GeoZone.SettingsGroup.json`. |
| `src/GeoZone/Map` | Map display: `GeoZoneMapModel`, `GeoZoneMapLayer` (scene graph rendering), `GeoZoneDisplay`, `GeoZoneNearbyModel`, the zone details, legend and search. |
| `src/GeoZone/Planning` | Plan View: `GeoZoneMissionPath` (see [Mission Path](#mission-path)), `GeoZoneMissionWorker`, `GeoZoneMissionCheck`, `GeoZoneProfile`, `GeoZoneRoutePlanner`, `GeoZoneBriefingExport`, `GeoZoneFenceImporter` with `GeoZoneFenceUpload` (the upload and read back of the fence, a state machine from `Idle` through `Sending` and `Verifying` to `Verified`, `Mismatch`, `Failed` or `Aborted`), `GeoZoneFenceTags`, `GeoZoneGuidedCheck` (the checks of the guided commands, with the returns from `GeoZoneReturnModel` and the mission from `GeoZoneMissionPath`), `GeoZonePlanning` and `GeoZonePlanData` (the `geoZones` section of plan files). |
| `src/GeoZone/Flight` | Fly View: `GeoZoneMonitor`, `GeoZoneVehicleMonitor`, `GeoZoneAlertArbiter`, `GeoZoneAlertHistory`, `GeoZoneAlertLog`, the indicator, alert popup, altitude strip and map layer. |
| `src/GeoZone/Settings` | The Geozones page of the Application Settings. |

The QML of each feature is next to its C++, in the `QGroundControl.GeoZone` QML module (`GeoZoneModule` in `src/GeoZone/CMakeLists.txt`).

The core never includes _QGroundControl_ headers. What it needs from the application goes through the hooks of `GeoZoneEnvironment`, which `GeoZoneModule::registerQmlTypes()` installs at startup, before zones are loaded:

| Hook | Set by _QGroundControl_ to | Default without the application |
| ---- | -------------------------- | ------------------------------- |
| `currentDateTimeUtc` | (not set) | The system clock |
| `sourceSettings` | The refresh and outdated data settings of `GeoZoneSettings` | 24 hours, 15 minutes, 28 days, 2 hours |
| `verticalDistanceUnits`, `metersToVerticalDistanceUnits` | The vertical distance units of the application | Meters |
| `terrainElevation` | `TerrainAtCoordinateQuery::getAltitudesForCoordinates`, NaN when the tile isn't loaded | NaN (unknown) |
| `qnhHPa` | An atomic copy of the **QNH** setting, since it is called from worker threads | 1013.25 hPa |
| `prepareDownload` | `QGCFileDownload::setIgnoreSSLErrorsIfNeeded` | Nothing |

`GeoZoneEnvironment::setHooks()` isn't thread safe: set the hooks while no other thread uses zones. Tests replace them with `ScopedEnvironment`, see [Testing](#testing).

## Data Flow {#data-flow}

```txt
 file ─────────────────────────────────────────────┐
 feed or service ─► GeoZoneFeedFetch (GUI thread)  │   token, dataset link, download, WFS pages,
                    with GeoZoneDownloader         │   companions, SHA-256, conditional requests
                                                   ▼
                    GeoZoneLoader::run (worker thread)
                      unzip ─► GeoZoneParseCache (current?) ─► GeoZoneParser ─► GeoZoneFormat
                      stored copy and cache written to GeoZoneStore
                                                   ▼
          GeoZoneSourceRepository (GUI thread): GeoZoneSource, GeoZoneSourceIndex (sources.json)
                                                   ▼
          GeoZoneManager: GeoZoneCatalog::rebuild() with its GeoZoneIndex, zonesChanged()
                                                   ▼  catalog copies
   GeoZonePathEvaluator: GeoZoneMissionCheck, GeoZoneProfile, GeoZoneRouteSearch, GeoZoneGuidedCheck,
                         GeoZoneBriefing, GeoZoneFences
   GeoZoneMonitor ─► GeoZoneVehicleMonitor ─► GeoZoneAlertTracker, GeoZoneAlertArbiter
   GeoZoneMapModel ─► GeoZoneMapLayer
```

1. `GeoZoneSourceRepository` adds, loads, refreshes and removes the sources. Each load or fetch of a source gets a new `GeoZoneCancelToken`, which cancels the previous one of the same source; removing a source or closing the application cancels its token too. Every stage checks the token and drops its result once it is canceled.
1. Feeds and services are downloaded by a `GeoZoneFeedFetch`, in stages: the OAuth2 token of the service, the feed URL (which may be a page linking to the current dataset, see `GeoZoneProviders::datasetLink`), the dataset, the further pages of a WFS response the server limited, and the companion documents the format refers to (like the NOTAM details of FAA TFRs). The document is checked against the SHA-256 its publisher gives. `GeoZoneRefreshScheduler` decides when feeds are fetched again.
1. `GeoZoneLoader::run()` reads the file or download on a worker thread: it extracts zip archives, restores the result from the `GeoZoneParseCache` when the file is a stored copy whose cache is current, or else parses it with `GeoZoneParser`, which detects the `GeoZoneFormat` and parses the features, and writes the stored copy and its cache with a copy of the `GeoZoneStore`.
1. Back on the GUI thread, the repository replaces the zones of the `GeoZoneSource` and saves the index of the sources (`GeoZoneSourceIndex`). `GeoZoneManager` then rebuilds the `GeoZoneCatalog` from the sources turned on and emits `zonesChanged()`.
1. The checks take a copy of the catalog. `GeoZonePathEvaluator` evaluates paths against it for the mission check, the altitude profile, the guided command checks, the route search, the briefing and the fence import, so that they agree; `GeoZonePolicy` selects the zones each of them applies to. The monitor evaluates the vehicle positions every second with `GeoZoneAlertTracker`, and the maps query the shapes in their view.

## Mission Path {#mission-path}

The Plan View checks see the mission as a `GeoZoneMissionPath`: the segments flown with their altitudes and terrain, the areas of patterns, the legs between the items, the paths besides the drawn flight path and the contingency paths, see the [user guide](../qgc-user-guide/geozones/geozones.md#mission-check). It is built on the GUI thread, then only read, so that the checks can run on worker threads. The parts:

- `GeoZoneMissionReader` reads the `MissionController` the way it draws the flight path (in `_recalcFlightPathSegments`): the segments of each item, the segment to the next item with a coordinate, nothing after the first **Return To Launch** but the segment home. It lists the items as `GeoZoneMissionItem`s, adds the paths the drawn flight path leaves out (loiter circles, VTOL transitions, the loiter of landing patterns, the return after a **Return To Launch**) and reads the vehicle's return parameters.
- `GeoZoneMissionJumps` walks the items like the vehicle with their **DO_JUMP** items, a pure computation on the items and the indexes and lengths of the drawn segments, tested without a mission controller (`GeoZonePlanningTest::_testMissionJumpWalk`).
- `GeoZoneReturnDestinations::fromMission()` (core) finds where the vehicle can return to from the items and the plan's home, rally points and breach return point, given as plain positions: the first waypoint after the takeoff, the landing sequences and the return path, for `GeoZoneReturnModel`. `GeoZoneMissionReader::destinations()` calls it with the plan of a `MissionController`, other checks can call it with their own. `GeoZoneReturnTest` tests it headless.
- `GeoZoneMissionPath` itself keeps the result and computes what doesn't need the application: the connected paths of the segments, the times along them, the contingency starts and paths with `GeoZoneReturnModel`, and the labels of the paths.

## Vertical Limits {#vertical-limits}

`GeoZoneVertical` converts the published limits of a volume to heights comparable with altitudes above mean sea level and compares them, for `GeoZone`, the evaluator, the alerts, the profile and the route search (`GeoZone::verticalOverlap()`, `verticalInterval()`, `verticalPosition()` and `limitAMSL()` forward to it). Limits above ground subtract the terrain elevation from the altitudes, limits above the WGS84 ellipsoid add the EGM96 geoid height, and flight levels are converted with the QNH, then floors are lowered and ceilings raised by `GeoZoneVertical::flightLevelMargin`.

There is one policy for what isn't known, in `GeoZoneVertical::bound()`: a limit bounds the volume only when it can be compared. A limit in an unknown reference, a limit above ground where the terrain is unknown, or any limit compared with an unknown altitude doesn't bound it, so the volume extends over all altitudes on that side and unknowns count as inside. Floors at the ground and the published unlimited ceiling don't bound it either. New comparisons should go through `bound()` rather than read the limits. `GeoZoneVertical::approximations()` tells why a comparison is approximate, which `GeoZonePathEvaluator` turns into the `Approximation` flags of its crossings, stretches and results, together with the flags of the paths (`TerrainCoarse`, `GreatCircleDeviation`) and schedules (`ScheduleAssumed`, see `GeoZone::scheduleAssumed()`). The briefing and the mission check warning report them; keep `approximationTexts()` and `approximationCodes()` in step with new flags.

## Threading Model {#threading}

- The GUI thread owns the manager, the repository, the sources, the downloads (`GeoZoneFeedFetch`, `GeoZoneDownloader`), the refresh scheduler, the monitor and the map models. `GeoZone::active` is updated there and read from other threads as an atomic.
- Long work runs with `QtConcurrent::run` on the global thread pool: `GeoZoneLoader::run`, `GeoZoneMissionCheck::evaluate`, the bands of `GeoZoneProfile` and the searches of `GeoZoneRoutePlanner` (which reports progress and can be canceled). Their results come back through a `QFutureWatcher` and are dropped when outdated: loads check their cancel token, and the mission check, the profile and the route planner share a `GeoZoneMissionWorker` (`src/GeoZone/Planning`).
- `GeoZoneMissionWorker` follows the signals of the `MissionController` after which the mission changes, and the changes its owner reports with `schedule()`, for a delay (half a second) before `triggered()` asks the owner to start an evaluation. `run()` starts a new generation and evaluates on the thread pool, `watch()` adds a computation to the current generation (like another round of the route search, with its progress), and `runNow()` evaluates on the GUI thread. `invalidate()` and `stop()` outdate the running evaluations, whose results are dropped and whose `cancelToken()` is canceled; the owners call them when the zones are about to change. `settled()` tells whether the owner's result is that of its current inputs. Saving a plan (`GeoZonePlanData::save()`) reuses the result of the Plan View's `GeoZoneMissionCheck` when it is settled (`GeoZoneMissionCheck::settledCheck()`), and otherwise evaluates the mission on the GUI thread with `evaluatePlan()` and `classify()`, which have no side effects like setting the area of interest.
- Workers only get values: a `GeoZoneCatalog` copy, which is immutable and keeps its zones alive through shared pointers while the sources change (but not the shapes, which only the maps use), a `GeoZoneMissionPath` built on the GUI thread, and the job or settings of the computation. `GeoZoneStore` methods only access files, so a copy can be used from a worker.
- Caches filled on first use from any thread are guarded: the simplified exclusion polygons of a `GeoZone` (`_simplifiedMutex`), its local projection (`std::once_flag`) and the Web Mercator rings of a `GeoZoneShape` per zoom level (`_mercatorMutex`). `GeoZoneIndex` queries don't modify the index.
- An evaluator copy can run on any thread, unless its policy excludes authorised zones, since authorisations are only looked up on the GUI thread, or its terrain lookup, which is called from the evaluating thread, needs the GUI thread. Formats are stateless and parse on worker threads; `parseXml()` stops between features once `GeoZoneParserUtils::cancelled()`.

## Route Search {#route-search}

`GeoZoneRouteSearch` (`src/GeoZone/Core/Evaluation`) is the facade `GeoZoneRoutePlanner` uses: `plan()` searches a detour, a climb and a descent for a leg and sorts them by cost. Its parts are internal to the core:

- `GeoZoneRouteObstacles`: the zone volumes a detour keeps the margin from, in a local projection around the leg. Their outlines are only projected, detailed ones simplified outward except near the ends of the leg, when a query comes near them, and their edges are put in a `GeoZoneRouteGrid` of cells which finds those near a segment. It tells whether a segment or a point keeps the margin, and where the nodes around the convex corners of an obstacle are.
- `GeoZoneRouteGraph`: the visibility graph of a detour, its nodes, the cache of the visibility of its edges and the A* search of its shortest paths, which tests an edge only when it would reach a node first (Lazy Weighted A*).
- `GeoZoneRouteDetour`: the rounds of a detour search, each adding to the graph the zones the path ran into, until it runs into none, and its outcome.
- `GeoZoneRouteVertical`: the samples of the leg with their terrain, and the climbs over and descents under the zones.
- `GeoZoneRouteVector.h`: planar vector helpers, a stand-in with the functions of the shared `GeoZoneVector.h`.

The work of a search is bounded. Testing the edges from each node to all others takes time growing with the square of the nodes, a minute of a debug build for a maze of 1200 zones. Graphs of more than 1000 nodes are therefore first searched along the edges to the near nodes only (those of the grid cells around a node, at least 32 nodes of 8 obstacles, and the end), combining edges like Theta* where a node sees the one before the previous, and the path is then straightened; all edges are searched when that finds no path, except by the search of detours up to 1.3 times the leg, which then leaves it to the search with the full length limit. Each graph counts its work, the edges and nodes considered and the visibility tests weighted by 16, and its searches stop with `Failure::SearchLimit` at `Settings::maxSearchWork`, as they do after 64 rounds or at 20000 nodes. `GeoZonePlanningTest::_testRouteMaze` checks the time, the work and the length of a detour through a maze, which `GeoZoneBenchmark::_benchmarkRouteSearchMaze` measures.

## Adding a Format {#adding-a-format}

1. Add a value at the end of `GeoZoneParser::Format`, whose values are stored in the parse caches.
1. Add a class implementing `GeoZoneFormat` in `src/GeoZone/Core/Formats`, registered with `GEOZONE_REGISTER_FORMAT(Class)`, and add its instance to `GeoZoneFormat::formats()` in `GeoZoneFormat.cc` (the linker leaves out files of the static library which nothing references). List the file in `src/GeoZone/Core/CMakeLists.txt`.
1. Recognize the documents:
   - JSON formats implement `detect()`, called by `GeoZoneFormat::detectJson()` in increasing `detectionOrder()`. Formats recognized by their feature properties come before ED-318 (50), which accepts any `FeatureCollection`, and ED-269 (60). Web feature services may answer with an empty list, which only the format hinted by the URL (`GeoZoneProviders::formatHint`) accepts, with `acceptsEmpty()`.
   - XML formats return `true` from `isXml()` and implement `detectXml()` with the root element and namespace, and `parseXml()` for the whole document.
1. Convert the features: `readMetadata()` for the title, provider and issue time, and `parseFeature()` filling a `GeoZone::Data` with `GeoZoneParserUtils` (`_appendHorizontalGeometry`, `_applicability`, `_lengthUnit`, `_commonProperties`...). Times of schedules are UTC, fixed offsets (`DailyPeriod::utcOffsetSecs`) or local times with their `QTimeZone` (`DailyPeriod::timeZone`), which keeps them right across daylight saving time changes; `_applicability` gives times without offset the time zone of the zone's country (`GeoZoneSchedule::countryTimeZone`), so it needs the country and the volumes. That time zone is only assumed (`DailyPeriod::timeZoneAssumed`), since the formats don't tell whether such times are local or UTC, so `GeoZoneSchedule` applies these periods in both. `GeoZoneParser` then checks the geometry (`_normalizeVolumes`). Report problems in `zone.issues` rather than failing the file.
1. If the features refer to other documents, implement `companionRequests()` (key, version and URL of each) and `parseFeatureWithCompanions()`; `GeoZoneFeedFetch` downloads them and stores them with the document (`GeoZoneParser::composite`), see the FAA TFR format.
1. Edges are taken straight in latitude and longitude, like GeoJSON lines. If the specification makes them geodesics, add points on their great circles with `GeoZoneParserUtils::_densifyGreatCircles()` (or `GeoZoneGeometry::densifyGreatCircles()` with a flag per edge, like the AIXM format), and turn arcs into points within `GeoZoneGeometry::kGreatCircleToleranceMeters`.
1. Return `true` from `isLive()` for short-lived restrictions refreshed and outdated after hours, and add the services of the format to `GeoZoneProviders::services()` (see [Adding a Provider or Service](#adding-a-provider)).
1. Bump `GeoZoneParser::version`, so that stored sources are parsed again instead of restored from their caches. Do the same whenever a change of the parser, a format or `GeoZoneParserUtils` changes the result for the same bytes.
1. If `GeoZoneParser::Result`, `GeoZone::Data`, `GeoZone::Volume` or the schedule periods get new fields, serialize them in their `QDataStream` operators and bump `GeoZoneParseCache::formatVersion`. The `static_assert`s on the sizes of these structures (in `GeoZoneParser.cc`, `GeoZone.cc` and `GeoZoneSchedule.cc`) fail when a field is added, as a reminder; update the sizes once the fields are serialized.
1. Add a fixture to `test/GeoZone/data/`, list it in `GEOZONE_TEST_DATA` of `test/GeoZone/CMakeLists.txt`, add a row to `GeoZoneParserTest::_testParseFixtures_data()` and a test of the zones it gives, and its edges to `_testEdgeInterpolation_data()` if they aren't straight. Document the fixture in `test/GeoZone/README.md`.
1. Add the format to the hints of `test/GeoZone/fuzz/GeoZoneFuzzJson.cc` (JSON formats) and seeds to `test/GeoZone/fuzz/seeds/` for responses the fixture doesn't cover; the fixtures are seeds too.
1. Update the [format support matrix](../qgc-user-guide/geozones/geozones.md#format-support-matrix) and the parse error messages naming the supported formats.

## Adding a Provider or Service {#adding-a-provider}

The services _QGroundControl_ knows are listed in `GeoZoneProviders` (`src/GeoZone/Core/Sources`):

- A national feed is an entry of `GeoZoneProviders::services()` with an id, a translated name, the host its URLs have (which `formatHint()` and `serviceId()` recognize) and the format. With a `feedUrl` it is offered by **Add Feed...** (`feedPresets()`).
- A feed whose URL is a download page gets a `datasetLinkPattern`, whose first capture is the link to a dataset and the second its effective date (`yyyyMMdd`); the latest dataset effective today is downloaded, like the AIRAC datasets of NATS.
- A service serving its zones by area gets an `areaUrl` and the `coverage` it serves: it is offered by **Download zones for the map area** (`areaProviders()`), for areas up to `maxAreaDegrees`. WFS `GetFeature` URLs get their further pages with `wfsPageUrl()`, up to `maxWfsPages`.
- A service needing credentials, an ED-318 POST or the area of interest is a `ServiceTemplate` of `serviceTemplates()`, offered by **Add Service...**: the URL template (with the placeholders of `GeoZoneService`), and a `GeoZoneService` with the `method`, `auth` (and `tokenUrl`, `authParameter`, `scope`), `headers`, `responseMember`, `areaOfInterest`, `timeWindowHours`, `maxAreaKm2` and `sha256Url`, a note telling how to get access, and `supported = false` for services whose data can't be read yet. Secrets are never part of `GeoZoneService`, `GeoZoneCredentials` keeps them.

`GeoZoneParserTest::_testProviders()` tests the providers, `GeoZoneManagerTest` the services with `TestFeedServer` (`_testServiceApiKeys`, `_testServiceOAuth2`, `_testEd318Service`, `_testServiceAreaOfInterest`), and `GeoZoneParserTest::_testLiveNationalFeeds()` downloads the real services when `QGC_GEOZONE_LIVE_TEST` is set. Add the provider to the national feeds or services listed in the [user guide](../qgc-user-guide/geozones/geozones.md#loading-data).

## Testing {#testing}

The tests are in `test/GeoZone`, whose `README.md` also describes the fixtures, the fuzzers and the benchmark.

- **Application tests**: `GeoZoneParserTest`, `GeoZoneGeometryTest`, `GeoZoneManagerTest`, `GeoZonePlanningTest`, `GeoZoneMonitorTest`, `GeoZoneFlightTest` and `GeoZoneReturnTest` run in a build with `QGC_BUILD_TESTING`, with `QGroundControl --unittest:<Class>` (see [Unit Tests](contribute/unit_tests.md)). They are registered in `test/GeoZone/GeoZoneUnitTests.cc` and `add_qgc_test()` in `test/GeoZone/CMakeLists.txt`, not in `UnitTestList.cc`, so that the module needs no changes to the test list of _QGroundControl_.
- **Headless core tests**: the parser, geometry and return tests also build into `GeoZoneCoreTest`, linked with `GeoZoneCore` only (`GEOZONE_CORE_TEST` leaves out the parts needing the application, see `GeoZoneTestBase.h`): `ctest -R GeoZoneCore`.
- **CTest labels**: `GeoZoneBenchmark` is labeled `benchmark` and the fuzz targets `fuzz`; `ctest -LE "benchmark|fuzz"` runs the other tests.
- **Benchmark**: `GeoZoneBenchmark` measures the core with 10000 synthetic zones: `ctest -L benchmark -V`.
- **Fuzzers**: `test/GeoZone/fuzz` has libFuzzer targets for JSON, XML and zip input, built with `-DGEOZONE_FUZZ=ON` or as a project of their own with clang.
- **QML**: `tst_geozone_qml` runs the Qt Quick tests of `test/GeoZone/qml` offscreen (`ctest -R tst_geozone_qml`). Only `GeoZoneStyle` is tested there, since the other components need the singletons of the application.
- **Time and terrain**: tests depending on the time, the terrain or the QNH set them with `GeoZoneTestHelpers::ScopedEnvironment`, whose clock stands still until the test advances it (`setNow()`, `addSecs()`) and whose terrain is unknown until set (`setTerrain()`).
- **Downloads**: `GeoZoneTestHelpers::TestFeedServer` is a local HTTP server answering by path (`/ok` with the ED-318 fixture, `/missing`, `/broken`, `/empty`, `/hang`), with added files, handlers and delays, which records the requests for the feed, service and refresh tests.

The `Geozone Sanitizers` workflow (`.github/workflows/geozone-sanitizers.yml`) runs the tests with AddressSanitizer and UndefinedBehaviorSanitizer, and the fuzzers for a minute each.

::: warning
QML errors are only found at runtime. A component of the module with a missing import or an unknown type still builds, but fails to load, and since the components are placed in views of _QGroundControl_ (like `PlanView.qml` and `FlyViewMap.qml`), the parent view breaks with it. After changing QML, run _QGroundControl_, open the Plan View, the Fly View and the Geozones settings with zones loaded, and check the console for QML warnings.
:::

## Upstream Footprint {#upstream-footprint}

The module is kept apart from _QGroundControl_ so that it can be rebased on upstream changes: everything lives in `src/GeoZone` and `test/GeoZone`, and each file outside them gets a single insertion point, listed in `GeoZoneModule.h`. New hooks should follow the same rule: one call, one QML component or one import, rather than logic outside the module. Settings, resources, QML types and tests are registered by the module itself.

The edits outside the module, from `git diff --stat <branch point> HEAD -- . ':!src/GeoZone' ':!test/GeoZone' ':!docs' ':!.github' ':!translations'` (the branch point is `f1d2e1006` at the time of writing):

| File | Lines (+/-) | Insertion point |
| ---- | ----------- | --------------- |
| `src/CMakeLists.txt`, `test/CMakeLists.txt` | +1, +2 | `add_subdirectory(GeoZone)` |
| `src/QGCApplication.cc` | +3 | `GeoZoneModule::registerQmlTypes()` and `GeoZoneModule::init()` |
| `src/API/QGCCorePlugin.cc` | +1 | `GeoZoneIndicator.qml` in the toolbar indicators |
| `src/UI/AppSettings/SettingsPagesModel.qml` | +8 | The Geozones settings page |
| `src/UI/MainWindow.qml` | +8 | `GeoZoneAlertPopup` |
| `src/QmlControls/PlanView.qml` | +26/-1 | `GeoZonePlanViewOverlay` (zones, mission banner, Geozones panel), `GeoZoneToolStripAction`, map clicks and the upload confirmation (`GeoZoneMissionCheck::uploadReasons()`) |
| `src/QmlControls/GeoFenceEditor.qml` | +5/-164 | `GeoZoneFenceList` in place of the lists of polygon and circular fences |
| `src/QmlControls/TerrainStatus.qml` | +7 | `GeoZoneProfileOverlay` |
| `src/QmlControls/QGCFileDialog.qml` | +5 | `fileName` proposed when saving, for the briefing export |
| `src/FlightDisplay/FlyViewMap.qml` | +13/-1 | `GeoZoneFlyMapLayer`, `GeoZoneMapClickButton` and confirmed go to locations entering zones |
| `src/FlightDisplay/FlyViewWidgetLayer.qml` | +9 | `GeoZoneAltitudeStrip` |
| `src/FlightDisplay/GuidedActionConfirm.qml` | +3 | `GeoZoneGuidedActionWarning` |
| `src/MissionManager/PlanMasterController.cc` | +4 | `GeoZonePlanData::load()` and `save()` of the `geoZones` section of plan files, and `follow()`, which resets the planned start and audit record when the Plan View replaces its mission |
| `src/Vehicle/Vehicle.cc` | +2 | Clears `altitudeAboveTerr` when the terrain query fails |

The `fileName` of `QGCFileDialog.qml` and the `Vehicle.cc` change don't depend on the module and could go upstream on their own.
