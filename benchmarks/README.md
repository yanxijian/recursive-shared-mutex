# Benchmarks vs `std::recursive_mutex`

**English** · [简体中文](README.zh-CN.md)

Compares `rsm::RecursiveSharedMutex` (readers use shared locks unless noted) against `std::recursive_mutex` under nested locking and a ~4 MiB working set.

## Run

```powershell
# Portable: needs cl.exe or g++ on PATH. No hard-coded Visual Studio path.
#   -Toolchain msvc|mingw|both   -Seconds 3   -Repeats 3   -OutDir results
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_bench.ps1 -Toolchain both -Seconds 3 -Repeats 3
```

Each (scenario, implementation) pair runs in a **fresh process** so MinGW `shared_mutex` stress cannot poison later configs in-process. Every pair runs `-Repeats` times and the **median** throughput is reported: one run on a shared machine is far too noisy to compare.

The driver is built through CMake (`-DRSM_BUILD_BENCHMARKS=ON`, target `bench_rsm`), so nothing depends on where Visual Studio happens to be installed.

## Automation

[`.github/workflows/benchmarks.yml`](../.github/workflows/benchmarks.yml) runs these scenarios on a pinned `windows-2022` runner:

- **Weekly (Mon 03:00 UTC)** and on demand: measure MSVC and MinGW and **commit the refreshed `results/ci-*.md` to `main` by itself** (`update_mode: auto-merge` by default; `pull-request` opens a PR and leaves it for review, `commit` to whatever branch the workflow runs on, `none` only measures). The commit is refused unless every changed path matches `benchmarks/results/ci-*.md`, so the pipeline cannot touch code or the desktop numbers.
- **On PRs touching `include/**`** (the library, not the harness or these files): a short run compared against the committed baseline. **Only S6 (sharded) and S7 (exclusive-only) gate the job**, at a 40% threshold.
  On a 4-core runner the read-parallelism scenarios sit at parity by construction - there are not enough cores for readers to overlap - so a 25% swing there is scheduling noise, not a regression (observed: S3 1.01x ? 0.76x while every other scenario stayed within ?6%). S6 and S7 measure this library's own wrapper cost, which is what a real regression would move.

It never merges a pull request: not one it opens itself (that mode is `pull-request`, and it leaves the PR alone) and certainly not a human's. The only automatic merge is the results branch the job created seconds earlier, after every changed path matched `benchmarks/results/ci-*.md`. Human PRs are only *checked* by the gate job, which runs `permissions: contents: read` and therefore cannot merge anything.

Two deliberate limits:

- Absolute ops/s belong to the runner that produced them. The number to look at is the speedup vs `recursive_mutex` measured **in the same run**.
- The gate only compares against a baseline that CI produced itself (the file says `Runner: GitHub Actions`). The hand-measured files are on different hardware, so the gate warns instead of failing until the workflow has refreshed them once.
- **GitHub runners have 4 cores.** Shared locking pays off when readers outnumber cores, so on CI the S1-S5 speedups collapse towards parity (measured: 0.8-1.06x) even though a 32-core desktop shows 13x. CI numbers are for run-to-run regression detection on fixed hardware, not for the absolute story; the run_bench.ps1 report says so inline when it sees fewer than 8 cores.
- Auto-merge needs no repository setting, because no pull request is involved. Only `update_mode: pull-request` needs Settings > Actions > General > *Allow GitHub Actions to create and approve pull requests*; the job warns and prints a compare link if that is off.

Before the metrics mean anything on a big machine, run it locally:

```powershell
# e.g. on a 32-core desktop
powershell -NoProfile -File benchmarks/run_bench.ps1 -Toolchain msvc -Seconds 3 -Repeats 3
```

## How precise are these numbers?

Not very, and the report says so: each one carries a `Run-to-run spread` line, the
observed min..max across the repeats. Measured on a busy 32-core desktop, the
median spread is around 40% and the balanced scenario has swung over 250% between
runs of the same binary. Treat the speedups as order of magnitude - "an order of
magnitude faster on read-heavy nested workloads", not "13.59x" - and use the
`ci-*.md` files, which are produced on one fixed runner, for drift.

`-Repeats` raises the confidence but not the machine's quietness: close
everything else before measuring, or the numbers describe your browser.

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

`results/<toolchain>.md` - measured on a real workstation, the representative numbers quoted elsewhere. Regenerate locally; CI does not touch them.

- [results/msvc.md](results/msvc.md)
- [results/mingw-gcc.md](results/mingw-gcc.md)

`results/ci-<toolchain>.md` - the 4-core GitHub runner, refreshed by the weekly workflow. Kept only so run-to-run drift can be detected and gated on identical hardware; **do not compare these with the files above**.

Each report records the backend it measured (`- Backend: ...`), because MinGW's numbers changed meaning in v0.3.0: it used to be `std::shared_mutex` over winpthreads and is now the native SRWLOCK wrapper. Reports generated before v0.3.0 say `std::shared_mutex` on MinGW and are not comparable with the ones after it.


- [results/ci-msvc.md](results/ci-msvc.md)
- [results/ci-mingw-gcc.md](results/ci-mingw-gcc.md)
