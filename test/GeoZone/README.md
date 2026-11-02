# Geozone tests

The test classes are listed once, in `CMakeLists.txt`:

- `GEOZONE_SHARED_TESTS` (GeoZoneParserTest, GeoZoneGeometryTest, GeoZoneReturnTest, GeoZoneIntentTest of the
  operational intents and GeoZoneMissionPathTest of the mission paths) run in QGroundControl and headless in
  `GeoZoneCoreTest`, which only links the GeoZoneCore library and leaves out their parts needing the application
  (`GEOZONE_CORE_TEST`, see `GeoZoneTestBase.h`).
- `GEOZONE_CORE_TESTS` (GeoZoneEvaluatorTest, GeoZoneRouteTest, GeoZoneBriefingTest) test the path evaluator, the
  route search and the briefing of the core, and only run headless.
- `GEOZONE_APP_TESTS` (GeoZoneManagerTest, GeoZonePlanningTest, GeoZoneMonitorTest, GeoZoneFlightTest) need the
  application, like its mission controller, MockLink or the map models, and only run in QGroundControl.

A class `<Class>` is `<Class>.cc` and `<Class>.h`, and its source ends with `GEOZONE_TEST(<Class>)`, which registers it
with QGroundControl's UnitTest (not in QGroundControl's `UnitTestList.cc`) or with `GeoZoneCoreTest`
(`GeoZoneCoreTestMain.cc`), see `GeoZoneTestBase.h`. The lists build the classes and add a CTest test for each: the
application tests are named after their class and labeled `geozone` and `app`, the headless ones `GeoZoneCore.<Class>`
and labeled `geozone` and `core`. A new class only needs its files and its name in a list. Test helpers shared by the
suites, like the fixtures of zones and the catalog `ZoneCatalog` which loads files like GeoZoneManager without the
application, are declared in `GeoZoneTestHelpers.h`.

QGroundControl defines its tests in the `test` directory of the build, where CTest runs:

```sh
ctest --test-dir build/test -L geozone -LE "benchmark|fuzz"   # all geozone tests but the benchmark and the fuzzers
ctest --test-dir build/test -L core                           # the headless tests, without the application
build/Debug/QGroundControl --unittest:GeoZonePlanningTest     # an application test
build/Debug/GeoZoneCoreTest GeoZoneRouteTest _testMaze        # a headless test class, or a function of it
```

Tests which depend on the time or the terrain set them with `GeoZoneTestHelpers::ScopedEnvironment`, whose clock
stands still until the test advances it and whose terrain is unknown until set.

## Test data

The files of `data/` are listed in `GEOZONE_TEST_DATA` of `CMakeLists.txt` and compiled into QGroundControl and
`GeoZoneCoreTest` as `:/unittest/<file>`. They are also seeds of the fuzzers, and some of them are packed into the zip
seeds made when configuring them. The provenance of the extracts of real data is only known as far as stated below:
where it isn't recorded, check the publisher's terms before redistributing the file outside this repository. Add new
fixtures to this list with their source, terms and tests. `GeoZoneParserTest::_testParseFixtures_data` records a hash of
the parse result of each fixture, which guards `GeoZoneParser::version`: record it again when the fixture changes.

