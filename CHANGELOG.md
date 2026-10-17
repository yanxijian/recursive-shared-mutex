# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `rsm::CacheAlignedRecursiveSharedMutex`: a 64-byte-aligned instance for sharded arrays. The default instance is 24 bytes, so ~2.6 of them share a cache line and the shards false-share the owner/counter updates. Measured with 8 shards / 16 threads (0 %, 15 %, 50 % writes): 13.9–14.3 M → 31.3–33.2 M total ops/s on MSVC, 28.1 M → 57.3 M on MinGW. New benchmark implementation `--impl rsm_padded` (meaningful with `--mode sharded`).
- `RSM_VERSION_MAJOR/MINOR/PATCH` and `RSM_VERSION`: the header now carries its own version, for consumers that pull it with `FetchContent` and cannot see the CMake package.
- `RSM_SHARED_SLOT_COUNT` override: the per-thread slot table cap is now a macro (default unchanged at 256, i.e. 4 KB of TLS per thread), so thread-heavy or embedded targets can shrink it.
- `RSM_BUILD_STRESS` CMake option: builds `tests/stress_concurrent.cpp` as `rsm_stress` and registers it as a test. It used to be built by hand, which is how such drivers rot.
- Debug-only assertion in the destructor (`owner_` / exclusive counters non-zero) so "destroyed while locked" fails at the point of misuse instead of corrupting a later mutex at the same address. Disable with `RSM_NO_DEBUG_ASSERT`.
- Writer-starvation observability (roadmap R2): optional opt-in header `recursive_shared_mutex/instrumented_write_lock.hpp` with an RAII `instrumented_write_lock` guard that measures write WAIT (blocked acquisition) and HOLD (exclusive critical section) and reports them to a pluggable `Recorder` sink (built-in `AtomicStatsRecorder`: count/total/max, atomics only). Zero cost when no recorder is installed (even clock reads are skipped), core component untouched, constructor-failure (fail-fast) records nothing, destructor is noexcept, and sink exceptions are swallowed. Intended to feed `rsm_write_wait_seconds` / `rsm_write_hold_seconds` histograms (see `docs/roadmap.md` R2).

### Fixed

- The project version reported `0.1.0` while releases were at `v0.2.1`, so `find_package(recursive_shared_mutex 0.2 REQUIRED)` failed against an installed tree. `project(VERSION)` now matches the header's `RSM_VERSION_*`.
- `tests/test_recursive_shared_mutex.cpp` asserted with `REQUIRE` inside a spawned thread. Catch2 assertions must run on the main thread: a failing one escapes as `Catch::TestFailureException` and terminates the whole test process instead of failing the case. The observation is now recorded in a bool and asserted after `join()`.

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
