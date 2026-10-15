# Benchmarks vs `std::recursive_mutex`

Compares `rsm::RecursiveSharedMutex` (readers use shared locks unless noted) against `std::recursive_mutex` under nested locking and a ~4 MiB working set.

## Run

```powershell
# From this directory
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_bench.ps1 -Toolchain both -Seconds 3
```

Each (scenario, implementation) pair runs in a **fresh process** so MinGW `shared_mutex` stress cannot poison later configs in-process.

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
