# Platform notes: measured behaviour per toolchain

**English** · [简体中文](platform-notes.zh-CN.md)

All numbers were measured on one Windows x64 host (8 threads for the contended
runs, `-O2`/`/O2`, 2 M iterations for the uncontended runs). Absolute numbers are
machine-specific; the *ratios* and the pass/fail results are what matter, and the
`S7` ratios were taken as the median of 5 interleaved 1 s rounds because
single-shot numbers on this host swing by 1.1×–3.2×.

## 1. Uncontended cost (ns per lock/unlock pair)

| Implementation | MinGW-GCC 13.2 (UCRT) | MSVC 19.51 | clang 22.1 (MSVC STL) |
|---|---:|---:|---:|
| `rsm::RecursiveSharedMutex` exclusive | 61.5 | 5.8 | 5.9 |
| `rsm::RecursiveSharedMutex` shared | 73.7 | 7.2 | 7.4 |
| nested exclusive (×2) | 71.4 | 9.9 | 10.6 |
| nested shared (×2) | 105.2 | 12.0 | 11.2 |
| `std::recursive_mutex` | 12.5 | 4.7 | 4.7 |
| `std::shared_mutex` (the backend floor) | 37.4 | 3.3 | 3.2 |
| `sizeof(std::thread::id)` | 8 | 4 | 4 |
| `sizeof(rsm::RecursiveSharedMutex)` | 24 | 24 | 24 |
| `sizeof(std::recursive_mutex)` | 8 | 80 | 80 |

Measured on v0.2.x. Since v0.3.0 the MinGW column describes the **`RSM_BACKEND_STD`**
configuration (MinGW now defaults to the native backend; its uncontended cost is in
section 6), and the MSVC exclusive row is **4.6 ns** after the exclusive fast path
stopped scanning the slot table (shared: 6.8 ns).

Reading: the wrapper costs ≈1.7× its backend on every toolchain, but the
*absolute* cost is dominated by the backend (11× slower on MinGW) and by TLS
access (emutls on MinGW is an order of magnitude more expensive than the
`__declspec(thread)`/TLS-descriptor paths used by MSVC and clang). On MSVC/clang
the whole component is within 25 % of `std::recursive_mutex`; on MinGW it is 5×
slower. Note that the shared path is *more* expensive than the exclusive path
when uncontended — the TLS slot lookup has to pay for itself with real reader
concurrency.

## 2. Exclusive-only path (the benchmarks' S7 shape)

8 threads, `try_lock` + `yield` spin, nesting depth 3, median of 5 interleaved
1 s rounds:

| Toolchain | `recursive_mutex` | this library | ratio |
|---|---:|---:|---:|
| MSVC 19.51 | 8,167,734 ops/s | 9,532,459 ops/s | **1.17×** |
| clang 22.1 (MSVC STL) | 8,068,846 ops/s | 11,468,567 ops/s | **1.42×** |
| MinGW-GCC 13.2 | 4,986,027 ops/s | 335,929 ops/s | **0.067×** |

The MinGW row is `RSM_BACKEND_STD`; the native backend reaches 1.52× in section 6.
The checked-in benchmark tables measure a different harness (3 s × 5 runs with real work
between lock operations) and report 2.59× (MSVC) and 1.32× (MinGW, native backend) for its S7
shape: the ratios are not directly comparable, and the exact value is protocol-dependent. The MinGW run here also threw
spurious `operation_not_permitted` twice during the five rounds, which is the symptom
described in section 3.

## 3. The backend contract (why MinGW is not just slow)

`std::shared_mutex` is only required to be as good as its implementation. On
MinGW-w64 GCC 13.2 / clang with libstdc++ over **winpthreads** - that is
`RSM_BACKEND_STD`, since the default on MinGW is the native backend - `try_lock()` is
not mutually exclusive: under a `try_lock`/`unlock` storm two threads can both
hold the write lock. An 8-thread storm with a ~200 ns critical section:

