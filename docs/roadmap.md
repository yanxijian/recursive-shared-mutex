# Roadmap

RecursiveSharedMutex is a **transition component**: its purpose is to carry a
codebase from `std::recursive_mutex` to reader/writer locking until the
codebase can be de-recursivized. This roadmap covers the remaining lifecycle.

## R1 - Binding-constraint seal + compile-time migration notice (immediate, comment/preprocessor-only)

Status: **landed** in the header (THREAD-BINDING CONSTRAINT block + compile-time
notice). Acceptance was verified on MinGW-w64 GCC 13.2: `-std=c++17` builds
emit nothing, `-std=c++20` emits the notice, boundary suite unchanged.

1. Add a prominent OS-thread-binding constraint block to the header. Precise
wording matters - the violation mechanism is fail-fast + stranded slot, not
memory corruption:

- The component is bound to OS native threads; all shared-recursion
  accounting lives in per-thread TLS.
- FORBIDDEN wherever a task may resume on a different physical thread:
  thread-migrating coroutines, work-stealing pools (TBB etc.), user-mode
  fibers with migration.
- SAFE: stackful coroutines/fibers that always resume on the acquiring
  thread; C++20 coroutines when the executor guarantees same-thread resume.
- Violation consequence: `unlock_shared()` from a foreign thread throws
  `operation_not_permitted`; the stranded slot starves writers; an exception
  escaping a destructor terminates the process. (Heap corruption is NOT the
  mechanism here - that was the separate emutls TLS-dtor defect, fixed in
  v0.2.0. Do not conflate them when debugging.)

2. Compile-time migration notice, fired by feature-test macro (C++17 builds
see nothing; the notice fires exactly when a future bump enables coroutines):

```cpp
#if defined(__cpp_impl_coroutine) && __cpp_impl_coroutine >= 201902L \
    && !defined(RSM_NO_THREAD_MIGRATION_NOTICE)
#pragma message("RecursiveSharedMutex is bound to OS threads: a task that " \
                "resumes on a different physical thread will strand its "   \
                "shared slot and starve writers. Same-thread resume is "    \
                "safe. See the thread-binding notes above.")
#endif
```

- Note-level (`#pragma message`), not `#warning`: enabling coroutine language
  mode is not misuse by itself; same-thread resume is legitimate.
- Opt-out: `-DRSM_NO_THREAD_MIGRATION_NOTICE`.
- Honest limits: nothing compile-time can detect thread migration or
  work-stealing executors (a runtime property), and C++17 does NOT exclude
  C++17-usable stackful coroutines/fibers or TBB-style pools - those are
  covered only by the runtime affinity guard (validation kit).

Acceptance: comment/preprocessor-only change; boundary suite unchanged;
building one TU with `-std=c++20` shows the notice, `-std=c++17` shows none.

## R2 - Writer-starvation observability (short term, consuming project)

Empirical basis: MinGW bench S1/S5 writer throughput collapses to single
digits under read pressure; glibc `pthread_rwlock` defaults to
`PTHREAD_RWLOCK_PREFER_READER_NP`, so Linux production is equally exposed.

Instrument at the guard layer (keep the library itself zero-overhead):

- `rsm_write_wait_seconds` histogram: time spent inside `lock()` / `try_lock`
  retry loops, per mutex site.
- `rsm_write_hold_seconds` histogram: exclusive critical-section duration.
- Alert: p99 write-wait above threshold for N consecutive minutes.

Mitigations once starvation is observed: writer batching, reader throttling,
sharding to reduce reader contention per lock, or R4.

## R3 - De-recursivization program (mid term, consuming project)

Interface/core separation, applied per module in call-graph order:

- Public API takes `WriteLock` (`std::unique_lock`) or `ReadLock`
  (`std::shared_lock`) at the outermost scope.
- Private core functions (`xxx_NoLock`) perform no locking and may freely
  call each other; they must document which lock mode their caller must hold.

Safety rails:

- Debug-only hold assertion for exclusive cores: `assert(!mtx.try_lock())`
  is true only when the caller already holds the lock exclusively.
- Shared cores cannot be asserted this way (`try_lock_shared` legitimately
  succeeds); they rely on call-graph discipline plus the boundary test suite
  run after every step.
- Read-then-write chains disappear naturally in this pattern: the outermost
  scope decides the mode, so no upgrade attempt can ever occur.

Payoff: re-entrancy is the ROOT CAUSE of every documented constraint of this
component (TLS domain rule, 256-slot cap, thread-migration ban, cross-module
shared-path rule). Once no re-entrancy remains, replace the whole component
with `std::shared_mutex` and all of those constraints disappear with it.

Exit: mark RecursiveSharedMutex deprecated in the consuming codebase; the
library remains available for other consumers.

## R4 - (optional, library-side) pluggable fair backend

Only if R2 proves starvation in production: implement a writer-preferring
fair rwlock exposing the same `SharedMutex` surface so RecursiveSharedMutex
becomes backend-pluggable (`std::shared_mutex` remains the default backend).
Do not build this speculatively - unfairness is the standard's trade-off and
a fair backend costs readers.

## Pending

- Re-measure `benchmarks/results/msvc.md` against v0.2.1 on an MSVC host
  (current tables are v0.2.0-era; densification should improve S7/mixed).
