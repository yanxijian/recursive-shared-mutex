# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

- `E3` now also checks the `try_*` half of the table-exhaustion asymmetry: with all 256 slots held, `try_lock_shared()` returns `false` instead of throwing. gcov showed that branch was never executed, and R9 pointed at `tests/backend_contract.cpp` for it - a file that has nothing to do with the slot table. R9 now names the test that actually covers both halves.
- New nightly `soak.yml`: the mutual-exclusion driver runs for 60 s, and for 30 s under TSan, every night. The push-time suite finishes in about three seconds, which is not a soak for a concurrency primitive; a race that needs a lucky interleaving needs more than that.
- The header records two measured dead ends so a later agent does not repeat them: merging the two slot-table TLS reads in `findActiveSlot()` (35.07 vs 35.35 ns, inside the 0.03 ns A/A noise) and dropping the `used == 0` fast path in `lock()` for a try_lock-first path (equal uncontended, 1.2-1.8x worse with four contending writers). The `RSM_SHARED_SLOT_COUNT` note also stops implying that only readers touch the table: `lock()` and `try_lock()` read it as well, so the 4 KB is per thread that ever locks.
- `instrumented_write_lock` now timestamps HOLD before releasing and reports it afterwards. The `Recorder` contract says a sink is never called while the lock is held, but the destructor called `observe_write_hold()` first and unlocked second, so a sink that took any lock of its own ran inside the critical section - a lock-order inversion waiting to happen, and it made the documented contract false. The reported duration is unchanged (the timestamp is still taken at the release point). New test `M8` proves from a second thread that the mutex is acquirable while HOLD is being reported; it fails against the old order.
- New `is_held_exclusive()` accessor, and `lock_shared` / `try_lock_shared` / `unlock_shared` use it instead of re-testing `owner_ == this_thread::get_id()`. On MinGW that test was a `pthread_self()` call on every shared operation even when no writer existed; skipping the id when `owner_` is empty takes the shared lock/unlock pair from 34.0 to 25.2 ns (-26%, A/A build-to-build noise 0.03 ns). The shortcut is MinGW-only on purpose: the unconditional form measured 7.58 -> 8.43 ns (+11%) on MSVC, where `get_id()` is a TLS read and the extra comparison is not free. The accessor also gives the migration guide's "exclusive core" rail an exact check - `assert(!try_lock())` also passes for a thread that holds the mutex shared, which is what R3 used to recommend.
- `T14` covers the migration guide's headline hazard: a foreign thread's `unlock_shared()` throws `operation_not_permitted` and leaves the owner's slot intact, so the mutex stays usable afterwards.
- `scripts/check_repo_rules.py --fix` no longer strips the UTF-8 BOM while normalising line endings. The repair branch decoded with `utf-8-sig` (which eats the BOM) and wrote a BOM back only for files that did not already have one, so a file that had a BOM *and* mixed line endings came out BOM-less: `--fix` was not idempotent, its own output failed the check it had just run (a second run would put the BOM back), and a commit made in between landed a source file MSVC parses in the system code page - the reason R1 exists. The BOM is now written unconditionally, so one run repairs and the second is a no-op.
- `tests/backend_contract.cpp` now checks the second promise `lock()` rests on - a backend must refuse `try_lock()` while the calling thread already holds the mutex shared - and its violation counters are atomic. They were plain `long long` incremented from several threads, so the run that mattered (a backend that had already broken exclusion) raced on its own evidence.
- The exclusive slow path's third branch - a thread that holds a shared lock elsewhere and contends for a mutex a writer owns - now has a test (`T13`), and `ci.yml` builds and runs `tests/stress_concurrent.cpp`, which no workflow had ever compiled.
- Documentation pointers that outlived the move to `hygiene.yml` are corrected (`AGENTS.md`, `.githooks/pre-commit`), `.clang-format` no longer cites a file from another repository, the v0.3.1 entries stop contradicting each other about batch-file line endings, and `[v0.3.1]` has its comparison link.
- `RSM_SHARED_SLOT_COUNT` is rejected at compile time when it is zero: a zero cap was not "no shared locks" but a zero-length array (a GNU extension, rejected by MSVC) with every `lock_shared()` throwing. No upper bound is enforced - the table costs 16 B of TLS per slot per thread, and the right maximum is the consumer's call.
- `scripts/check_repo_rules.py` has a test suite (`scripts/tests/`, run by `hygiene.yml`, documented in `AGENTS.md`) and now checks that every `## [x]` release heading has its `[x]: url` definition - the failure that left `[v0.3.1]` rendering as plain text, which nothing caught because `check_markdown_links` only looks at inline links. Its summary line no longer double-counts ("12 C/C++ and 60 text files" for 60 tracked files), the rule banners sit above the functions they describe, and `__pycache__/` is ignored.
- The README states the benchmark protocol and its source instead of implying the numbers come from `benchmarks/results/`: they come from `docs/platform-notes.md` §2/§6 (8 threads, `try_lock` + `yield` spin, median of 5 × 1 s rounds), while the harness runs 3 s × 3 repeats with real work between lock operations - hence 1.17× vs 1.03× for the same exclusive path. `platform-notes.md` no longer claims the checked-in tables report the pre-refresh 0.93×/0.10×, and the result files cite the `v0.3.0` tag instead of a commit id that the history rewrite orphaned.
- Two costs the README used to leave implicit are now stated: the type is not literally `Cpp17Lockable` (`unlock()` throws on misuse where the named requirement says *Throws: Nothing*), and on MinGW the default backend pulls in `<windows.h>` - 655 ms vs 207 ms to compile one translation unit here (3.2×).
- The ARM64 matrix cell no longer runs with `continue-on-error: true`: it is the only cell exercising weak memory ordering, and hiding its failures hid exactly the signal it exists to produce. `docs/roadmap.md` stops quoting a platform-matrix cell count that was already wrong (25 jobs now, "13 cells" in the sentence).
- Release notes use a collapsible `<details>` block for the Chinese section, not a `#简体中文` jump link: GitHub renders release bodies without heading ids, so the link had no target. Verified against the rendered HTML (headings have no `id`; `<details>` passes the sanitiser).
- `AGENTS.md` documents the release procedure: the five places a version lives, the annotated tag, and the bilingual GitHub Release body (English, then `## 简体中文`, title `english · 中文`). All five existing releases were brought up to that shape, and two older bodies were repaired (v0.3.0 carried cp1252 corruption, v0.1.0 carried a leaked link-reference block).

