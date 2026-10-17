# Migration guide: `recursive_mutex` → `rsm::RecursiveSharedMutex`

## Goal

Introduce reader/writer locking without immediately flattening every nested call that previously relied on `std::recursive_mutex`.

Longer term: remove recursion, switch to plain `std::shared_mutex`.

## Implementation notes

Canonical code: [`include/recursive_shared_mutex/recursive_shared_mutex.hpp`](../include/recursive_shared_mutex/recursive_shared_mutex.hpp).

| Concern | Approach |
|---------|----------|
| Exclusive recursion | `std::atomic<std::thread::id> owner_` + owner-only `exclusive_count_` / `under_exclusive_` |
| Shared recursion | per-thread table of POD slots keyed by the mutex address (`this`); active slots dense in `[0, used)`, recycled via swap-with-last compaction on unlock-to-zero |
| Upgrade (shared → exclusive) | `lock()` throws `resource_deadlock_would_occur`; `try_lock()` returns `false` |

Shared depth is not stored in `thread_local unordered_map` (allocator path under contended `shared_mutex` is fragile on some MinGW builds) — and not in a heap-allocated `thread_local` list either: freeing nodes inside a `thread_local` destructor reliably corrupts the heap at thread exit on MinGW-w64 GCC 13.2 ucrt/posix (emutls; verified with gdb). The slot table is constant-initialized POD with no destructor and no heap allocation; slots are keyed by the mutex address and recycled when the depth drops to zero, so the working set tracks currently held shared locks, not mutexes ever touched. Acquire failures (table exhausted, or the test hook injecting a failure) are raised BEFORE the underlying lock is taken, so an error can never strand a shared slot.

## Steps

1. Replace `std::recursive_mutex` with `rsm::RecursiveSharedMutex`.
2. Keep writers on `std::unique_lock`.
3. Audit for read-then-write on the same lock (hard gate), then change pure readers to `std::shared_lock`.
4. Fix any shared→exclusive nesting by taking a write lock at the outermost scope.

## Hard rules

- No lock upgrade (shared then exclusive on the same mutex).
- No cross-thread unlock (TLS / owner id).
- Do not destroy a mutex while it is still locked.
- Cross-module (DLL/EXE) use: the TLS slot table is per-module. A single mutex's `lock_shared`/`unlock_shared` sequence must stay within one module's TLS domain; if a mutex is called from several modules, host the TLS state in a single exported module instead of relying on the header-only TLS. This is not a "stranded slot" edge case: the second module's `lock_shared()` finds no slot and calls the backend's `lock_shared()` again on a thread that already holds it in shared mode, which is **undefined behaviour** per `[thread.sharedmutex]`. SRWLOCK and reader-preferring glibc/musl rwlocks accept it silently; a writer-preferring rwlock can deadlock the calling thread. `examples/cross_module_hazard/` reproduces it with a real DLL/.so and is wired into CI.
- Read-then-write on the same lock serialized under `recursive_mutex` but now throws `resource_deadlock_would_occur`; audit before moving any reader to `shared_lock`.
- Writer starvation: `std::shared_mutex` has no writer preference. Under continuous readers, writers may starve (see benchmarks S1/S5); add upstream throttling/yielding if writes have latency requirements.
- Per-thread cap: at most `kSharedSlotCount` (256) shared locks may be held concurrently by one thread (≈4 KB TLS per thread). The next `lock_shared` throws `resource_unavailable_try_again`; `try_lock_shared` returns `false` instead, so `std::shared_lock(mtx, std::try_to_lock)` stays usable. Slots recycle on unlock, so sequentially touching any number of mutexes is fine.
- No `std::condition_variable_any` once recursion depth > 1: `wait()` unlocks only one level, the mutex stays held and the waiting thread never wakes.
- The backend must honour mutual exclusion (`try_lock` grants at most one owner, no reader inside while a writer is). MinGW-w64 GCC 13.2 over winpthreads does not; `-DRSM_BACKEND_SRWLOCK` replaces it with a native SRWLOCK and fixes both the correctness defect and the 5×–15× exclusive-path regression (see `docs/platform-notes.md` §6). Run `tests/backend_contract.cpp` on the target toolchain before migrating.

## Stranded slots and the rejected escape valve

A shared slot is stranded forever when `unlock_shared()` runs on a thread (or in a module) that does not own the slot: it throws `operation_not_permitted`, and the acquiring thread's slot stays at `{key, depth=1}`. Consequences: `try_lock()` keeps returning `false` and writers starve until the owning thread releases properly.

An upstream `release_shared_unchecked()` (skip TLS accounting, call the underlying `unlock_shared()` directly) was evaluated and REJECTED: it would leave the owner's slot at `{key, depth=1}` while the underlying lock is already free; any later RAII unwinding on the owner would then release the underlying lock a **second** time - unowned release on the SRWLOCK/pthread path = undefined behavior / heap corruption. It could only ever be used when the owner is guaranteed to never touch the mutex again (thread terminated, task fully cancelled, external guards removed). It is a nuclear button, not a cleanup back door.

The supported mitigation is **homed unlock**: an RAII guard records the acquiring thread; if release is attempted from a foreign thread, never call into the library - flag and warn only, then either ship the unlock back to the owning thread/executor (deferred to the task's own unwind logic; never post into a possibly-stalled queue from the violation handler - that is a secondary deadlock) or accept and report the stranded slot. A reference guard is ~20 lines: store `std::this_thread::id` at acquire, call `unlock_shared()` only when the ids match, never throw from the destructor, and route affinity violations to a flag-and-warn handler.

## Test matrix

Covered by `tests/test_recursive_shared_mutex.cpp`:

- T1–T8 single-thread reentrancy / errors / try paths
- M1–M2 multi-instance isolation
- C1–C2 concurrent readers + writer
- P1–P2 try paths against foreign holders
- E1 `lock_shared` injected acquire failure: no shared slot stranded, lock still usable
- E2 `try_lock_shared` contention fails cleanly, mutex stays usable
- E3 slot recycling across many mutexes + `kSharedSlotCount` cap enforcement
- E5 `try_lock_shared` injected acquire failure: symmetric to E1

Framework-free regression tests (no Catch2, one-line compile, used by the cross-platform CI matrix):

- `tests/backend_contract.cpp` — does *this toolchain's* `std::shared_mutex` honour mutual exclusion? Runs an 8-thread `try_lock` storm with a critical-section counter, plus reader-vs-writer. Fails on MinGW-w64 GCC 13.2 (~1 violation per 1200 successful acquisitions); passes on MSVC / clang / clang-cl / 32-bit.
- `tests/mutual_exclusion.cpp` — does *the library* keep two writers out of the critical section, and no reader with a writer? The existing suite asserts reentrancy bookkeeping and error paths, but not this.

Failure injection uses the `RSM_ENABLE_TEST_HOOKS`-only `fail_next_shared_acquire` hook defined in the header; tests must not replace the global allocator.

Optional stress: `tests/stress_concurrent.cpp` (manual build).