| Toolchain | two exclusive owners | reader inside while writer inside |
|---|---:|---:|
| MSVC 19.51 | 0 / 134,371 | 0 / 294,219 |
| clang 22.1 (MSVC STL) | 0 / 200,927 | 0 / 323,282 |
| MinGW-GCC 13.2 | **3–171 / ~140,000** | **268–502 / ~500,000** |
| clang 22.1 + libstdc++/winpthreads | **135 / 140,080** | **268 / 489,375** |

The defect follows the runtime, not the compiler frontend. Consequences for this
library:

- `try_lock()` can return `true` for a thread that does not own the mutex, so
  `owner_` / `exclusive_count_` get clobbered by the "second owner", and the real
  owner's `unlock()` throws `operation_not_permitted` (≈1 event per 50,000
  successful acquisitions). This is inherited, not caused by the wrapper.
- Benchmarks whose readers spin on `try_lock`/`try_lock_shared` (all of
  `benchmarks/bench_vs_recursive_mutex.cpp`) are measuring a broken primitive on
  that toolchain, so the MinGW table cannot be used as a design verdict.

`tests/backend_contract.cpp` checks this in one file with no dependencies; it is
the first job in the CI matrix. It also passes when one thread holds the lock and
another probes with `try_lock`, so a "steady holder" test will not catch it — the
window is the `unlock`/`try_lock` interleaving.

## 4. Other configurations measured

| Configuration | Backend contract | Library mutual exclusion |
|---|---|---|
| MSVC x64 Release / Debug | pass | pass |
| MSVC x86 (32-bit) Release | pass | pass |
| clang-cl x64 Release / Debug | pass | pass |
| clang x64 Release (MSVC STL) | pass | pass |
| MinGW-GCC default (native SRWLOCK) | pass | pass |
| MinGW-GCC `RSM_BACKEND_STD` | **fail** (raw backend) | pass (blocking paths dominate) |
| clang + libstdc++/winpthreads (same backend as `RSM_BACKEND_STD`) | **fail** | pass |

How CI watches this: `.github/workflows/platform-matrix.yml` runs one weekly job,
`expected-fail / mingw winpthreads (red = upstream fixed)`, which builds against
`RSM_BACKEND_STD` and **expects** the contract to be violated. It is green while the
defect exists and turns red only when the toolchain stops violating the contract,
which is the signal to delete it.

32-bit detail: `sizeof(rsm::RecursiveSharedMutex)` drops to 16, `lock+unlock` is
7.9 ns and `std::recursive_mutex` 9.7 ns — on that configuration the wrapper is
faster than what it replaces.

Not covered here (only reachable from Linux/macOS CI): glibc and musl rwlock
semantics, libc++'s `shared_mutex`, ThreadSanitizer, and weak-memory-ordering
hardware. `.github/workflows/platform-matrix.yml` covers those.

## 5. Reproducing

```bash
# backend contract, any toolchain, no build system needed
c++ -O2 -std=c++17 -pthread -Iinclude tests/backend_contract.cpp -o bc && ./bc
cl /O2 /std:c++17 /EHsc /Iinclude tests\backend_contract.cpp
g++ -O2 -std=c++17 -pthread -Iinclude tests/backend_contract.cpp -o bc.exe && ./bc.exe

# library-level mutual exclusion
c++ -O2 -std=c++17 -pthread -Iinclude tests/mutual_exclusion.cpp -o me && ./me

# full suite
cmake -S . -B build -DRSM_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build

# cross-module TLS domain (real DLL/.so; expected to show "separate TLS domain")
cd examples/cross_module_hazard && ./build_and_run.sh     # Linux/macOS
build_and_run.cmd msvc                                     # Windows
```

## 6. The SRWLOCK backend (the default on MinGW)

`RSM_BACKEND_SRWLOCK` replaces `std::shared_mutex` with an 8-byte SRWLOCK wrapper
inside the same class. Since the version that introduced `RSM_BACKEND_STD`,
MinGW-w64 selects it automatically: the alternative is unsound there, not just
slow. `-DRSM_BACKEND_STD` restores `std::shared_mutex`. Measured on MinGW-w64 GCC
13.2, same binary layout, same tests:

