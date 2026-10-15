# Benchmark results: MinGW-GCC

- Dataset: 1,048,576 x uint32 (~4 MiB)
- Nested reenter depth: 3 (outer `try_*` + 2)
- Read window 24 / write window 12
- Duration/config: 3s
- Host hw_concurrency: 32
- Launch mode: one process per (scenario, impl) pair

## Speedup at a glance

Bar length ∝ total-ops/s speedup vs `recursive_mutex` (1 block ≈ 0.5x; above 1x = faster, below 1x = slower).

```
S1 read-heavy 15/1      1.03x  ██
S2 write-heavy 2/12     0.13x  ▎
S3 balanced 8/8         0.54x  █▏
S4 mixed ~20% write     0.95x  █▊
S5 mixed ~5% write      1.00x  ██
S6 sharded ~15% write   0.04x  ▏
S7 exclusive-only       0.11x  ▏
```

## Per-scenario verdict

| Scenario | Baseline ops/s | RSM ops/s | Speedup | Verdict |
|---|---:|---:|---:|---|
| S1 read-heavy (15/1) | 661,926 | 683,427 | 1.03x | ➖ roughly flat |
| S2 write-heavy (2/12) | 782,793 | 101,443 | 0.13x | ❌ much slower |
| S3 balanced (8/8) | 666,698 | 363,106 | 0.54x | ❌ slower |
| S4 mixed ~20% write | 686,016 | 649,151 | 0.95x | ➖ ~flat |
| S5 mixed ~5% write | 715,328 | 714,213 | 1.00x | ➖ flat |
| S6 sharded x8 ~15% write | 15,131,731 | 674,176 | 0.04x | ❌ worst case on this toolchain |
| S7 exclusive-only (no `shared_lock` yet) | 663,638 | 74,526 | 0.11x | ❌ large wrapper overhead on this toolchain |

<details>
<summary>Full metrics</summary>

| Scenario | Impl | R/W or N | Read ops | Write ops | Total ops/s | Read ops/s | Write ops/s | Total speedup |
|----------|------|----------|----------|-----------|-------------|------------|-------------|---------------|
| S1 read-heavy nested | `recursive_mutex` | 15/1 | 1848115 | 137664 | 661926 | 616038 | 45888 | 1x |
| S1 read-heavy nested | `RecursiveSharedMutex` | 15/1 | 2050272 | 8 | 683427 | 683424 | 3 | 1.03x |
| S2 write-heavy nested | `recursive_mutex` | 2/12 | 251114 | 2097265 | 782793 | 83705 | 699088 | 1x |
| S2 write-heavy nested | `RecursiveSharedMutex` | 2/12 | 78515 | 225814 | 101443 | 26172 | 75271 | 0.13x |
| S3 balanced nested | `recursive_mutex` | 8/8 | 912281 | 1087813 | 666698 | 304094 | 362604 | 1x |
| S3 balanced nested | `RecursiveSharedMutex` | 8/8 | 1081314 | 8004 | 363106 | 360438 | 2668 | 0.54x |
| S4 mixed ~20% write | `recursive_mutex` | N=16 | 1646715 | 411332 | 686016 | 548905 | 137111 | 1x |
| S4 mixed ~20% write | `RecursiveSharedMutex` | N=16 | 1947347 | 105 | 649151 | 649116 | 35 | 0.95x |
| S5 mixed ~5% write | `recursive_mutex` | N=16 | 2038555 | 107428 | 715328 | 679518 | 35809 | 1x |
| S5 mixed ~5% write | `RecursiveSharedMutex` | N=16 | 2142629 | 11 | 714213 | 714210 | 4 | 1x |
| S6 sharded x8 ~15% write | `recursive_mutex` | N=16 | 38612598 | 6782595 | 15131731 | 12870866 | 2260865 | 1x |
| S6 sharded x8 ~15% write | `RecursiveSharedMutex` | N=16 | 1881967 | 140561 | 674176 | 627322 | 46854 | 0.04x |
| S7 exclusive-only overhead | `recursive_mutex` | 8/8 | 895911 | 1095004 | 663638 | 298637 | 365001 | 1x |
| S7 exclusive-only overhead | `RSM exclusive-only` | 8/8 | 108340 | 115237 | 74526 | 36113 | 38412 | 0.11x |

</details>

Raw lines:

```
S1 read-heavy nested|recursive_mutex|15/1|1848115|137664|3
S1 read-heavy nested|RecursiveSharedMutex|15/1|2050272|8|3
S2 write-heavy nested|recursive_mutex|2/12|251114|2097265|3
S2 write-heavy nested|RecursiveSharedMutex|2/12|78515|225814|3
S3 balanced nested|recursive_mutex|8/8|912281|1087813|3
S3 balanced nested|RecursiveSharedMutex|8/8|1081314|8004|3
S4 mixed ~20% write|recursive_mutex|N=16|1646715|411332|3
S4 mixed ~20% write|RecursiveSharedMutex|N=16|1947347|105|3
S5 mixed ~5% write|recursive_mutex|N=16|2038555|107428|3
S5 mixed ~5% write|RecursiveSharedMutex|N=16|2142629|11|3
S6 sharded x8 ~15% write|recursive_mutex|N=16|38612598|6782595|3
S6 sharded x8 ~15% write|RecursiveSharedMutex|N=16|1881967|140561|3
S7 exclusive-only overhead|recursive_mutex|8/8|895911|1095004|3
S7 exclusive-only overhead|RSM exclusive-only|8/8|108340|115237|3
```
