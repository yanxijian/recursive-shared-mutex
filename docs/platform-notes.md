# Platform notes: measured behaviour per toolchain

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

The checked-in benchmark tables report 0.93× (MSVC) and 0.10× (MinGW); both are
in the right ballpark — the MinGW cell is if anything worse in this harness. The
MinGW run also threw spurious `operation_not_permitted` twice during the five
rounds, which is the symptom described in section 3.

## 3. The backend contract (why MinGW is not just slow)

`std::shared_mutex` is only required to be as good as its implementation. On
MinGW-w64 GCC 13.2 / clang with libstdc++ over **winpthreads**, `try_lock()` is
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
| MinGW-GCC Release / Debug | **fail** | pass (blocking paths dominate) |
| clang + libstdc++/winpthreads | **fail** | pass |

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