## [v0.3.1] - 2026-10-17

### Changed

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
- Line endings now follow a declared standard instead of a hand-maintained list: the new `.gitattributes` keeps C/C++ as UTF-8 BOM + CRLF and marks it `-text` (git never converts those bytes), gives `.bat`/`.cmd` the same CRLF-in-store-and-on-checkout rule, and normalises everything else to LF (`* text=auto eol=lf`), matching `.editorconfig`. The nine remaining CRLF build/tooling files were converted; R7 now verifies the working tree against `.gitattributes` and verifies that `.gitattributes` still declares the standard.
- All Markdown converted to LF, which is what `.editorconfig` already declared as its default: the 8 documents that were CRLF (`CHANGELOG`, `CONTRIBUTING`, `SECURITY`, `CODE_OF_CONDUCT`, `docs/migration.md`, the issue/PR templates) and the 2 that were mixed (`README.md`, `docs/platform-notes.md`) are now plain LF. Content is unchanged - `git diff --ignore-cr-at-eol` is empty - and R7's `EOL_POLICY` no longer lists any markdown file, so new docs default to LF and a flip still fails the build.
- The two `RSM_BACKEND_STD` CI cells that were red on every push are replaced by one weekly probe, `expected-fail / mingw winpthreads (red = upstream fixed)`: it asserts the winpthreads contract is *still* violated, so it is green while the defect exists and turns red the day upstream fixes it. Push and PR pages no longer carry a permanently red cell, and the suite cell that failed on the same root cause is gone.
- New repository rule R7, enforced by `scripts/check_repo_rules.py`: a text file keeps the line endings it was committed with (LF by default; the CRLF and genuinely mixed files are listed in `EOL_POLICY`). A whole-file flip - which neither the compiler, clang-format nor git notices - now fails CI instead of silently turning a 194-line document into a 392-line diff.
- Documentation and comments re-checked against the code: the README's MinGW section no longer describes the native backend as an opt-in flag (it is the default), the FetchContent example points at v0.3.0, the exclusive-path cost is the current 4.6 ns, the project layout lists `benchmarks/`, `scripts/`, `AGENTS.md`; `docs/platform-notes.md` labels which rows describe `RSM_BACKEND_STD` and which are v0.2.x-era; `docs/migration.md` records the new backend promise (`try_lock()` must refuse while the caller holds the mutex shared) and the new `[perf]` test; `run_bench.ps1` documents `-GateScenarios`.
- `lock()` no longer scans the shared-slot table when the thread holds no shared locks, and uses a `try_lock`-first path when it does: the exclusive path is now constant (5.4 ns) instead of growing to 107 ns while 250 shared locks are held. The shared path is unchanged and still linear - the header now documents the measured cost.
- `static_assert` that `std::atomic<std::thread::id>` is lock-free (the owner token is on every hot path), and a debug assertion against destroying a mutex this thread still holds shared.
- CI is tiered so a pull request does not wait for the exhaustive matrix: PRs run `ci.yml`, `contract-mingw`, the MinGW suite and the sanitizers (~90 s of critical path), while the rest of the matrix, the ARM64 runner and the benchmark gate wait for `main`, the weekly schedule, or a change to `include/**`. Docs-only pushes run nothing but `ci.yml`.
- Housekeeping: the `clang-format` CI job is pinned to 22.1.8 through PyPI instead of taking whatever the runner image ships; `benchmarks/results/*.md` were re-measured on v0.3.0 (median of 3 runs, with a `Run-to-run spread` line - the balanced scenario has swung 250% between runs on the same machine, so the speedups are order-of-magnitude); `docs/roadmap.md` reflects what actually landed.
- The benchmark PR gate now fails only on S6 (sharded) and S7 (exclusive-only), at 40%: on a 4-core runner the read-parallelism scenarios cannot show a real regression there, only scheduling noise. Benchmark reports record the backend they measured (`--backend`, printed as a `- Backend:` line). MinGW numbers before and after v0.3.0 are not comparable: the default backend changed there.
- Agent commits: the commit is authored by the maintainer and carries a
  `Co-authored-by: DeepSeek v4.1-flash <noreply@deepseek.com>` trailer. The
  trailer is advisory and vendor-neutral (AGENTS.md prescribes no model, version
  or address), and history was rewritten so every agent commit reads the same
  way.

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

[Unreleased]: https://github.com/yanxijian/recursive-shared-mutex/compare/v0.3.1...HEAD
[v0.3.1]: https://github.com/yanxijian/recursive-shared-mutex/compare/v0.3.0...v0.3.1
[v0.3.0]: https://github.com/yanxijian/recursive-shared-mutex/compare/v0.2.1...v0.3.0
[v0.2.1]: https://github.com/yanxijian/recursive-shared-mutex/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/yanxijian/recursive-shared-mutex/releases/tag/v0.2.0
[0.1.0]: https://github.com/yanxijian/recursive-shared-mutex/releases/tag/v0.1.0
