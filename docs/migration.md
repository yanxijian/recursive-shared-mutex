# Migration guide: `recursive_mutex` → `rsm::RecursiveSharedMutex`

## Goal

Introduce reader/writer locking without immediately flattening every nested call that previously relied on `std::recursive_mutex`.

Longer term: remove recursion, switch to plain `std::shared_mutex`.

## Implementation notes

Canonical code: [`include/recursive_shared_mutex/recursive_shared_mutex.hpp`](../include/recursive_shared_mutex/recursive_shared_mutex.hpp).

| Concern | Approach |
|---------|----------|
| Exclusive recursion | `std::atomic<std::thread::id> owner_` + owner-only `exclusive_count_` / `under_exclusive_` |
| Shared recursion | `thread_local` intrusive list keyed by the mutex address (`this`) |
| Upgrade (shared → exclusive) | `lock()` throws `resource_deadlock_would_occur`; `try_lock()` returns `false` |

Shared depth is not stored in `thread_local unordered_map` (allocator path under contended `shared_mutex` is fragile on some MinGW builds). The TLS list is keyed by the mutex address itself: under the "never destroy while locked" contract, address reuse across mutex lifetimes can only inherit depth==0 nodes, which cannot fake re-entrancy. The TLS node is allocated before the underlying shared lock is taken (rolled back if acquisition fails), so `bad_alloc` can never strand a shared slot.

## Steps

1. Replace `std::recursive_mutex` with `rsm::RecursiveSharedMutex`.
2. Keep writers on `std::unique_lock`.
3. Audit for read-then-write on the same lock (hard gate), then change pure readers to `std::shared_lock`.
4. Fix any shared→exclusive nesting by taking a write lock at the outermost scope.

## Hard rules

- No lock upgrade (shared then exclusive on the same mutex).
- No cross-thread unlock (TLS / owner id).
- Do not destroy a mutex while it is still locked.
- Cross-module (DLL/EXE) use: the TLS list is per-module. A single mutex's `lock_shared`/`unlock_shared` sequence must stay within one module's TLS domain; if a mutex is called from several modules, host the TLS state in a single exported module instead of relying on the header-only TLS.
- Read-then-write on the same lock serialized under `recursive_mutex` but now throws `resource_deadlock_would_occur`; audit before moving any reader to `shared_lock`.
- Writer starvation: `std::shared_mutex` has no writer preference. Under continuous readers, writers may starve (see benchmarks S1/S5); add upstream throttling/yielding if writes have latency requirements.

## Test matrix

Covered by `tests/test_recursive_shared_mutex.cpp`:

- T1–T8 single-thread reentrancy / errors / try paths
- M1–M2 multi-instance isolation
- C1–C2 concurrent readers + writer
- P1–P2 try paths against foreign holders

Optional stress: `tests/stress_concurrent.cpp` (manual build).
