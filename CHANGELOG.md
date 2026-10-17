# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- The agent signature is pinned to model **and** deployment version: `Co-authored-by: deepseek-flash v4.1-flash <noreply@deepseek.com>`. The version comes from the maintainer because an agent cannot read its own deployment identifier. Historical trailers were normalised to the same string.
- The `Co-authored-by` convention now has the **agent sign itself**: the trailer names the model that produced the change (`Co-authored-by: deepseek-flash <noreply@deepseek.com>`) instead of the placeholder `review <review@local>`. The commit author stays the maintainer, because an address that belongs to nobody is unattributed on GitHub and invisible to `git log --author`.
- Commit authorship normalised: the 35 commits that had been authored as `review <review@local>` are now authored by the maintainer with `Co-authored-by: review <review@local>` in the body. Same trees, same order, same count - only the author and the trailer differ.
- CI is filtered by build inputs: `ci.yml` and `platform-matrix.yml` now list the paths that can affect a build, so a documentation or script change starts no compiler. The rules checks moved to a new always-on `hygiene.yml` (line endings, translations, and every relative Markdown link), because those are the checks a documentation change can actually break.
- The three governance documents moved from the root into `.github/`
  (`CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md` and their Chinese
  editions). GitHub looks for all three in the root, `docs/` or `.github/`, so
  nothing about how they surface changed; the root now holds only what a reader
  opens first: `README.md`, `README.zh-CN.md`, `CHANGELOG.md`, `LICENSE`,
  `AGENTS.md` and `CLAUDE.md`.
- Simplified Chinese editions of the eight reader-facing documents (README, CONTRIBUTING, SECURITY, CODE_OF_CONDUCT, benchmarks/README, docs/migration, docs/platform-notes, docs/roadmap), side by side as `*.zh-CN.md`. English stays the default; each pair links to the other under its title and the translation records the English revision it came from as a content hash.
- New rule R8, enforced by `scripts/check_repo_rules.py`: the translated set must stay complete, both sides must keep their switcher link, and a translation whose English source moved on is reported as `STALE` (a warning, not a failure - translating must not block a merge, but it must not go unnoticed either).
- Repaired cp1252 double-encoding in `docs/platform-notes.md` and `CHANGELOG.md` (`Ã—` was `×`, `â€“` was `–`, `â‰ˆ` was `≈`).
- `.github/SECURITY.md` listed only 0.1.x as supported; it now lists 0.3.x.
- Batch files moved onto the same rule as C/C++ in `.gitattributes`: CRLF in the object store and on checkout (`-text -eol`), instead of LF-in-store / CRLF-on-checkout. One mechanism for "this file must be CRLF" instead of two, and an accidental LF flip is now a visible diff rather than something git normalises away.
- Line endings now follow a declared standard instead of a hand-maintained list: new `.gitattributes` keeps C/C++ as UTF-8 BOM + CRLF and marks it `-text` (git never converts those bytes), stores `.bat`/`.cmd` as LF and checks them out as CRLF, and normalises everything else to LF (`* text=auto eol=lf`), matching `.editorconfig`. The nine remaining CRLF build/tooling files were converted; R7 now verifies the working tree against `.gitattributes` and verifies that `.gitattributes` still declares the standard.
- All Markdown converted to LF, which is what `.editorconfig` already declared as its default: the 8 documents that were CRLF (`CHANGELOG`, `CONTRIBUTING`, `SECURITY`, `CODE_OF_CONDUCT`, `docs/migration.md`, the issue/PR templates) and the 2 that were mixed (`README.md`, `docs/platform-notes.md`) are now plain LF. Content is unchanged - `git diff --ignore-cr-at-eol` is empty - and R7's `EOL_POLICY` no longer lists any markdown file, so new docs default to LF and a flip still fails the build.
- The two `RSM_BACKEND_STD` CI cells that were red on every push are replaced by one weekly probe, `expected-fail / mingw winpthreads (red = upstream fixed)`: it asserts the winpthreads contract is *still* violated, so it is green while the defect exists and turns red the day upstream fixes it. Push and PR pages no longer carry a permanently red cell, and the suite cell that failed on the same root cause is gone.
- New repository rule R7, enforced by `scripts/check_repo_rules.py`: a text file keeps the line endings it was committed with (LF by default; the CRLF and genuinely mixed files are listed in `EOL_POLICY`). A whole-file flip - which neither the compiler, clang-format nor git notices - now fails CI instead of silently turning a 194-line document into a 392-line diff.
- Documentation and comments re-checked against the code: the README's MinGW section no longer describes the native backend as an opt-in flag (it is the default), the FetchContent example points at v0.3.0, the exclusive-path cost is the current 4.6 ns, the project layout lists `benchmarks/`, `scripts/`, `AGENTS.md`; `docs/platform-notes.md` labels which rows describe `RSM_BACKEND_STD` and which are v0.2.x-era; `docs/migration.md` records the new backend promise (`try_lock()` must refuse while the caller holds the mutex shared) and the new `[perf]` test; `run_bench.ps1` documents `-GateScenarios`.
- `lock()` no longer scans the shared-slot table when the thread holds no shared locks, and uses a `try_lock`-first path when it does: the exclusive path is now constant (5.4 ns) instead of growing to 107 ns while 250 shared locks are held. The shared path is unchanged and still linear - the header now documents the measured cost.
- `static_assert` that `std::atomic<std::thread::id>` is lock-free (the owner token is on every hot path), and a debug assertion against destroying a mutex this thread still holds shared.
- CI is tiered so a pull request does not wait for the exhaustive matrix: PRs run `ci.yml`, `contract-mingw`, the MinGW suite and the sanitizers (~90 s of critical path), while the other 11 cells, the ARM64 preview runner and the benchmark gate wait for `main`, the weekly schedule, or a change to `include/**`. Docs-only pushes run nothing but `ci.yml`.
- Housekeeping: the `clang-format` CI job is pinned to 22.1.8 through PyPI instead of taking whatever the runner image ships; `benchmarks/results/*.md` were re-measured on v0.3.0 (median of 3 runs, with a `Run-to-run spread` line - the balanced scenario has swung 250% between runs on the same machine, so the speedups are order-of-magnitude); `docs/roadmap.md` reflects what actually landed.
- The benchmark PR gate now fails only on S6 (sharded) and S7 (exclusive-only), at 40%: on a 4-core runner the read-parallelism scenarios cannot show a real regression there, only scheduling noise. Benchmark reports record the backend they measured (`--backend`, printed as a `- Backend:` line). MinGW numbers before and after v0.3.0 are not comparable: the default backend changed there.

