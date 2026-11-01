# Geozone tests

The application tests run with `QGroundControl --unittest:<Class>` (GeoZoneParserTest, GeoZoneGeometryTest,
GeoZoneManagerTest, GeoZonePlanningTest, GeoZoneMonitorTest, GeoZoneFlightTest). The parser and geometry tests also
run headless in `GeoZoneCoreTest`, which only links the GeoZoneCore library (`ctest -R GeoZoneCore`).

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
