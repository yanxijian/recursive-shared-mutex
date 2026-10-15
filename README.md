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
  GIT_TAG v0.1.0
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

## Limitations

- The per-thread TLS slot table is per-module (header-only): keep one mutex's `lock_shared`/`unlock_shared` sequence within a single module's TLS domain when DLLs/EXE boundaries are involved, or host the TLS state in a single exported module.
- At most `kSharedSlotCount` (256) shared locks may be held concurrently by one thread; the next `lock_shared` throws `resource_unavailable_try_again`. Slots recycle on unlock, so sequentially touching any number of mutexes is fine.
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
