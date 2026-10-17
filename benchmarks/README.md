# Benchmarks vs `std::recursive_mutex`

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

- **Weekly (Mon 03:00 UTC)** and on demand: measure MSVC and MinGW, open a PR that refreshes `results/ci-*.md`, and **merge it automatically** (`update_mode: auto-merge` by default; `pull-request` leaves it for review, `commit` pushes straight to the branch, `none` only measures). The auto-merge refuses anything outside `benchmarks/results/ci-*.md`, so the pipeline cannot touch code or the desktop numbers.
- **On PRs touching `include/**` or `benchmarks/**`**: a short run compared against the committed baseline. A scenario that drops by more than 25% fails the job.

Two deliberate limits:

- Absolute ops/s belong to the runner that produced them. The number to look at is the speedup vs `recursive_mutex` measured **in the same run**.
- The gate only compares against a baseline that CI produced itself (the file says `Runner: GitHub Actions`). The hand-measured files are on different hardware, so the gate warns instead of failing until the workflow has refreshed them once.
- **GitHub runners have 4 cores.** Shared locking pays off when readers outnumber cores, so on CI the S1-S5 speedups collapse towards parity (measured: 0.8-1.06x) even though a 32-core desktop shows 13x. CI numbers are for run-to-run regression detection on fixed hardware, not for the absolute story; the run_bench.ps1 report says so inline when it sees fewer than 8 cores.
- Opening the refresh PR needs Settings > Actions > General > *Allow GitHub Actions to create and approve pull requests*. Without it the job still measures and pushes `bench/refresh-<toolchain>`, prints a compare link, and warns - it does not fail.

Before the metrics mean anything on a big machine, run it locally:

```powershell
# e.g. on a 32-core desktop
powershell -NoProfile -File benchmarks/run_bench.ps1 -Toolchain msvc -Seconds 3 -Repeats 3
```

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

- [results/ci-msvc.md](results/ci-msvc.md)
- [results/ci-mingw-gcc.md](results/ci-mingw-gcc.md)
