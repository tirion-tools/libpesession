# libpesession

Clean-room reader for SentryOne / SolarWinds Plan Explorer `.pesession` archives. Includes a lossless converter to `.osession` (the open SQLite container).

Maintained by [Tirion](https://tirion.tools). Used by [Calliper](https://github.com/tirion-tools/calliper). MIT.

Built clean-room from public Microsoft NRBF and ShowPlanXML specifications. No SolarWinds binary is referenced.

## Status

1.0. Stable API.

## What it reads

A `.pesession` is a ZIP with per-item entries:

| Entry | Contents |
|---|---|
| `<n>.queryanalysis` | NRBF blob: TraceRowEx, QueryStats, WaitAggregate, PlanData, ConnectionParameters, batch text, Index Analyzer JSON |
| `<n>.runtime` | Per-statement aggregates (cpu_ms, elapsed_ms, reads, writes, row_count) keyed by source offset |
| `<n>.queryplan` | Backup merged showplan XML (optional) |
| `index.json` | Per-item metadata |

## Surface

| Function | Purpose |
|---|---|
| `read_pesession_index(path)` | List every item |
| `read_pesession_runtime(path, file_number)` | Per-statement aggregates |
| `read_pesession_traces(path, file_number)` | TraceRowEx + QueryStats + WaitAggregate + PlanData |
| `read_pesession_connection_params(path, file_number)` | ConnectionParameters (auth, server version, login) |
| `read_pesession_batch_text(path, file_number)` | Raw user-submitted batch SQL |
| `read_pesession_index_analyzer_gz(path, file_number)` | Gzipped JSON for the Index Analyzer panel |
| `extract_pesession_xml_blocks(path, file_number)` | All `<ShowPlanXML>` blobs in document order |
| `convert_pesession_to_osession(in, out)` | Lossless conversion |

## Layout

```
libpesession/
├── include/pesession/
│   ├── pesession.hpp
│   └── pesession_to_osession.hpp
├── src/
│   ├── pesession.cpp
│   └── pesession_to_osession.cpp
├── tests/
│   └── test_smoke.cpp
├── CMakeLists.txt
├── LICENSE
└── README.md
```

## Dependencies

- [`libshowplan`](https://github.com/tirion-tools/libshowplan)
- [`libnrbf`](https://github.com/tirion-tools/libnrbf)
- [`libosession`](https://github.com/tirion-tools/libosession) (converter target)
- nlohmann/json (fetched via CMake `FetchContent`)
- miniz (vendored inside libosession)

Expected sibling layout:

```
<tree>/
├── libshowplan/
├── libnrbf/
├── libosession/
└── libpesession/
```

CMake resolves `../libshowplan` etc. by default. Override with `-DSHOWPLAN_DIR=…` / `-DNRBF_DIR=…` / `-DOSESSION_DIR=…`.

## Building

```bash
cd libpesession
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

CMake integration:

```cmake
add_subdirectory(path/to/libpesession)
target_link_libraries(my_app PRIVATE pesession::pesession)
```

## License

MIT. Issues at [github.com/tirion-tools/libpesession](https://github.com/tirion-tools/libpesession).