| | `RSM_BACKEND_STD` (`std::shared_mutex`) | `RSM_BACKEND_SRWLOCK` (default) |
|---|---:|---:|
| `lock+unlock` | 62.7 ns/pair | **24.2 ns/pair** (2.6×) |
| `lock_shared+unlock_shared` | 64.4 ns/pair | **25.8 ns/pair** (2.5×) |
| 2× nested exclusive | 72.5 ns | **33.9 ns** |
| 2× nested shared | 86.7 ns | **47.2 ns** |
| S7 shape vs `std::recursive_mutex` | 0.068× | **1.523×** |
| `backend_contract` | **fails** | **passes** |
| full suite | **fails** | **passes** |

The shared rows were re-measured on v0.4.0: `is_held_exclusive()` now skips the
`pthread_self()` call when no exclusive owner exists, which helps both backends.
The exclusive and S7 rows were not affected by that change and predate it.

On MSVC the switch is a no-op in effect (`std::shared_mutex` already is SRWLOCK):
5.8 vs 5.7 ns, contract passes either way.

What it does **not** fix: the ~20 ns of wrapper overhead that remains on MinGW is
the TLS/emutls cost of the slot table (`sizeof(RecursiveSharedMutex)` is 24 B on
both backends). Removing that needs the "instance holds nothing but the backend"
refactor — measured at ≈40 ns total on the std backend and ≈4 ns with SRWLOCK in
a prototype, i.e. it is the next 3× on that platform, not this one.

## 7. Tried and rejected (with numbers)

Two further optimizations were prototyped and measured before being dropped.
They are recorded so nobody repeats the work, and because the negative results
say something about where the cost actually is. The baselines here are from
v0.3.x; v0.4.0 lowered the shared rows (section 6), so the ratios below are
historical.

### TLS-only state (the instance holds nothing but the backend)

Move `owner_` / `exclusive_count_` / `under_exclusive_` into the slot table, so
the object is 8 bytes and no thread id is ever read. The same suite, all passing
on MSVC, clang and MinGW + SRWLOCK:

| | current (hybrid) | TLS-only |
|---|---:|---:|
| MinGW + SRWLOCK: exclusive | 23.4 ns | 27.5 ns |
| MinGW + SRWLOCK: 2× nested exclusive | 32.9 ns | **52.0 ns** |
| MinGW + SRWLOCK: shared | 35.1 ns | **15.7 ns** |
| MinGW + SRWLOCK: 2× nested shared | 66.1 ns | **28.6 ns** |
| MSVC: exclusive / shared | 5.8 / 7.2 ns | 5.9 / 6.1 ns |
| `sizeof` | 24 B | **8 B** |

Why it loses on MinGW: the current design keeps the *recursive-exclusive* fast
path completely off TLS (it is two instance-side atomics), whereas a TLS-only
design pays a slot lookup on every `lock()` **and** every `unlock()`. Since the
exclusive path is the one that MinGW needs help with, the refactor trades the
platform in question for the ones that are already fine. Rejected.

### One-entry slot hint + a single TLS fetch per operation

`findActiveSlot()` fetched the thread-local table twice, and the shared paths
fetched it three times per operation; a validated one-entry hint plus one fetch
per call site should have removed that. Measured on MinGW:

| | before | after |
|---|---:|---:|
| exclusive `lock+unlock` | 62.8 ns | 61.7 ns |
| shared `lock_shared+unlock_shared` | 76.4 ns | 74.4 ns |
| exclusive, `RSM_BACKEND_SRWLOCK` | 23.4 ns | 23.2 ns |
| shared, `RSM_BACKEND_SRWLOCK` | 35.1 ns | 36.1 ns |

All inside noise. Conclusion: the per-operation thread-local fetch is **not** the
cost centre on this toolchain (a `thread_local` int write measures ~0.4 ns), so
the extra table state was not kept.

### What that leaves

With `RSM_BACKEND_SRWLOCK`, the wrapper costs ~20 ns per pair over the raw
SRWLOCK (23.2 vs 3.4). It is spread across `std::this_thread::get_id()`
(winpthreads `pthread_self`), two relaxed atomics on `owner_`, the slot
bookkeeping and the backend call — no single micro-change moved it. The next
step should be a profiler run on the target toolchain, not more guessing;
`benchmarks/` currently cannot separate those contributions.
