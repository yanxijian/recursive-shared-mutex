# Validation

**English** · [简体中文](validation.zh-CN.md)

This page defines what "the library is validated" means here, how the evidence is
recorded, and what the evidence does **not** cover. It is the contract the soak
workflows are sized against; `verification/soak-history.md` is the ledger.

The goal is to validate a transition component before a consumer trusts it,
rather than letting the first consumer find its bugs. The fixed suite covers the
cases someone thought of; the model checker covers the spec over random
sequences; the soak covers time and scheduler families.

## What runs today

| Layer | Tool | Where |
|---|---|---|
| Backend contract (the toolchain's mutex) | `tests/backend_contract.cpp` | every CI cell |
| Fixed suite (recursion, errors, try paths) | Catch2, `tests/test_recursive_shared_mutex.cpp` | every CI cell |
| Spec vs shadow model | `tests/model_check.cpp` | `ctest`, nightly, batches |
| Library-level exclusion | `tests/mutual_exclusion.cpp` | `ctest`, nightly |
| Sanitizers | ASan/UBSan/TSan matrix | CI, nightly |
| Packaging | install + `find_package` consumer | every CI run |
| Platforms | MSVC, clang, MinGW (SRWLOCK and winpthreads), musl, libc++, ARM64, 32-bit | CI matrix |

## Two ledgers, not one

Coverage and cost are different quantities and are recorded separately.

- **Coverage** - what the test itself reports: `ops`, exclusive `entries`,
  distinct `seed`s, `thread-seconds` (`threads x seconds`), platform. These are
  machine-independent and are the numbers the exit criteria use.
- **Cost** - what GitHub bills: job-minutes (wall clock per job). A shard costs
  the same whether it runs 4 or 8 threads.

Do not use "core-hours" as a target: the program cannot measure it, the runner
does not report it, and on a 4-vCPU runner the thread count is an oversubscription
choice, not a resource count.

## Exit criteria

**Tier 1 - every release** (the 0.4.x baseline already met it):

- `ctest` green on every CI cell (30 tests at v0.4.1).
- Nightly soak green for the release week, seeds derived from the run id.
- One recorded batch of at least 8 job-hours with zero violations.

**Tier 2 - the "self-certified" bar, before a v1.0 or before a consumer is asked to trust it:**

- `model_check`: at least **10^12 operations** across at least **200 distinct
  seeds**, with Linux, Windows and ARM64 all showing zero violations. The long
  operation count may come from one platform; the other two need at least a
  recorded batch of their own.
- `mutual_exclusion`: at least **10^9 exclusive entries** with zero overlaps,
  zero reader-with-writer, zero stranded slots.
- TSan: at least **20 thread-hours** clean, including the model checker.
- ASan/UBSan: the full suite and the model checker.
- Every error path is exercised **under contention** (injection, slot
  exhaustion, misuse combinations), not only single-threaded.
- The mutation tests still fail: a checker that cannot detect a pinned
  `is_held_exclusive()` or a removed `under_exclusive_` guard is not evidence.
- No known flaky test is left in the tree.

These are floors, not a completeness claim. Size batches from the
recorded rate: `ops_target / (ops_per_second x shards x minutes x 60)` gives the minutes
per shard. The ledger now has the rate: an 8-shard x 300 min x 8-thread batch (40
job-hours) produced **7.03 x 10^11 operations**, an 8-shard x 60 min batch produced
**1.44 x 10^11**, and the whole ledger needed **62 job-hours for 1.03 x 10^12**.
Short shards run at about 2.1 x 10^10 operations per job-hour, 5-hour shards at
about 1.76 x 10^10 (longer runs throttle on a 4-vCPU runner). Read `ops` from the
history before sizing the next one.

## Status (2026-10-18)

Tier 1 and Tier 2 are met, from the nine recorded batches in
`verification/soak-history.md`:

| Criterion | Result |
|---|---:|
| `model_check` operations | **1.03 x 10^12** |
| distinct seeds | **351** |
| exclusive entries | **1.03 x 10^11** |
| TSan | **24.1 thread-hours** |
| platforms with zero violations | Linux (long runs), Windows, ARM64 |
| mutation tests | still fail as designed |

The split is uneven: the long operation count comes from Linux; the Windows and
ARM64 rows are short batches that show the same zero-violation result on their
toolchains. A pass is evidence of absence of the observed failures, not a proof;
see "What this does not cover".

## Running it

- **Nightly** - `soak.yml`, automatic: 60 s of `mutual_exclusion`, 30 s under
  TSan, four `model_check` seeds and one TSan seed, all seeded from
  `GITHUB_RUN_ID`. Costs about 8 job-minutes.
- **Calibration batch** - `soak-batch.yml` with `shards=2, minutes=2,
  seeds_per_shard=2`: four seeds, four job-minutes, records a row. Use it to measure
  the runner's ops/s.
- **Milestone batch** - for example `shards=4, minutes=120, seeds_per_shard=8`:
  32 seeds, 8 job-hours. Set `record=true` (the default) to append one row to
  `verification/soak-history.md`; the workflow commits only that file. The commit
  is pushed with `GITHUB_TOKEN`, so it does not start the rules job - the guard
  that refuses any other path is what keeps it narrow.
- **TSan batch** - `tsan=true`: builds with `-fsanitize=thread` and runs
  `--mutexes 1` (see False positives). Unix only. A 3-shard x 120 min x 4-thread
  batch contributes 24 thread-hours.
- **Other platforms** - `runs_on=windows-latest, label=windows` or
  `runs_on=ubuntu-24.04-arm, label=arm64`; the shard builds with MSVC or g++
  accordingly. Short batches there are enough to show zero violations on the
  platform; the long runs stay on Linux, where a runner-hour buys the most ops.

The 6-hour job limit is the constraint: keep `minutes` at 300 or below.

## False positives

A red run is triaged, never silenced. The first known case:

- TSan reports a **lock-order inversion** in `model_check`'s phase 1. That phase
  holds several mutexes at once but uses one thread, so it cannot deadlock; TSan
  cannot see the phase boundary. The TSan job runs `--mutexes 1` (data-race
  detection does not need the multi-mutex layout); the plain job keeps 8.

Silencing anything requires a comment at the point of silencing and a line in
this section, with the seed that reproduces it.

## What this does not cover

- **Cross-module TLS**: the hazard in `examples/cross_module_hazard` needs a
  multi-DLL build; a single-process test cannot prove it.
- **Formal proof**: this is testing, not verification. A pass is evidence of
  absence of the observed failures, not a proof.
- **Performance**: shared runners are for correctness only. Benchmarks live in
  `benchmarks/` and follow R13.
- **Windows/ARM coverage** depends on the CI runners being available; the
  history records which platform produced each row.

## History format

`verification/soak-history.md` has one row per recorded batch:

| Date | Run | Platform | Shards | Minutes | Threads | Seeds | Ops | Entries | Verdict |

Every row links a run id to the numbers; a failure row keeps its seed range so
the batch can be replayed.
