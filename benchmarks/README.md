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

- **Weekly (Mon 03:00 UTC)** and on demand: measure MSVC and MinGW, then open a PR that refreshes `results/*.md` (`update_mode: pull-request` by default; `commit` and `none` are available).
- **On PRs touching `include/**` or `benchmarks/**`**: a short run compared against the committed baseline. A scenario that drops by more than 25% fails the job.

Two deliberate limits:

- Absolute ops/s belong to the runner that produced them. The number to look at is the speedup vs `recursive_mutex` measured **in the same run**.
- The gate only compares against a baseline that CI produced itself (the file says `Runner: GitHub Actions`). The hand-measured files are on different hardware, so the gate warns instead of failing until the workflow has refreshed them once.

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

- [results/msvc.md](results/msvc.md)
- [results/mingw-gcc.md](results/mingw-gcc.md)
