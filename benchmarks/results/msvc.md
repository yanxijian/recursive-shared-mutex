# Benchmark results: MSVC

- Measured against: **v0.2.0** (pre-densification slot table) — re-measure with MSVC for v0.2.1; the densification removes the per-op full-table scan, so the exclusive-only (S7) and mixed scenarios should improve
- Dataset: 1,048,576 x uint32 (~4 MiB)
- Nested reenter depth: 3 (outer `try_*` + 2)
- Read window 24 / write window 12
- Duration/config: 3s
- Host hw_concurrency: 32
- Launch mode: one process per (scenario, impl) pair

## Speedup at a glance

Bar length ∝ total-ops/s speedup vs `recursive_mutex` (1 block ≈ 0.5x; above 1x = faster, below 1x = slower).

```
S1 read-heavy 15/1     13.59x  ███████████████████████████▏
S2 write-heavy 2/12     1.97x  ███▉
S3 balanced 8/8        12.27x  ████████████████████████▌
S4 mixed ~20% write    13.51x  ███████████████████████████
S5 mixed ~5% write      6.09x  ████████████▏
S6 sharded ~15% write   0.59x  █▏
S7 exclusive-only       0.93x  █▊
```

## Per-scenario verdict

| Scenario | Baseline ops/s | RSM ops/s | Speedup | Verdict |
|---|---:|---:|---:|---|
| S1 read-heavy (15/1) | 630,477 | 8,571,281 | **13.59x** | ✅ biggest win; writer starves under heavy readers (write ops ≈ 0) |
| S2 write-heavy (2/12) | 541,571 | 1,064,699 | **1.97x** | ✅ write path benefits too once readers go shared |
| S3 balanced (8/8) | 457,529 | 5,613,197 | **12.27x** | ✅ |
| S4 mixed ~20% write | 432,530 | 5,841,350 | **13.51x** | ✅ |
| S5 mixed ~5% write | 1,089,740 | 6,632,829 | **6.09x** | ✅ |
| S6 sharded x8 ~15% write | 13,641,670 | 8,020,888 | 0.59x | ❌ sharding already removed contention; shared locking is slower here |
| S7 exclusive-only (no `shared_lock` yet) | 1,207,639 | 1,124,432 | 0.93x | ⚠️ ~7% pure wrapper overhead, acceptable as migration step 1 |

<details>
<summary>Full metrics</summary>

| Scenario | Impl | R/W or N | Read ops | Write ops | Total ops/s | Read ops/s | Write ops/s | Total speedup |
|----------|------|----------|----------|-----------|-------------|------------|-------------|---------------|
| S1 read-heavy nested | `recursive_mutex` | 15/1 | 1775482 | 115950 | 630477 | 591827 | 38650 | 1x |
| S1 read-heavy nested | `RecursiveSharedMutex` | 15/1 | 25713843 | 1 | 8571281 | 8571281 | 0 | 13.59x |
| S2 write-heavy nested | `recursive_mutex` | 2/12 | 200242 | 1424470 | 541571 | 66747 | 474823 | 1x |
| S2 write-heavy nested | `RecursiveSharedMutex` | 2/12 | 1206990 | 1987107 | 1064699 | 402330 | 662369 | 1.97x |
| S3 balanced nested | `recursive_mutex` | 8/8 | 687508 | 685079 | 457529 | 229169 | 228360 | 1x |
| S3 balanced nested | `RecursiveSharedMutex` | 8/8 | 16831569 | 8021 | 5613197 | 5610523 | 2674 | 12.27x |
| S4 mixed ~20% write | `recursive_mutex` | N=16 | 1037354 | 260235 | 432530 | 345785 | 86745 | 1x |
| S4 mixed ~20% write | `RecursiveSharedMutex` | N=16 | 17523933 | 117 | 5841350 | 5841311 | 39 | 13.51x |
| S5 mixed ~5% write | `recursive_mutex` | N=16 | 3104870 | 164350 | 1089740 | 1034957 | 54783 | 1x |
| S5 mixed ~5% write | `RecursiveSharedMutex` | N=16 | 19898477 | 10 | 6632829 | 6632826 | 3 | 6.09x |
| S6 sharded x8 ~15% write | `recursive_mutex` | N=16 | 34786785 | 6138225 | 13641670 | 11595595 | 2046075 | 1x |
| S6 sharded x8 ~15% write | `RecursiveSharedMutex` | N=16 | 22489081 | 1573583 | 8020888 | 7496360 | 524528 | 0.59x |
| S7 exclusive-only overhead | `recursive_mutex` | 8/8 | 2015176 | 1607741 | 1207639 | 671725 | 535914 | 1x |
| S7 exclusive-only overhead | `RSM exclusive-only` | 8/8 | 1610601 | 1762695 | 1124432 | 536867 | 587565 | 0.93x |

</details>

Raw lines:

```
S1 read-heavy nested|recursive_mutex|15/1|1775482|115950|3
S1 read-heavy nested|RecursiveSharedMutex|15/1|25713843|1|3
S2 write-heavy nested|recursive_mutex|2/12|200242|1424470|3
S2 write-heavy nested|RecursiveSharedMutex|2/12|1206990|1987107|3
S3 balanced nested|recursive_mutex|8/8|687508|685079|3
S3 balanced nested|RecursiveSharedMutex|8/8|16831569|8021|3
S4 mixed ~20% write|recursive_mutex|N=16|1037354|260235|3
S4 mixed ~20% write|RecursiveSharedMutex|N=16|17523933|117|3
S5 mixed ~5% write|recursive_mutex|N=16|3104870|164350|3
S5 mixed ~5% write|RecursiveSharedMutex|N=16|19898477|10|3
S6 sharded x8 ~15% write|recursive_mutex|N=16|34786785|6138225|3
S6 sharded x8 ~15% write|RecursiveSharedMutex|N=16|22489081|1573583|3
S7 exclusive-only overhead|recursive_mutex|8/8|2015176|1607741|3
S7 exclusive-only overhead|RSM exclusive-only|8/8|1610601|1762695|3
```
