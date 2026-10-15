# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
