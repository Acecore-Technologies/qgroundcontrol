# Geozone tests

The application tests run with `QGroundControl --unittest:<Class>` (GeoZoneParserTest, GeoZoneGeometryTest,
GeoZoneManagerTest, GeoZonePlanningTest, GeoZoneMonitorTest, GeoZoneFlightTest, GeoZoneReturnTest). They are
registered in `GeoZoneUnitTests.cc` and `CMakeLists.txt` here, not in QGroundControl's `UnitTestList.cc`. The parser,
geometry and return tests also run headless in `GeoZoneCoreTest`, which only links the GeoZoneCore library
(`ctest -R GeoZoneCore`).

Tests which depend on the time or the terrain set them with `GeoZoneTestHelpers::ScopedEnvironment`, whose clock
stands still until the test advances it and whose terrain is unknown until set.

`tst_geozone_qml` runs the Qt Quick tests of `qml/` with the offscreen platform (`ctest -R tst_geozone_qml`). It
registers the GeoZone enums and loads the components from their sources, without QGroundControl. Only GeoZoneStyle is
tested: the other components use the singletons of the application, like ScreenTools, QGCPalette, the controls of
QGroundControl.Controls and GeoZoneManager or GeoZoneMonitor, which need the application or a large set of mocks.

## Test data

The files of `data/` are listed in `GEOZONE_TEST_DATA` of `CMakeLists.txt` and compiled into QGroundControl and
`GeoZoneCoreTest` as `:/unittest/<file>`. They are also seeds of the fuzzers, and some of them are packed into the zip
seeds made when configuring them. The provenance of the extracts of real data is only known as far as stated below:
where it isn't recorded, check the publisher's terms before redistributing the file outside this repository. Add new
fixtures to this list with their source, terms and tests.

