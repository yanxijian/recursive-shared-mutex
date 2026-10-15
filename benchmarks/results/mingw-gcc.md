# Benchmark results: MinGW-GCC

- Measured against: **v0.2.1** (dense shared-slot table)
- Dataset: 1,048,576 x uint32 (~4 MiB)
- Nested reenter depth: 3 (outer `try_*` + 2)
- Read window 24 / write window 12
- Duration/config: 3s
- Host hw_concurrency: 32
- Launch mode: one process per (scenario, impl) pair

## Speedup at a glance

Bar length ∝ total-ops/s speedup vs `recursive_mutex` (1 block ≈ 0.5x; above 1x = faster, below 1x = slower).

```
S1 read-heavy 15/1      0.96x  ██
S2 write-heavy 2/12     0.13x  ▎
S3 balanced 8/8         1.48x  ███
S4 mixed ~20% write     0.91x  █▊
S5 mixed ~5% write      0.94x  █▉
S6 sharded ~15% write   0.05x  ▏
S7 exclusive-only       0.10x  ▏
```

## Per-scenario verdict

| Scenario | Baseline ops/s | RSM ops/s | Speedup | Verdict |
|---|---:|---:|---:|---|
| S1 read-heavy (15/1) | 739,646 | 710,477 | 0.96x | ➖ roughly flat; writer starves under heavy readers (write ops ≈ 0) |
| S2 write-heavy (2/12) | 821,725 | 104,116 | 0.13x | ❌ much slower |
| S3 balanced (8/8) | 243,007 | 359,652 | 1.48x | ✅ faster on this run (baseline run showed unusually low absolute ops; re-run before drawing conclusions) |
| S4 mixed ~20% write | 730,144 | 662,618 | 0.91x | ➖ slightly slower |
| S5 mixed ~5% write | 778,547 | 734,984 | 0.94x | ➖ ~flat |
| S6 sharded x8 ~15% write | 13,185,589 | 709,338 | 0.05x | ❌ worst case on this toolchain |
| S7 exclusive-only (no `shared_lock` yet) | 713,590 | 74,894 | 0.10x | ❌ large wrapper overhead on this toolchain (TLS scan + `thread_local` access); the densification removed the full-table scan, not the per-op TLS cost |

<details>
<summary>Full metrics</summary>

| Scenario | Impl | R/W or N | Read ops | Write ops | Total ops/s | Read ops/s | Write ops/s | Total speedup |
|----------|------|----------|----------|-----------|-------------|------------|-------------|---------------|
| S1 read-heavy nested | `recursive_mutex` | 15/1 | 2062553 | 156385 | 739646 | 687518 | 52128 | 1x |
| S1 read-heavy nested | `RecursiveSharedMutex` | 15/1 | 2131429 | 2 | 710477 | 710476 | 1 | 0.96x |
| S2 write-heavy nested | `recursive_mutex` | 2/12 | 300437 | 2164739 | 821725 | 100146 | 721580 | 1x |
| S2 write-heavy nested | `RecursiveSharedMutex` | 2/12 | 90432 | 221915 | 104116 | 30144 | 73972 | 0.13x |
| S3 balanced nested | `recursive_mutex` | 8/8 | 367408 | 361612 | 243007 | 122469 | 120537 | 1x |
| S3 balanced nested | `RecursiveSharedMutex` | 8/8 | 1070820 | 8135 | 359652 | 356940 | 2712 | 1.48x |
| S4 mixed ~20% write | `recursive_mutex` | N=16 | 1752798 | 437633 | 730144 | 584266 | 145878 | 1x |
| S4 mixed ~20% write | `RecursiveSharedMutex` | N=16 | 1987765 | 90 | 662618 | 662588 | 30 | 0.91x |
| S5 mixed ~5% write | `recursive_mutex` | N=16 | 2218305 | 117336 | 778547 | 739435 | 39112 | 1x |
| S5 mixed ~5% write | `RecursiveSharedMutex` | N=16 | 2204924 | 27 | 734984 | 734975 | 9 | 0.94x |
| S6 sharded x8 ~15% write | `recursive_mutex` | N=16 | 33645962 | 5910805 | 13185589 | 11215321 | 1970268 | 1x |
| S6 sharded x8 ~15% write | `RecursiveSharedMutex` | N=16 | 1980007 | 148008 | 709338 | 660002 | 49336 | 0.05x |
| S7 exclusive-only overhead | `recursive_mutex` | 8/8 | 897201 | 1243570 | 713590 | 299067 | 414523 | 1x |
| S7 exclusive-only overhead | `RSM exclusive-only` | 8/8 | 109181 | 115501 | 74894 | 36394 | 38500 | 0.1x |

</details>

Raw lines:

```
S1 read-heavy nested|recursive_mutex|15/1|2062553|156385|3
S1 read-heavy nested|RecursiveSharedMutex|15/1|2131429|2|3
S2 write-heavy nested|recursive_mutex|2/12|300437|2164739|3
S2 write-heavy nested|RecursiveSharedMutex|2/12|90432|221915|3
S3 balanced nested|recursive_mutex|8/8|367408|361612|3
S3 balanced nested|RecursiveSharedMutex|8/8|1070820|8135|3
S4 mixed ~20% write|recursive_mutex|N=16|1752798|437633|3
S4 mixed ~20% write|RecursiveSharedMutex|N=16|1987765|90|3
S5 mixed ~5% write|recursive_mutex|N=16|2218305|117336|3
S5 mixed ~5% write|RecursiveSharedMutex|N=16|2204924|27|3
S6 sharded x8 ~15% write|recursive_mutex|N=16|33645962|5910805|3
S6 sharded x8 ~15% write|RecursiveSharedMutex|N=16|1980007|148008|3
S7 exclusive-only overhead|recursive_mutex|8/8|897201|1243570|3
S7 exclusive-only overhead|RSM exclusive-only|8/8|109181|115501|3
```
