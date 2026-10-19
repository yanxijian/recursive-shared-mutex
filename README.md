# recursive-shared-mutex

**English** · [简体中文](README.zh-CN.md)

[![CI](https://github.com/yanxijian/recursive-shared-mutex/actions/workflows/ci.yml/badge.svg)](https://github.com/yanxijian/recursive-shared-mutex/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A C++17 header-only recursive `shared_mutex`. It lets a codebase move from
`std::recursive_mutex` to reader/writer locking without first flattening every
nested call.

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

## Contents

- [Features](#features)
- [Sharded arrays](#sharded-arrays-use-the-cache-aligned-variant)
- [Compile-time knobs](#compile-time-knobs)
- [Benchmarks vs std::recursive_mutex](#benchmarks-vs-stdrecursive_mutex)
- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Semantics](#semantics-short)
- [Writer-starvation observability](#writer-starvation-observability-r2)
- [Limitations](#limitations)
- [Project layout](#project-layout)
- [Contributing](#contributing)
- [Security](#security)
- [License](#license)

## Features

- Works with `std::unique_lock` and `std::shared_lock` (C++17 *SharedMutex*
  surface; see [Limitations](#limitations) for the one exception).
- Recursion is counted per thread and per mutex. The counters live in a POD slot
  table keyed by the mutex address; slots are recycled on unlock, with no heap
  allocation.
- Write→read nesting is allowed. Read→write upgrade throws `std::system_error`.
- `try_lock` and `try_lock_shared`.
- `is_held_exclusive()` for debug assertions. It is true only for the owning
  thread and false for a shared holder, which `!try_lock()` cannot distinguish.
- `CacheAlignedRecursiveSharedMutex` for sharded arrays: same state machine, but
  64-byte aligned. Roughly twice the throughput when several mutexes sit next to
  each other.
- Header-only CMake `INTERFACE` target: `rsm::recursive_shared_mutex`.

## Sharded arrays: use the cache-aligned variant

The default instance is 24 bytes, so about 2.6 of them share one 64-byte cache
line. When several mutexes sit next to each other in an array or vector, the
shards false-share their owner and counter updates. Measured with 8 shards and
16 threads at 0 %, 15 % and 50 % writes, on MSVC and MinGW:

| | plain `RecursiveSharedMutex` | `CacheAlignedRecursiveSharedMutex` |
|---|---:|---:|
| total ops/s | 13.9–14.3 M | **31.3–33.2 M** (×2.2–2.4) |

```cpp
std::vector<rsm::CacheAlignedRecursiveSharedMutex> shards(8);
```

The aligned type is separate because 64 bytes per instance is wasteful when a
mutex is embedded in a per-object struct. Reproduce with
`bench_rsm --impl rsm_padded --mode sharded`.

## Compile-time knobs

| Macro | Default | Effect |
|---|---|---|
| `RSM_SHARED_SLOT_COUNT` | `256` | Per-thread cap on tracked mutexes. One slot is 16 B of TLS, so the default costs 4 KB per thread. It sizes a class member array, so it must be identical in every translation unit; never set it per target. |
| `RSM_VERSION_MAJOR/MINOR/PATCH`, `RSM_VERSION` | `0/4/1` | Header version for consumers that only see the file (FetchContent). Kept in sync with `project(VERSION)`. |
| `RSM_BACKEND_SRWLOCK` | on for MinGW, off elsewhere | Use the native Win32 SRWLOCK wrapper as the backend. Auto-selected on `__MINGW32__` because the alternative is unsound there. |
| `RSM_BACKEND_STD` | off | Force `std::shared_mutex` even on MinGW. Only useful to reproduce the winpthreads defect; a weekly CI probe does that. |
| `RSM_NO_DEBUG_ASSERT` | off | Disables the debug-only "destroyed while locked" assertion. |
| `RSM_NO_THREAD_MIGRATION_NOTICE` | off | Silences the C++20 coroutine-mode notice. |

`RSM_SHARED_SLOT_COUNT` is part of the class definition, not a private tuning
knob. Setting it on one target or one source file is an ODR violation (IFNDR):
the linker may merge one definition and leave other translation units with a
table of the wrong size. When the project is added through
`add_subdirectory()`/FetchContent, the cache variable propagates to every linked
target. An installed package carries the value it was built with, so every
consumer must use that value. Consumers that include the header directly must set
the macro globally.

## Benchmarks vs `std::recursive_mutex`

See [benchmarks/README.md](benchmarks/README.md) for the harness and
[docs/platform-notes.md](docs/platform-notes.md) for the cross-toolchain picture.

| Exclusive-only path (their S7 shape) | `recursive_mutex` | this library | ratio |
|---|---:|---:|---:|
| MSVC (SRWLOCK) | 8.17 M ops/s | 9.53 M ops/s | **1.17×** |
| clang-cl / clang with MSVC STL | 8.07 M ops/s | 11.47 M ops/s | **1.42×** |
| MinGW-GCC 13.2 + `RSM_BACKEND_STD` (libstdc++/winpthreads) | 4.99 M ops/s | 0.34 M ops/s | **0.067×** |
| MinGW-GCC 13.2 (default: native SRWLOCK) | 4.99 M ops/s | 7.54 M ops/s | **1.52×** |

> **Protocol:** 8 threads, `try_lock` + `yield` spin, nesting depth 3, median of 5
> interleaved 1 s rounds on one 32-core Windows host. The raw ops/s and the same
> table are in [docs/platform-notes.md](docs/platform-notes.md) §2 and §6.
>
> This is a different harness from [benchmarks/results](benchmarks/results)
> (3 s × 5 runs, real work between lock operations), so the ratios differ:
> that harness reports 2.59× (MSVC) and 1.32× (MinGW, native backend) for its S7
> shape, against 1.17× and 1.52× here. Both put the exclusive path at or above
> parity; the exact ratio is protocol-dependent, and neither transfers to another
> machine. Run-to-run noise on this host reaches 245 % on the read-heavy scenario
> (S1), so treat the numbers as order-of-magnitude.

- **MSVC / clang with the MSVC STL:** the wrapper costs about 1.3 ns per lock
  pair (4.6 ns vs 3.3 ns for the raw backend, both uncontended). The cost no
  longer depends on how many shared locks the thread holds elsewhere.
  Exclusive-only overhead is **1.17×–1.42×** in the 8-thread table (2.59× in the
  3 s × 5 desktop run), and read-heavy workloads gain roughly **6×–11×** total
  throughput.
- **MinGW-GCC with `RSM_BACKEND_STD`** (what the library used to pick by
  default): 0.07×–0.2×, a 5×–15× regression on the exclusive path. The cause is
  the toolchain, not the design. libstdc++'s `std::shared_mutex` over winpthreads
  has a backend floor about 11× higher than the SRWLOCK-based backends on the
  other toolchains (37.4 vs 3.3 ns uncontended); with the wrapper's TLS work the
  exclusive pair is 61.5 vs 24.2 ns, every `thread_local` access goes through
  emutls, and `try_lock()` on that backend is not mutually exclusive. MinGW
  therefore defaults to the native backend.
- **MinGW-GCC with the native backend (the default):** 62.7 → 24.2 ns per
  exclusive lock/unlock pair (2.6×), 64.4 → 25.8 ns for the shared pair (2.5×),
  and the exclusive-only ratio goes from 0.067× to **1.52×**, faster than
  `std::recursive_mutex`. The backend contract test passes again.

### MinGW-w64: the native backend is the default

```bash
g++ ...                            # nothing to add: MinGW gets the native backend
g++ -DRSM_BACKEND_STD ...          # opt back into std::shared_mutex (unsound there)
```

On MinGW-w64 the header selects `RSM_BACKEND_SRWLOCK` on its own, because the
alternative is not just slow but wrong. The switch replaces `std::shared_mutex`
with an 8-byte SRWLOCK wrapper (`rsm::detail::SrwLockBackend`, exposed as
`RecursiveSharedMutex::backend_type`). On MSVC it changes nothing, because
`std::shared_mutex` there already is SRWLOCK. It requires Windows Vista or later
and has no effect on other platforms. SRWLOCK is non-recursive in exclusive
mode, which is fine here: the class intercepts every recursion before touching
the backend.

Reproduce with [benchmarks/run_bench.ps1](benchmarks/run_bench.ps1), which reports
medians of `-Repeats` runs. See [benchmarks/README.md](benchmarks/README.md) for
why the spread matters.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks/run_bench.ps1 -Toolchain both -Seconds 3 -Repeats 3
```

## Requirements

- A C++17 compiler (MSVC, GCC or Clang).
- **C++ exceptions enabled.** The header reports misuse and slot-table exhaustion
  with `std::system_error`; with exceptions disabled it stops at a single
  `#error`. `cl.exe`, `g++` and `clang++` enable exceptions by default;
  **`clang-cl` does not**, so pass `/EHsc`. The CMake target adds `/EHsc` for
  MSVC-simulating compilers.
- CMake ≥ 3.16 for building, testing and installing.

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
  GIT_TAG v0.4.1
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
| `unlock` without ownership, or while a nested shared lock is still held | Throws `operation_not_permitted` |
| Per-thread slot table exhausted | `lock_shared` throws `resource_unavailable_try_again`; `try_lock_shared` returns `false` |
| Unlock on a different OS thread | Unsupported (TLS) |
| Unlock crossing a DLL/EXE boundary | Unsupported; the second module's `lock_shared` re-enters the backend = UB |

Exclusive recursion uses an atomic owner id on the instance. Shared recursion
uses a `thread_local` table of POD slots keyed by the mutex address, kept dense
in `[0, used)`, recycled on unlock and never freed at thread exit.

> **Misuse terminates; it does not throw to the caller.** The `unlock` cases above
> throw `std::system_error`, but those calls also come from `std::unique_lock` /
> `std::shared_lock` destructors, which are `noexcept`. A mismatched pair calls
> `std::terminate`. This is fail-fast and not catchable; see the header comment.

This component is only as correct as the `std::shared_mutex` underneath it.
[tests/backend_contract.cpp](tests/backend_contract.cpp) verifies that contract on
the current toolchain; run it before trusting a new compiler or standard library.

See [docs/migration.md](docs/migration.md) for the migration guide and test matrix.

## Writer-starvation observability (R2)

Any backend without writer preference can starve writers under read pressure;
glibc's `pthread_rwlock` and Win32 SRWLOCK are examples. An optional instrumented
guard measures write wait and hold times without touching the core component:

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

The guard costs nothing when no recorder is installed. See `docs/roadmap.md` R2
for the metric and alerting contract.

## Limitations

- The TLS slot table is per module. Keep one mutex's `lock_shared` /
  `unlock_shared` sequence inside a single module's TLS domain when DLLs or EXE
  boundaries are involved, or host the TLS state in one exported module.
  Violating this is undefined behaviour, not just a stranded slot;
  [examples/cross_module_hazard](examples/cross_module_hazard) reproduces it with
  a real DLL/.so.
- Destroying a mutex while the current thread still holds a shared lock is
  undefined behaviour. Debug builds assert. With `NDEBUG` or
  `RSM_NO_DEBUG_ASSERT`, the destructor silently drops this thread's slot as
  damage control, which does not make the destruction safe.
- One thread can hold at most `kSharedSlotCount` (256) shared locks at once,
  about 4 KB of TLS per thread. The next `lock_shared` throws
  `resource_unavailable_try_again`; `try_lock_shared` returns `false`. Slots
  recycle on unlock, so touching any number of mutexes sequentially is fine.
- There is no lock upgrade from shared to exclusive: `lock()` throws
  `resource_deadlock_would_occur`. Audit read-then-write call chains before
  moving readers to `shared_lock`.
- The underlying `std::shared_mutex` has no writer preference, so writers may
  starve under continuous readers (see benchmarks S1/S5).
- `std::condition_variable_any` is unsafe once recursion depth > 1: `wait()`
  releases only one level, so the mutex stays held and the waiter never wakes.
- The backend must honour mutual exclusion. MinGW-w64 GCC 13.2
  (libstdc++/winpthreads) does not: `try_lock()` can grant two owners, which
  surfaces as a spurious `operation_not_permitted` from `unlock()`. That is why
  MinGW defaults to the native backend and `RSM_BACKEND_STD` is opt-in. Run
  [tests/backend_contract.cpp](tests/backend_contract.cpp) on any new toolchain;
  details are in [docs/platform-notes.md](docs/platform-notes.md).
- **Not literally `Cpp17Lockable`.** `unlock()` and `unlock_shared()` throw
  `std::system_error` on misuse, while the named requirement says *Throws:
  Nothing* (`[thread.mutex.requirements.mutex]`). Generic algorithms only unlock
  what they acquired, so `std::lock`, `std::scoped_lock` and the two RAII wrappers
  work. A mismatched pair reached from a destructor calls `std::terminate`, as
  noted above.
- **MinGW pulls in `<windows.h>`.** The default native backend includes it, and
  that costs real compile time: 655 ms vs 207 ms for one translation unit on the
  machine these numbers come from (3.2×). `RSM_BACKEND_STD` avoids the include
  but is unsound on that toolchain, so there is no good way to opt out.

## Project layout

```text
include/recursive_shared_mutex/   # public headers
tests/                            # Catch2 tests + framework-free regression tests
examples/                         # usage demos (incl. the cross-module hazard repro)
benchmarks/                       # driver, CI automation, checked-in results
docs/                             # design, migration, platform notes, roadmap, validation
verification/                     # soak history, one row per recorded batch
scripts/repo_rules.py             # the rules AGENTS.md promises CI enforces (vendored RepoRules)
cmake/                            # package config
.github/                          # workflows, issue/PR templates, CONTRIBUTING / SECURITY / CODE_OF_CONDUCT
AGENTS.md                         # repository rules, one file for every agent
```

## Contributing

See [CONTRIBUTING.md](.github/CONTRIBUTING.md). Please open an issue before large
design changes.

The repository rules live in [AGENTS.md](AGENTS.md), one file for Claude Code,
Codex, Cursor, Copilot and anything else; `CLAUDE.md`, `.cursor/rules/repo.mdc`
and `.github/copilot-instructions.md` point at it. The mechanical rules (UTF-8 BOM
and CRLF, 140 columns, no `#pragma comment(lib)`, header hygiene, version sync,
changelog section, translated documents staying linked) run in CI on every push
through RepoRules (`scripts/repo_rules.py`), so they fail the build instead of waiting
for review. Run `python3 scripts/repo_rules.py fix --config repo-rules.json` to repair the
mechanical ones, and `git config core.hooksPath .githooks` to get them before the
commit.

## Security

See [SECURITY.md](.github/SECURITY.md).

## License

[MIT](LICENSE) © 2026 yanxijian