| File | Content | Source | Licence / terms | Used by |
| ---- | ------- | ------ | --------------- | ------- |
| `ch_zurich_ed269.json` | 8 Swiss zones around Zurich in ED-269: substations, prisons, the airports LSZH and LSMD and the CTRs of Zurich and Dübendorf | Extract of the Swiss UAS Geozones (`ch.bazl.einschraenkungen-drohnen`) of the Federal Office of Civil Aviation FOCA, `https://data.geo.admin.ch/ch.bazl.einschraenkungen-drohnen/`, published 2026-10-31: the unmodified zones intersecting latitude 47.3653 to 47.4332 and longitude 8.4858 to 8.5787, as described in the file. Added in 596a12cb3. | Open data, opendata.swiss "Open use. Must provide the source" (stated in the file's `description`, which must be kept) | GeoZoneParserTest (`_testParseFixtures`, `_testParserFuzz`), GeoZoneFlightTest (`_testFlightAlertSequence`, `_testMissionCrossedZones`, `_testMockLinkVehicle`) |
| `dipul_test.json` | 4 German zones of a DiPUL WFS response: a control zone, a restricted area, a temporary no-fly zone with its period and limits given with their units, and a hospital, with the WFS counts and `timeStamp` | Synthetic, written for these tests in the structure of the DiPUL WFS layers and properties, with invented names, ids and geometry. Added in 94ecc5dff as an excerpt of DiPUL data, since replaced. | QGroundControl's licence (`COPYING.md`) | GeoZoneParserTest (`_testParseFixtures`, `_testParseNationalFormats`) |
| `ed269_test.json` | 4 ED-269 features: a prohibited zone with two polygon layers, a circle requiring authorisation, a conditional zone with a schedule, and a feature without geometry; UTF-8 byte order mark | Synthetic, written for these tests (its `description` says so). Added in 57c0f41b7. | QGroundControl's licence (`COPYING.md`) | GeoZoneParserTest (`_testParseFixtures`, `_testParseED269`, `_testParserFuzz`, `_testDisplayText`, `_testXmlAndPackages`, `_testParseCacheValidation`), GeoZoneGeometryTest (`_testIsActiveAt`, `_testIsActiveDuring`, `_testExclusionShapes`), GeoZoneManagerTest (`_testManagerLoadPersistClear`, `_testManagerBackgroundLoad`, `_testFeedNotModified`, `_testOfflinePack`), GeoZonePlanningTest (`_testGeoFenceImport`) |
| `ed318_test.json` | 4 ED-318 features with dataset metadata: a circle (`Point` extent) with localized names, a multipolygon, a U-space geometry collection and an unsupported `LineString` | Synthetic, written for these tests (provider "Test Authority", `example.com` contacts). Added in 57c0f41b7, extended in 655ef4548. | QGroundControl's licence (`COPYING.md`) | GeoZoneParserTest (`_testParseFixtures`, `_testParseED318`, `_testParserFuzz`, `_testParseCacheValidation`), GeoZoneGeometryTest (`_testIsActiveAt`, `_testExclusionShapes`), GeoZoneManagerTest (`_testManagerLoadPersistClear`, `_testManagerStoreFailures`, `_testManagerBackgroundLoad`, `_testFeedCancellation`, `_testFeedNotModified`, `_testSourceValidity`, and the feed tests through `TestFeedServer`, which serves it at `/ok`), GeoZonePlanningTest (`_testGeoFenceImport`, and `_testPlanSave` through `TestFeedServer`) |
| `faa_airspace_test.json` | 7 features of the FAA's public ArcGIS airspace layers around Washington, DC: two UAS facility map cells of Dulles (300 ft, and a copy set to 0 ft), the USNO national security UAS flight restriction, the surface area of the Washington class B, the Reagan National Mode C veil (left out by the parser), P-56 and R-4001A | Excerpts of `FAA_UAS_FacilityMap_Data`, `DoD_Mar_13`, `Class_Airspace` and `Special_Use_Airspace` of `https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/`, queried 2026-11-02 with `f=geojson`: properties as published (without the `Shape__` fields), polygons thinned to about ten vertices, the second facility map cell modified. | US government work, public domain; the FAA's disclaimer applies (not for navigation) | GeoZoneParserTest (`_testParseFixtures`, `_testParseFaaAirspace`), GeoZoneManagerTest (`_testArcGisPaging`) |
| `faa_tfr_test.json` | 2 FAA TFRs of the `V_TFR_LOC` WFS layer (NOTAMs 6/4240 San Angelo and 5/6914 South Padre Island, TX), circles published as 36-sided polygons | Apparently an excerpt of the tfr.faa.gov GeoServer WFS response; not recorded. The NOTAM details the tests pair with it are written in the tests. Added in 596a12cb3. | unknown — verify before redistributing | GeoZoneParserTest (`_testParseFixtures`, `_testParseFaaTfr`, `_testParserFuzz`), GeoZoneManagerTest (`_testFeedCompanions`) |
| `france_uas_test.json` | 2 French open category restrictions ("Vol interdit" and a 50 m maximum height) in a WFS response limited to 2 of 10 features | Synthetic, written for these tests in the structure of the IGN Géoplateforme layer `carte_restriction_drones_lf`, with invented ids, remark and rectangular geometries near Paris, and WFS counts set to simulate a limited response. Added in 94ecc5dff with properties of the layer, since replaced. | QGroundControl's licence (`COPYING.md`) | GeoZoneParserTest (`_testParseFixtures`, `_testParseNationalFormats`), GeoZoneManagerTest (`_testWfsPaging`) |
| `openaip_test.json` | 8 OpenAIP airspaces in the format of its GeoJSON exports: a class D CTR with a frequency, a restricted area by NOTAM with weekday hours and a flight level ceiling, a prohibited area in meters, class C and E TMAs, a FIR, a danger area on demand and a protected area in meters AGL; the class E TMA and the FIR are left out | Synthetic, written for these tests in the structure of `storage.openaip.net/openaip-system-exports/<country>_asp.geojson` (fields and enums of the OpenAIP airspace schema), with invented names, ids and square geometries near 47° N 8° E | QGroundControl's licence (`COPYING.md`) | GeoZoneParserTest (`_testParseFixtures`, `_testParseOpenAip`), GeoZoneManagerTest (`_testOpenAipService`, as the items of the API's pages) |
| `uk_aixm_test.xml` | 3 UK airspaces in an AIXM 5.1 basic message: the Blackbushe FRZ (`EGR1U021A`, arcs and geodesic strings), Springfields (`EGR312`, a circle) and the Raasay danger area (`EGD710`, line strings and geodesic strings) | Apparently an excerpt of the NATS UAS AIXM 5.1 dataset (snapshot of 2026-10-29); not recorded. Added in 94ecc5dff. | unknown — verify before redistributing | GeoZoneParserTest (`_testParseFixtures`, `_testParseNationalFormats`, `_testRejectedAixmGeometry`, `_testXmlAndPackages`, `_testParseCancel`), GeoZoneManagerTest (`_testFeedDatasetLink`) |