## [v0.3.0] - 2026-10-17

### Fixed

- Building with clang-cl failed: it disables C++ exceptions by default, and the header throws std::system_error by design. The header now emits one clear #error when exceptions are off, the CMake target asks for /EHsc on MSVC-simulating compilers, and README lists it as a requirement.
- README described the shared-recursion state as a `thread_local` intrusive list "released on thread exit", which stopped being true in v0.2.0 (dense POD slot table, no thread-exit release). Corrected, together with the performance summary: MSVC/clang are at 0.9×–1.2× of `std::recursive_mutex` on the exclusive path, MinGW-GCC at 0.07×–0.2×, with the toolchain-level cause spelled out.
- The project version reported `0.1.0` while releases were at `v0.2.1`, so `find_package(recursive_shared_mutex 0.2 REQUIRED)` failed against an installed tree. `project(VERSION)` now matches the header's `RSM_VERSION_*`.
- `tests/test_recursive_shared_mutex.cpp` asserted with `REQUIRE` inside a spawned thread. Catch2 assertions must run on the main thread: a failing one escapes as `Catch::TestFailureException` and terminates the whole test process instead of failing the case. The observation is now recorded in a bool and asserted after `join()`.

### Changed

- On MinGW the native SRWLOCK backend is now selected automatically: libstdc++'s `std::shared_mutex` there is built on winpthreads, which lets two threads hold the exclusive lock and is ~11x slower. `RSM_BACKEND_STD` restores the old behaviour (and the defect) for anyone who needs it; `RSM_BACKEND_AUTO_SELECTED` reports that the choice was automatic.
- Column limit raised from 120 to 140 in `.clang-format`, AGENTS.md R2 and the rule checker. Sources reformatted accordingly; the widest line in the tree is now exactly 140.
- `try_lock_shared()` returns `false` instead of throwing when the per-thread slot table is full. `SharedLockable` requires `try_lock_shared` not to throw, and the previous behaviour made `std::shared_lock(mtx, std::try_to_lock)` non-conforming. `lock_shared()` still throws `resource_unavailable_try_again` on exhaustion.
- `RSM_BUILD_TESTS` / `RSM_BUILD_EXAMPLES` default to `ON` only when the project is top-level, so `add_subdirectory()` / `FetchContent_MakeAvailable()` consumers no longer download Catch2 and build two test binaries by default.
- Documented that a mismatched `unlock`/`unlock_shared` ends in `std::terminate` (the throwing call is reached from `noexcept` RAII destructors), that `std::condition_variable_any` is unsafe once recursion depth > 1, and that a cross-module shared-lock sequence is undefined behaviour rather than just a stranded slot.

