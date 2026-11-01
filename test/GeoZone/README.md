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