GeoZoneParserTest and GeoZoneGeometryTest also run in `GeoZoneCoreTest`, without the parts which need the application.
The other tests make their zones with the helpers of `GeoZoneTestHelpers.h`.
`GeoZoneBenchmark` doesn't use these files, it generates its zones.

The seeds of `fuzz/seeds/` are synthetic: `json/wfs_empty.json` (an empty WFS response), `json/faa_tfr_composite.json`
(the TFRs of `faa_tfr_test.json` stored with a written NOTAM detail of 6/4240, as feeds with companion documents are
stored)
and `xml/exception_report.xml` (an OGC exception report). They were added in e28d3691f.

## Fuzzing

The parsers read untrusted files and feeds, so `fuzz/` has libFuzzer targets for them:

- `GeoZoneFuzzJson`: JSON documents with each format hint, and in another language
- `GeoZoneFuzzXml`: XML documents like AIXM files and web service responses
- `GeoZoneFuzzZip`: zip archives and packages, read like the loader reads them

Their seed corpora are the files of `data/` and `fuzz/seeds/`, and zip packages of the test files made when
configuring. The fuzz project can be configured on its own with clang, which builds only GeoZoneCore and the targets
with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S test/GeoZone/fuzz -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_PREFIX_PATH=<Qt>/6.8.3/gcc_64
cmake --build build-fuzz
cd build-fuzz && mkdir -p work/json && ./GeoZoneFuzzJson -max_total_time=60 work/json corpus/json
```

Inputs found by the fuzzer are added to `work/json`, crashes are written to the current directory. When clang selects
a GCC installation without its C++ library, pass `--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/<version>` in
`CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS`.

In the QGroundControl build, `-DGEOZONE_FUZZ=ON` adds the targets too. With compilers other than clang they are built
with a driver which runs each file of a corpus once instead of fuzzing. `ctest --test-dir build/test -L fuzz` runs the seed corpora
through the targets in both cases.

## Validator

`tools/` has `geozone-validate`, which checks geozone files and zip packages like QGroundControl loads them and
prints their format, zones, warnings, altitude limits and schedules, see the Testing section of the developer guide
(`docs/en/qgc-dev-guide/geozones.md`). It shares the reading of files and directories with the fuzz driver.

## Benchmarks

`GeoZoneBenchmark` measures the core with 10000 synthetic zones over central Europe, about the size of a large
national dataset: parsing the 5 MB ED-269 file, restoring it as a stored source at startup by parsing it and writing
its parse cache or from the cache (with the stored copy hashed when its time changed), loading it into a source,
rebuilding the catalog with its index, 1000 `zonesAt` and 1000 `zonesInArea` queries of 10 km areas, evaluating a
mission of 500 waypoints over 1000 km, searching the routes around the zones of 50 legs of 20 km with
`GeoZoneRouteSearch`, and the detour of a leg through a maze of 1200 zones (`_mazeFeatures` of
`GeoZoneTestHelpers.h`), the worst case of the search. It takes a few seconds and is a test of its own, labeled `benchmark`, which the sanitizer workflow doesn't run:

```sh
ctest --test-dir build/test -L benchmark -V        # or ./GeoZoneBenchmark, with QtTest options like -iterations 20
```

Debug builds of the core are several times slower than release builds, compare measurements of the same build type.