### Added

- `AGENTS.md` + `scripts/check_repo_rules.py`: one rule set for every agent (Claude Code, Codex, Cursor, Copilot, Gemini CLI), with the mechanical rules (UTF-8 BOM + CRLF, 120 columns, no `#pragma comment(lib)`, header hygiene, version sync, changelog section) enforced by a `repo-rules` CI job and an optional pre-commit hook.
- `RSM_BACKEND_SRWLOCK`: backend policy that swaps `std::shared_mutex` for an 8-byte SRWLOCK wrapper (`rsm::detail::SrwLockBackend`, exposed as `RecursiveSharedMutex::backend_type`). Default on MinGW since `RSM_BACKEND_STD` exists to opt out; a no-op in effect on MSVC, whose `std::shared_mutex` already is SRWLOCK.
- `tests/backend_contract.cpp`: framework-free check that the mutex the library is actually built on honours mutual exclusion. It reports the backend (`std::shared_mutex` vs `SrwLockBackend`), so CI can prove the switch fixes the MinGW cell.
- `tests/mutual_exclusion.cpp`: framework-free check that the library keeps writers mutually exclusive and readers out while a writer is inside — a property the existing suite did not assert.
- `.github/workflows/platform-matrix.yml`: cross-platform CI matrix (MSVC x64/x86 and x64+SRWLOCK, clang-cl, MinGW-UCRT64 default *and* with `RSM_BACKEND_SRWLOCK`, gcc-11/13, clang+libstdc++, clang+libc++, macOS arm64/x86_64, Linux arm64, alpine/musl, TSAN/ASAN+UBSAN, C++20 notice assertion, shared-library TLS domain). The backend contract test runs first in every cell; the two MinGW cells that use the unsound default backend are marked `continue-on-error` with the reason.
- `examples/cross_module_hazard/`: a real DLL/.so reproducer for the per-module TLS table, showing that the same thread re-acquires the backend inside the second module (undefined behaviour that is silent on SRWLOCK and on reader-preferring rwlocks).
- `docs/platform-notes.md`: measured three-toolchain data (MinGW-GCC 13.2 / MSVC 19.51 / clang 22.1), the backend contract results, the before/after of the SRWLOCK switch, and reproduction commands.
- `rsm::CacheAlignedRecursiveSharedMutex`: a 64-byte-aligned instance for sharded arrays. The default instance is 24 bytes, so ~2.6 of them share a cache line and the shards false-share the owner/counter updates. Measured with 8 shards / 16 threads (0 %, 15 %, 50 % writes): 13.9–14.3 M → 31.3–33.2 M total ops/s on MSVC, 28.1 M → 57.3 M on MinGW. New benchmark implementation `--impl rsm_padded` (meaningful with `--mode sharded`).
- `RSM_VERSION_MAJOR/MINOR/PATCH` and `RSM_VERSION`: the header now carries its own version, for consumers that pull it with `FetchContent` and cannot see the CMake package.
- `RSM_SHARED_SLOT_COUNT` override: the per-thread slot table cap is now a macro (default unchanged at 256, i.e. 4 KB of TLS per thread), so thread-heavy or embedded targets can shrink it.
- `RSM_BUILD_STRESS` CMake option: builds `tests/stress_concurrent.cpp` as `rsm_stress` and registers it as a test. It used to be built by hand, which is how such drivers rot.
- Debug-only assertion in the destructor (`owner_` / exclusive counters non-zero) so "destroyed while locked" fails at the point of misuse instead of corrupting a later mutex at the same address. Disable with `RSM_NO_DEBUG_ASSERT`.
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

[Unreleased]: https://github.com/yanxijian/recursive-shared-mutex/compare/v0.3.0...HEAD
[v0.3.0]: https://github.com/yanxijian/recursive-shared-mutex/compare/v0.2.1...v0.3.0
[v0.2.1]: https://github.com/yanxijian/recursive-shared-mutex/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/yanxijian/recursive-shared-mutex/releases/tag/v0.2.0
[0.1.0]: https://github.com/yanxijian/recursive-shared-mutex/releases/tag/v0.1.0
