# recursive-shared-mutex

[![CI](https://github.com/yanxijian/recursive-shared-mutex/actions/workflows/ci.yml/badge.svg)](https://github.com/yanxijian/recursive-shared-mutex/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

C++17 **header-only** recursive `shared_mutex` for migrating codebases from `std::recursive_mutex` to reader/writer locking without immediately flattening every nested call.

```cpp
#include <recursive_shared_mutex/recursive_shared_mutex.hpp>
#include <mutex>
#include <shared_mutex>

rsm::RecursiveSharedMutex mtx;

void write() {
  std::unique_lock lock(mtx);  // exclusive, re-entrant
  // ...
}

void read() {
  std::shared_lock lock(mtx);  // shared, re-entrant
  // ...
}
```

## Features

- Compatible with `std::unique_lock` / `std::shared_lock` (C++17 *SharedMutex* surface)
- Per-thread **and** per-mutex recursion counts (POD slot table keyed by the mutex address; slots recycled on unlock, no heap allocation)
- Write→read nesting allowed; read→write upgrade is rejected (`std::system_error`)
- `try_lock` / `try_lock_shared`
- `CacheAlignedRecursiveSharedMutex` for sharded arrays: same state machine, 64-byte aligned instance — **2× throughput** when several mutexes sit next to each other (see below)
- Header-only, CMake `INTERFACE` library (`rsm::recursive_shared_mutex`)

## Sharded arrays: use the cache-aligned variant

The default instance is 24 bytes, so ~2.6 of them share one 64-byte cache line.
When several mutexes live next to each other (a sharded array/vector), the shards
false-share the owner/counter updates. Measured with 8 shards and 16 threads
(0 % / 15 % / 50 % writes, MSVC and MinGW):

| | plain `RecursiveSharedMutex` | `CacheAlignedRecursiveSharedMutex` |
|---|---:|---:|
| total ops/s | 13.9–14.3 M | **31.3–33.2 M** (×2.2–2.4) |

```cpp
std::vector<rsm::CacheAlignedRecursiveSharedMutex> shards(8);
```

It is a separate type instead of the default because 64 bytes per instance is
wasteful when a mutex is embedded in a per-object struct. Reproduce with
`bench_rsm --impl rsm_padded --mode sharded`.

## Compile-time knobs

| Macro | Default | Effect |
|---|---|---|
| `RSM_SHARED_SLOT_COUNT` | `256` | Per-thread cap on tracked mutexes; one slot is 16 B of TLS, so the default costs 4 KB per thread. Lower it for thread-heavy targets. |
| `RSM_VERSION_MAJOR/MINOR/PATCH`, `RSM_VERSION` | `0/2/1` | Header version, for consumers that only see the file (FetchContent). Kept in sync with `project(VERSION)`. |
| `RSM_NO_DEBUG_ASSERT` | off | Disables the debug-only "destroyed while locked" assertion. |
| `RSM_NO_THREAD_MIGRATION_NOTICE` | off | Silences the C++20 coroutine-mode notice. |

## Benchmarks vs `std::recursive_mutex`

See [benchmarks/README.md](benchmarks/README.md). Checked-in numbers:

- [MSVC](benchmarks/results/msvc.md) — read-heavy scenarios about **6×–14×** total ops/s when readers use shared locks; exclusive-only overhead about **0.93×**
- [MinGW-GCC](benchmarks/results/mingw-gcc.md) — shared-read gains are much smaller / writer-starved on this toolchain; validate on your release compiler

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks/run_bench.ps1 -Toolchain both -Seconds 3
```

## Requirements

- C++17 compiler (MSVC, GCC, Clang)
- CMake ≥ 3.16 (for build/tests/install)

## Quick start

### CMake (recommended)

```cmake
add_subdirectory(recursive-shared-mutex) # or FetchContent
target_link_libraries(your_target PRIVATE rsm::recursive_shared_mutex)
```

FetchContent example:

```cmake
include(FetchContent)
FetchContent_Declare(
  recursive_shared_mutex
  GIT_REPOSITORY https://github.com/yanxijian/recursive-shared-mutex.git
  GIT_TAG v0.2.1
)
FetchContent_MakeAvailable(recursive_shared_mutex)
target_link_libraries(your_target PRIVATE rsm::recursive_shared_mutex)
```

### Build & test

```bash
cmake -S . -B build -DRSM_BUILD_TESTS=ON -DRSM_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Semantics (short)

| Situation | Behavior |
|-----------|----------|
| Same thread re-enters `lock` / `lock_shared` | Allowed |
| Holds exclusive, then `lock_shared` | Allowed |
| Holds shared, then `lock` | Throws `resource_deadlock_would_occur` |
| Holds shared, then `try_lock` | Returns `false` |
| Unlock on a different OS thread | Unsupported (TLS) |

Exclusive recursion uses an atomic owner id on the instance; shared recursion uses a `thread_local` intrusive list (not `unordered_map`) keyed by the mutex address and released on thread exit.

See [docs/migration.md](docs/migration.md) for the migration guide and test matrix.

## Writer-starvation observability (R2)

MinGW (and glibc's reader-preferring `pthread_rwlock`) can starve writers under
read pressure. An optional instrumented guard measures write wait and hold
times without touching the core component:

```cpp
#include <recursive_shared_mutex/instrumented_write_lock.hpp>

// Install once at startup (must outlive all guards):
rsm::metrics::AtomicStatsRecorder stats;
rsm::metrics::set_recorder(&stats);

// Hot path:
{
    rsm::metrics::instrumented_write_lock guard(mtx); // records wait + hold
    // ... exclusive work ...
}
// stats.snapshot() -> feed rsm_write_wait_seconds / rsm_write_hold_seconds
```

Zero cost when no recorder is installed; see
`docs/roadmap.md` R2 for the metric/alerting contract.

## Limitations

- The per-thread TLS slot table is per-module (header-only): keep one mutex's `lock_shared`/`unlock_shared` sequence within a single module's TLS domain when DLLs/EXE boundaries are involved, or host the TLS state in a single exported module.
- At most `kSharedSlotCount` (256) shared locks may be held concurrently by one thread; the next `lock_shared` throws `resource_unavailable_try_again`. Slots recycle on unlock, so sequentially touching any number of mutexes is fine. Note that `try_lock_shared` also throws on a full table (it only returns `false` on contention) — unlike the usual `try_*` convention.
- No lock upgrade (shared → exclusive): `lock()` throws `resource_deadlock_would_occur` by design; audit read-then-write call chains before moving readers to `shared_lock`.
- `std::shared_mutex` underneath has no writer preference: writers may starve under continuous readers (see benchmarks S1/S5).

## Project layout

```text
include/recursive_shared_mutex/   # public headers
tests/                            # Catch2 tests
examples/                         # small usage demos
docs/                             # design & migration notes
cmake/                            # package config
.github/                          # CI, issue/PR templates
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Please open an issue before large design changes.

## Security

See [SECURITY.md](SECURITY.md).

## License

[MIT](LICENSE) © 2026 yanxijian