| File | Content | Source | Licence / terms | Used by |
| ---- | ------- | ------ | --------------- | ------- |
| `ch_zurich_ed269.json` | 8 Swiss zones around Zurich in ED-269: substations, prisons, the airports LSZH and LSMD and the CTRs of Zurich and Dübendorf | Extract of the Swiss UAS Geozones (`ch.bazl.einschraenkungen-drohnen`) of the Federal Office of Civil Aviation FOCA, `https://data.geo.admin.ch/ch.bazl.einschraenkungen-drohnen/`, published 2026-10-31: the unmodified zones intersecting latitude 47.3653 to 47.4332 and longitude 8.4858 to 8.5787, as described in the file. Added in 596a12cb3. | Open data, opendata.swiss "Open use. Must provide the source" (stated in the file's `description`, which must be kept) | GeoZoneParserTest (`_testParseFixtures`, `_testParserFuzz`), GeoZoneFlightTest (`_testFlightAlertSequence`, `_testMissionCrossedZones`, `_testMockLinkVehicle`) |
| `dipul_test.json` | 4 German zones of a DiPUL WFS response: a control zone of Frankfurt, the Holzdorf restricted area, the temporary ED-R 18 Ahaus and a hospital, with the WFS counts and `timeStamp` | Apparently an excerpt of the DiPUL WFS of DFS / BMDV (layer and feature ids, legal references and UUIDs as published); whether features were trimmed or edited isn't recorded. Added in 94ecc5dff. | DiPUL data is licensed CC BY-ND 4.0 (user guide); unknown for this excerpt, which may be modified — verify before redistributing | GeoZoneParserTest (`_testParseFixtures`, `_testParseNationalFormats`) |
| `ed269_test.json` | 4 ED-269 features: a prohibited zone with two polygon layers, a circle requiring authorisation, a conditional zone with a schedule, and a feature without geometry; UTF-8 byte order mark | Synthetic, written for these tests (its `description` says so). Added in 57c0f41b7. | QGroundControl's licence (`COPYING.md`) | GeoZoneParserTest (`_testParseFixtures`, `_testParseED269`, `_testParserFuzz`, `_testDisplayText`, `_testXmlAndPackages`, `_testParseCacheValidation`), GeoZoneGeometryTest (`_testIsActiveAt`, `_testIsActiveDuring`, `_testExclusionShapes`), GeoZoneManagerTest (`_testManagerLoadPersistClear`, `_testManagerBackgroundLoad`, `_testFeedNotModified`, `_testOfflinePack`), GeoZonePlanningTest (`_testGeoFenceImport`) |
| `ed318_test.json` | 4 ED-318 features with dataset metadata: a circle (`Point` extent) with localized names, a multipolygon, a U-space geometry collection and an unsupported `LineString` | Synthetic, written for these tests (provider "Test Authority", `example.com` contacts). Added in 57c0f41b7, extended in 655ef4548. | QGroundControl's licence (`COPYING.md`) | GeoZoneParserTest (`_testParseFixtures`, `_testParseED318`, `_testParserFuzz`, `_testParseCacheValidation`), GeoZoneGeometryTest (`_testIsActiveAt`, `_testExclusionShapes`), GeoZoneManagerTest (`_testManagerLoadPersistClear`, `_testManagerStoreFailures`, `_testManagerBackgroundLoad`, `_testFeedCancellation`, `_testFeedNotModified`, `_testSourceValidity`, and the feed tests through `TestFeedServer`, which serves it at `/ok`), GeoZonePlanningTest (`_testGeoFenceImport`) |
| `faa_tfr_test.json` | 2 FAA TFRs of the `V_TFR_LOC` WFS layer (NOTAMs 6/4240 San Angelo and 5/6914 South Padre Island, TX), circles published as 36-sided polygons | Apparently an excerpt of the tfr.faa.gov GeoServer WFS response; not recorded. The NOTAM details the tests pair with it are written in the tests. Added in 596a12cb3. | unknown — verify before redistributing | GeoZoneParserTest (`_testParseFixtures`, `_testParseFaaTfr`, `_testParserFuzz`), GeoZoneManagerTest (`_testFeedCompanions`) |
| `france_uas_test.json` | 2 French open category restrictions ("Vol interdit" and a 50 m maximum height) in a WFS response limited to 2 of 10 features | Feature ids, properties and remark of the IGN Géoplateforme layer `carte_restriction_drones_lf`, with synthetic rectangular geometries near Paris and WFS counts set to simulate a limited response. Added in 94ecc5dff. | unknown — verify before redistributing | GeoZoneParserTest (`_testParseFixtures`, `_testParseNationalFormats`), GeoZoneManagerTest (`_testWfsPaging`) |
| `uk_aixm_test.xml` | 3 UK airspaces in an AIXM 5.1 basic message: the Blackbushe FRZ (`EGR1U021A`, arcs and geodesic strings), Springfields (`EGR312`, a circle) and the Raasay danger area (`EGD710`, line strings and geodesic strings) | Apparently an excerpt of the NATS UAS AIXM 5.1 dataset (snapshot of 2026-10-29); not recorded. Added in 94ecc5dff. | unknown — verify before redistributing | GeoZoneParserTest (`_testParseFixtures`, `_testParseNationalFormats`, `_testRejectedAixmGeometry`, `_testXmlAndPackages`, `_testParseCancel`), GeoZoneManagerTest (`_testFeedDatasetLink`) |

GeoZoneParserTest and GeoZoneGeometryTest also run in `GeoZoneCoreTest`, without the parts which need the application.
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
with a driver which runs each file of a corpus once instead of fuzzing. `ctest -L fuzz` runs the seed corpora
through the targets in both cases.

## Benchmarks

`GeoZoneBenchmark` measures the core with 10000 synthetic zones over central Europe, about the size of a large
national dataset: parsing the 5 MB ED-269 file, restoring it as a stored source at startup by parsing it and writing
its parse cache or from the cache (with the stored copy hashed when its time changed), loading it into a source,
rebuilding the catalog with its index, 1000 `zonesAt` and 1000 `zonesInArea` queries of 10 km areas, evaluating a
mission of 500 waypoints over 1000 km, and searching the routes around the zones of 50 legs of 20 km with
`GeoZoneRouteSearch`. It takes a few seconds and is a test of its own, labeled `benchmark`, which the sanitizer workflow doesn't run:

```sh
ctest -L benchmark -V          # or ./GeoZoneBenchmark, with QtTest options like -iterations 20
ctest -LE "benchmark|fuzz"     # the other tests
```

Debug builds of the core are several times slower than release builds, compare measurements of the same build type.
