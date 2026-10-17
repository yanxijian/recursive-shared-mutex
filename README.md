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
- Header-only, CMake `INTERFACE` library (`rsm::recursive_shared_mutex`)

## Benchmarks vs `std::recursive_mutex`

See [benchmarks/README.md](benchmarks/README.md) for the harness and
[docs/platform-notes.md](docs/platform-notes.md) for the cross-toolchain picture.

| Exclusive-only path (their S7 shape) | `recursive_mutex` | this library | ratio |
|---|---:|---:|---:|
| MSVC (SRWLOCK) | 8.17 M ops/s | 9.53 M ops/s | **1.17×** |
| clang-cl / clang with MSVC STL | 8.07 M ops/s | 11.47 M ops/s | **1.42×** |
| MinGW-GCC 13.2 (libstdc++/winpthreads) | 4.99 M ops/s | 0.34 M ops/s | **0.067×** |

- **MSVC / clang with the MSVC STL**: the wrapper costs about 1 ns per lock
  pair (5.8 ns vs 3.3 ns for the raw backend); exclusive-only overhead is
  **0.9×–1.2×**, and read-heavy workloads gain roughly 6×–14× total throughput.
- **MinGW-GCC**: 0.07×–0.2×, i.e. a 5×–15× **regression** on the exclusive path.
  The cause is the toolchain, not the design: libstdc++'s `std::shared_mutex`
  over winpthreads is ~11× slower than a native `SRWLOCK`, and every
  `thread_local` access goes through emutls. Worse, `try_lock()` on that backend
  is not mutually exclusive (see [docs/platform-notes.md](docs/platform-notes.md)).
  **Do not migrate on MinGW without replacing the backend first.**

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
| `unlock` without ownership, or while nested shared is still held | Throws `operation_not_permitted` |
| Per-thread slot table exhausted | `lock_shared` throws `resource_unavailable_try_again`; `try_lock_shared` returns `false` |
| Unlock on a different OS thread | Unsupported (TLS) |
| Unlock crossing a DLL/EXE boundary | Unsupported; the second module's `lock_shared` re-enters the backend = UB |

Exclusive recursion uses an atomic owner id on the instance; shared recursion
uses a `thread_local` table of POD slots keyed by the mutex address, kept dense
in `[0, used)`, recycled on unlock and never freed at thread exit.

> **Misuse terminates, it does not throw.** The `unlock` cases above throw
> `std::system_error`, but those calls also come from `std::unique_lock` /
> `std::shared_lock` destructors, which are `noexcept`: a mismatched pair ends
> the process (`std::terminate`). That is deliberate fail-fast, but it is not
> catchable — see the header comment.

This component is only as correct as the `std::shared_mutex` underneath it.
[tests/backend_contract.cpp](tests/backend_contract.cpp) verifies that contract
on the current toolchain; run it before trusting a new compiler/stdlib.

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

- The per-thread TLS slot table is per-module (header-only): keep one mutex's `lock_shared`/`unlock_shared` sequence within a single module's TLS domain when DLLs/EXE boundaries are involved, or host the TLS state in a single exported module. Violating this is undefined behaviour, not just a stranded slot — [examples/cross_module_hazard](examples/cross_module_hazard) reproduces it with a real DLL/.so.
- At most `kSharedSlotCount` (256) shared locks may be held concurrently by one thread (≈4 KB of TLS per thread). The next `lock_shared` throws `resource_unavailable_try_again`; `try_lock_shared` returns `false`. Slots recycle on unlock, so sequentially touching any number of mutexes is fine.
- No lock upgrade (shared → exclusive): `lock()` throws `resource_deadlock_would_occur` by design; audit read-then-write call chains before moving readers to `shared_lock`.
- `std::shared_mutex` underneath has no writer preference: writers may starve under continuous readers (see benchmarks S1/S5).
- `std::condition_variable_any` is unsafe once recursion depth > 1: `wait()` releases only one level, so the mutex stays held and the waiter never wakes.
- The backend must honour mutual exclusion. MinGW-w64 GCC 13.2 (libstdc++/winpthreads) does not: `try_lock()` can grant two owners, which surfaces as spurious `operation_not_permitted` from `unlock()` inside this library. Run [tests/backend_contract.cpp](tests/backend_contract.cpp) on your toolchain; details in [docs/platform-notes.md](docs/platform-notes.md).

## Project layout

```text
include/recursive_shared_mutex/   # public headers
tests/                            # Catch2 tests + framework-free regression tests
examples/                         # usage demos (incl. the cross-module hazard repro)
docs/                             # design, migration & platform notes
cmake/                            # package config
.github/                          # CI, issue/PR templates
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Please open an issue before large design changes.

## Security

See [SECURITY.md](SECURITY.md).

## License

[MIT](LICENSE) © 2026 yanxijian
