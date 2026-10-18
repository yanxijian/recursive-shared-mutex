# Benchmarks vs `std::recursive_mutex`

**English** · [简体中文](README.zh-CN.md)

Compares `rsm::RecursiveSharedMutex` (readers use shared locks unless noted) against `std::recursive_mutex` under nested locking and a ~4 MiB working set.

## Run

```powershell
# Portable: needs cl.exe or g++ on PATH. No hard-coded Visual Studio path.
#   -Toolchain msvc|mingw|both   -Seconds 3   -Repeats 3   -OutDir results
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_bench.ps1 -Toolchain both -Seconds 3 -Repeats 3
```

Each (scenario, implementation) pair runs in a **fresh process**, so MinGW `shared_mutex` stress cannot poison later configurations. Every pair runs `-Repeats` times and the report uses the **median** throughput; a single run on a shared machine is too noisy to compare.

The driver is built through CMake (`-DRSM_BUILD_BENCHMARKS=ON`, target `bench_rsm`), so nothing depends on where Visual Studio happens to be installed.

## Automation

[`.github/workflows/benchmarks.yml`](../.github/workflows/benchmarks.yml) runs these scenarios on a pinned `windows-2022` runner.

- **Weekly (Mon 03:00 UTC) and on demand:** the default `mode=compare` runs both implementations in one process, alternating ABBA, and writes two files: `results/compare-ci-<toolchain>.md` (the next run's gate baseline) and the BMF file Bencher ingests. With `update_mode: auto-merge` the job commits only the `compare-ci-*.md` file, and only if every changed path matches that pattern. `pull-request` opens a PR and leaves it for review, `commit` commits to the current branch, `none` measures without committing.
- **On pull requests touching `include/**`:** a short comparison against the committed `compare-ci-msvc.md`. Only S6 (sharded) and S7 (exclusive-only) gate, at 20%. On four cores the read-parallelism scenarios sit at parity by construction; S6/S7 are the ones that can show the wrapper cost moving.
- The workflow never merges a pull request, including the one `pull-request` mode opens. The only automatic merge is the results commit this job created seconds earlier, after the path check above; the gate job runs read-only.

Worth knowing:

- Absolute ops/s belong to the runner that produced them. Read the ratio against `recursive_mutex`, measured in the same run.
- The gate only uses CI-produced reports from the same runner. Each compare report records how many rounds had a healthy baseline (within 80% of its best); scenarios with fewer than three healthy rounds are skipped rather than failed.
- GitHub runners have four cores. Shared locking needs more readers than cores to pay off, so S1-S5 sit near parity on CI even though a 32-core desktop shows a large win. CI is for drift detection, not for the performance story; the report says so when it sees fewer than eight cores.
- `update_mode: pull-request` needs *Allow GitHub Actions to create and approve pull requests* under Settings > Actions > General; auto-merge needs no setting.

A local run on a big machine:

```powershell
# e.g. on a 32-core desktop
powershell -NoProfile -File benchmarks/run_bench.ps1 -Toolchain msvc -Seconds 3 -Repeats 3
```

## Continuous tracking (optional, Bencher)

`run_bench.ps1 -Json <path>` writes [Bencher Metric Format](https://bencher.dev/docs/reference/bencher-metric-format/):
one JSON object per (toolchain, scenario, implementation) with a `throughput`
measure (median and min/max across the repeats). `benchmarks.yml` feeds that file
to `bencher run --adapter json` after measuring.

It is **tracking only**. The gate is still the `-RegressionPct` comparison above,
and CI sets no thresholds. Without the repository variable `BENCHER_PROJECT` and
the secret `BENCHER_API_KEY`, the step runs `--dry-run` and prints the report to
the log, so a fork or a fresh clone validates the format without an account.

To enable it: create the project first (the sending path requires it to exist),
then open the project's **Keys** page and create a key - the `bencher_run_*` value
is shown once, and a key's detail page only shows its metadata afterwards. Set
`BENCHER_PROJECT` (the project slug) as a repository variable and
`BENCHER_API_KEY` as a secret. Start without thresholds. Wall-clock noise on a
shared runner is tens of percent; a threshold only becomes meaningful after a few
weeks of history, and instruction-count benchmarks are a better basis for a hard
gate.

## How precise are these numbers?

Not very, and the report says so: each one carries a `Run-to-run spread` line, the
observed min..max across the repeats. Measured on a busy 32-core desktop, the
median spread is 18-48% and the worst scenario (S1 on MSVC) has swung 245%
between runs of the same binary. Treat the speedups as order-of-magnitude claims
("reads are much faster", not "13.59x"), and use the `compare-ci-*.md` reports,
which pair both implementations on one fixed runner, for drift.

`-Repeats` raises confidence, not the machine's quietness. Close everything else
before measuring; otherwise the numbers describe your browser.

## Telemetry

Every run also reports, after the existing columns, the number of failed
`try_lock`/`try_lock_shared` attempts, the number of `yield()` calls that
followed them, and the process CPU milliseconds split into user and kernel time.
`run_bench.ps1` aggregates them (medians across repeats) into a **Telemetry**
table in each report.

They are diagnostic, not a gate. On S3 the `recursive_mutex` baseline can fail two
orders of magnitude more often than RSM while RSM's own numbers stay flat; the
speedup moves because of the baseline. High kernel time means the run spent its
time in the scheduler, not in the lock.

## Interleaved comparison (`-Compare`)

`run_bench.ps1 -Compare -Rounds N` runs each scenario in **one process**: the
baseline and the library alternate (ABBA), every round records `RSM ops /
baseline ops`, and the report uses the median of those ratios instead of the
ratio of two independently measured medians. It writes
`compare-<toolchain>.md` locally (git-ignored) or `compare-ci-<toolchain>.md` on
the runner (committed) with the per-scenario median ratio, the IQR, and the
number of "healthy" rounds - rounds where the baseline came within 80% of its
best round.

The baseline is bimodal under a `try_lock` + `yield` spin: when it convoys, the
ratio inflates. The healthy count says how much of the number is baseline
behaviour rather than the library.

This is the weekly path: `mode=compare` (the scheduled default) writes
`compare-ci-<toolchain>.md` (committed; it is the next run's gate baseline) and
the BMF for Bencher from the same run. The gate compares the healthy ratio
against the previous report and skips scenarios with fewer than three healthy
rounds. The weekly gate covers only scenarios that stayed within a few percent
across three seeded samples: MSVC S2/S4/S5/S6/S7 and MinGW S5/S6, at 10%. The
pull-request gate uses the same report for S6/S7 at 20%; `measure` mode remains
for the desktop numbers and manual checks.

## Scenarios

| ID | Description |
|----|-------------|
| S1 | 15 readers / 1 writer, nested depth 3 |
| S2 | 2 readers / 12 writers |
| S3 | 8 / 8 balanced |
| S4 | 16 workers, ~20% writes |
| S5 | 16 workers, ~5% writes |
| S6 | 8 shards, 16 workers, ~15% writes |
| S7 | Exclusive-only overhead (RSM without `lock_shared`) |

## Checked-in results

`results/<toolchain>.md` - a frozen single-host snapshot (32-core desktop, 3 s x 5), the representative numbers quoted elsewhere. CI does not touch them; re-measure only when the library's performance characteristics change materially.

- [results/msvc.md](results/msvc.md)
- [results/mingw-gcc.md](results/mingw-gcc.md)

`results/compare-ci-<toolchain>.md` - the only auto-refreshed baseline: the 4-core GitHub runner, refreshed by the weekly compare run. It is the next run's gate baseline; **do not compare it with the desktop snapshot above**.

Each report records the backend it measured (`- Backend: ...`), because MinGW's numbers changed meaning in v0.3.0: it used to be `std::shared_mutex` over winpthreads and is now the native SRWLOCK wrapper. Reports generated before v0.3.0 say `std::shared_mutex` on MinGW and are not comparable with the ones after it.

- [results/compare-ci-msvc.md](results/compare-ci-msvc.md)
- [results/compare-ci-mingw-gcc.md](results/compare-ci-mingw-gcc.md)
