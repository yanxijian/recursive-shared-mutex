# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- The CMake package reported version `0.1.0` while releases were at `v0.2.1`, so `find_package(recursive_shared_mutex 0.2 REQUIRED)` failed against an installed tree. The project version (and therefore `recursive_shared_mutexConfigVersion.cmake`) now tracks the released version.
- README described the shared-recursion state as a `thread_local` intrusive list "released on thread exit", which stopped being true in v0.2.0 (dense POD slot table, no thread-exit release). Corrected, together with the performance summary: MSVC/clang are at 0.9×–1.2× of `std::recursive_mutex` on the exclusive path, MinGW-GCC at 0.07×–0.2×, with the toolchain-level cause spelled out.

### Changed

- `try_lock_shared()` returns `false` instead of throwing when the per-thread slot table is full. `SharedLockable` requires `try_lock_shared` not to throw, and the previous behaviour made `std::shared_lock(mtx, std::try_to_lock)` non-conforming. `lock_shared()` still throws `resource_unavailable_try_again` on exhaustion.
- `RSM_BUILD_TESTS` / `RSM_BUILD_EXAMPLES` default to `ON` only when the project is top-level, so `add_subdirectory()` / `FetchContent_MakeAvailable()` consumers no longer download Catch2 and build two test binaries by default.
- Documented that a mismatched `unlock`/`unlock_shared` ends in `std::terminate` (the throwing call is reached from `noexcept` RAII destructors), that `std::condition_variable_any` is unsafe once recursion depth > 1, and that a cross-module shared-lock sequence is undefined behaviour rather than just a stranded slot.

### Added

- `tests/backend_contract.cpp`: framework-free check that the current toolchain's `std::shared_mutex` actually honours mutual exclusion. MinGW-w64 GCC 13.2 over winpthreads fails it (~1/1200 successful `try_lock` acquisitions grant two owners).
- `tests/mutual_exclusion.cpp`: framework-free check that the library keeps writers mutually exclusive and readers out while a writer is inside — a property the existing suite did not assert.
- `.github/workflows/platform-matrix.yml`: cross-platform CI matrix (MSVC x64/x86, clang-cl, MinGW-UCRT64, gcc-11/13, clang+libstdc++, clang+libc++, macOS arm64/x86_64, Linux arm64, alpine/musl, TSAN/ASAN+UBSAN, C++20 notice assertion, and a shared-library TLS-domain job). The backend contract test runs first in every cell.
- `examples/cross_module_hazard/`: a real DLL/.so reproducer for the per-module TLS table, showing that the same thread re-acquires the backend inside the second module (undefined behaviour that is silent on SRWLOCK and on reader-preferring rwlocks).
- `docs/platform-notes.md`: measured three-toolchain data (MinGW-GCC 13.2 / MSVC 19.51 / clang 22.1), the backend contract results, and reproduction commands.

### Added

- Writer-starvation observability (roadmap R2): optional opt-in header `recursive_shared_mutex/instrumented_write_lock.hpp` with an RAII `instrumented_write_lock` guard that measures write WAIT (blocked acquisition) and HOLD (exclusive critical section) and reports them to a pluggable `Recorder` sink (built-in `AtomicStatsRecorder`: count/total/max, atomics only). Zero cost when no recorder is installed (even clock reads are skipped), core component untouched, constructor-failure (fail-fast) records nothing, destructor is noexcept, and sink exceptions are swallowed. Intended to feed `rsm_write_wait_seconds` / `rsm_write_hold_seconds` histograms (see `docs/roadmap.md` R2).
## [v0.2.1] - 2026-10-16

### Performance

- The per-thread shared-slot table now keeps active slots dense in `[0, used)` with swap-with-last compaction on unlock-to-zero (and in the defensive destructor path). `findActiveSlot` scans only the active prefix, so threads holding no shared slots skip the scan entirely; the `depth == 0` slot-reuse branch became unreachable and was removed.
- Measured on MinGW-w64 GCC 13.2 (UCRT): exclusive `lock()+unlock()` 365.6 → 108.6 ns/pair (**3.4×**), cold shared acquire/release churn 367.1 → 152.6 ns/pair (**2.4×**). Semantics verified unchanged by the full boundary suite (1051 checks) and the test matrix.
- The dense-slot invariant and the POINTER-STABILITY red line (slot pointers are used across the blocking `mtx_.lock_shared()` call; safe only because a blocked call suspends the whole thread) are documented in the header. The rejected `release_shared_unchecked()` escape valve (unowned double-release UB hazard) is documented in `docs/migration.md`.

## [0.2.0] - 2026-10-16

### Fixed

- Shared-recursion TLS state is now keyed by the mutex address instead of a per-module `id_` counter: the inline per-instance counter was duplicated per DLL/EXE, so mutexes created in different modules could collide on the same `id_` and fake re-entrancy across locks.
- The shared-recursion TLS state is a per-thread table of POD slots (constant-initialized, no heap allocation, no thread_local destructor). This removes three failure modes at once: freeing memory inside a thread_local destructor corrupted the heap at thread exit on MinGW-w64 GCC 13.2 ucrt/posix (verified with gdb: SIGSEGV in `RtlFreeHeap` on the `LdrShutdownThread` path); the previous heap-node design could strand a shared slot if the node allocation failed after the underlying lock was taken; and nodes leaked for the lifetime of the thread. Slots are recycled as soon as a shared depth drops to zero.
- `lock_shared`/`try_lock_shared` fail with `resource_unavailable_try_again` BEFORE taking the underlying lock when the per-thread slot table (`kSharedSlotCount`, 256) is exhausted or the test hook injects a failure, so an error can never strand a shared slot.

### Changed

- Cross-module (DLL/EXE) constraint documented: a single mutex's `lock_shared`/`unlock_shared` sequence must stay within one module's TLS domain, or the TLS state must be hosted in a single exported module (`docs/migration.md`).
- Migration hard rules expanded: read-then-write audit is now a hard gate before moving readers to `shared_lock`; writer starvation of `std::shared_mutex` is documented as a first-class risk.
- `try_lock_shared` throws `resource_unavailable_try_again` when the per-thread slot table is full (it only returns `false` on contention), unlike the usual `try_*` convention.

### Added

- Benchmarks vs `std::recursive_mutex` (`benchmarks/`) with MSVC and MinGW-GCC result tables, presented with speedup bar charts and per-scenario verdicts.
- Test hooks under `RSM_ENABLE_TEST_HOOKS` (test target only): one-shot shared-acquire failure injection. New test cases E1/E2/E3/E5 cover acquire-failure safety, contention behavior, and slot recycling/cap enforcement.

## [0.1.0] - 2026-10-15

### Added

- Initial `rsm::RecursiveSharedMutex` header-only implementation (C++17)
- CMake INTERFACE package with install/export support
- Catch2 unit tests (single-thread, multi-instance, concurrent, try paths)
- Example `basic_usage`
- Migration guide under `docs/migration.md`
- GitHub Actions CI (Windows / Ubuntu / macOS)
- MIT license and standard community health files

[Unreleased]: https://github.com/yanxijian/recursive-shared-mutex/compare/v0.2.1...HEAD
[v0.2.1]: https://github.com/yanxijian/recursive-shared-mutex/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/yanxijian/recursive-shared-mutex/releases/tag/v0.2.0
[0.1.0]: https://github.com/yanxijian/recursive-shared-mutex/releases/tag/v0.1.0
