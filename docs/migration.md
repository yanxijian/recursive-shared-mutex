# Migration guide: `recursive_mutex` → `rsm::RecursiveSharedMutex`

## Goal

Introduce reader/writer locking without immediately flattening every nested call that previously relied on `std::recursive_mutex`.

Longer term: remove recursion, switch to plain `std::shared_mutex`.

## Implementation notes

Canonical code: [`include/recursive_shared_mutex/recursive_shared_mutex.hpp`](../include/recursive_shared_mutex/recursive_shared_mutex.hpp).

| Concern | Approach |
|---------|----------|
| Exclusive recursion | `std::atomic<std::thread::id> owner_` + owner-only `exclusive_count_` / `under_exclusive_` |
| Shared recursion | `thread_local` intrusive list keyed by `this` (not `unordered_map`; see below) |
| Upgrade (shared → exclusive) | `lock()` throws `resource_deadlock_would_occur`; `try_lock()` returns `false` |

**Why not `thread_local unordered_map`?** On some MinGW libstdc++ builds, combining `thread_local std::unordered_map` with contended `std::shared_mutex` produced heap corruption (`0xC0000374`). A simple intrusive list sidesteps that allocator path.

## Steps

1. Replace `std::recursive_mutex` with `rsm::RecursiveSharedMutex`.
2. Keep writers on `std::unique_lock`.
3. After call-graph review, change pure readers to `std::shared_lock`.
4. Fix any shared→exclusive nesting by taking a write lock at the outermost scope.

## Hard rules

- No lock upgrade (shared then exclusive on the same mutex).
- No cross-thread unlock (TLS / owner id).
- Do not destroy a mutex while it is still locked.

## Test matrix

Covered by `tests/test_recursive_shared_mutex.cpp`:

- T1–T8 single-thread reentrancy / errors / try paths
- M1–M2 multi-instance isolation
- C1–C2 concurrent readers + writer
- P1–P2 try paths against foreign holders

Optional stress: `tests/stress_concurrent.cpp` (manual build).
