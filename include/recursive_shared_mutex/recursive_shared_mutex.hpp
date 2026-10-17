#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <shared_mutex>
#include <system_error>
#include <thread>

// Compile-time knobs (all optional; the defaults are what the benchmarks and the
// documented semantics assume).
//
//   RSM_VERSION / RSM_VERSION_MAJOR/MINOR/PATCH
//     Version of this header, for consumers that pull it via FetchContent and
//     can only see the file, not the CMake package. Keep in sync with
//     project(VERSION) in CMakeLists.txt.
//
//   RSM_SHARED_SLOT_COUNT (default 256)
//     Per-thread cap on distinct mutexes tracked for shared recursion. One slot
//     is 16 bytes of thread-local storage, so the default costs 4 KB per thread
//     that ever takes a shared lock. Lower it (64 -> 1 KB) on thread-heavy or
//     embedded targets; raise it if one thread really holds more than 256
//     distinct shared locks at once.
//
//   RSM_NO_DEBUG_ASSERT
//     Disables the debug-only checks (destruction while locked).
#define RSM_VERSION_MAJOR 0
#define RSM_VERSION_MINOR 3
#define RSM_VERSION_PATCH 1
#define RSM_VERSION (RSM_VERSION_MAJOR * 10000 + RSM_VERSION_MINOR * 100 + RSM_VERSION_PATCH)

#ifndef RSM_SHARED_SLOT_COUNT
#define RSM_SHARED_SLOT_COUNT 256
#endif

// C++17 header-only recursive shared_mutex.
// Exclusive: atomic owner id + owner-only counters on the instance.
// Shared recursion: a per-thread table of POD slots keyed by the mutex
//   address (constant-initialized thread_local: no heap allocation, no
//   thread_local destructor). INVARIANT: active slots are dense in
//   [0, used) and every active slot has depth > 0 (unlock-to-zero recycles
//   the slot via swap-with-last compaction; the destructor path does too).
//   Free capacity is slots[used]. A reused mutex address starts from a
//   free slot, so it can never fake re-entrancy.
//   NOTE: do NOT add any design that frees heap memory inside a
//   thread_local destructor. On MinGW-w64 GCC 13.2 (ucrt/posix, emutls)
//   that corrupts the heap on the LdrShutdownThread path (verified with
//   gdb: SIGSEGV inside RtlFreeHeap during thread-exit TLS destructors).
// POINTER-STABILITY RED LINE: findActiveSlot() returns a pointer into the
//   calling thread's TLS table, and lock_shared()/try_lock_shared() keep
//   using it across the BLOCKING mtx_.lock_shared() call. Safe only
//   because a blocked call suspends the whole thread: nothing can mutate
//   this table while the pointer is held, and no other thread can ever
//   touch it. Any change allowing table mutation between findActiveSlot()
//   and the last use of the pointer (callbacks, coroutines resuming on
//   this thread, moving state out of TLS) breaks this silently.
// Cross-module note: the slot table lives in every module that includes
//   this header. A mutex whose lock_shared/unlock_shared sequence crosses
//   a DLL/EXE boundary must stay within one module's TLS domain; if that
//   cannot be guaranteed, host the TLS state in a single exported module.
//   Violating this is NOT merely a stranded slot: the second module's
//   lock_shared() finds no slot and calls the backend's lock_shared() again
//   on a thread that already owns it in shared mode, which is undefined
//   behaviour per [thread.sharedmutex]. On SRWLOCK and on reader-preferring
//   glibc/musl rwlocks it silently "works" (that is why it is dangerous);
//   with a writer waiting it can deadlock the calling thread.
//   examples/cross_module_hazard/ reproduces this with a real DLL/.so.
// BACKEND REQUIREMENT: every lock()/try_lock()/lock_shared()/try_lock_shared()
//   this class issues assumes the backend keeps its promise - `try_lock()`
//   grants exclusive ownership to at most one thread, and a reader is never
//   inside while a writer is. MinGW-w64 GCC 13.2 (libstdc++ over winpthreads)
//   violates that: two threads can both "acquire" the write lock under a
//   try_lock storm, and this class then throws spurious system_error from
//   unlock() (or corrupts the accounting) through no fault of its own.
//   RSM_BACKEND_SRWLOCK is the answer and is selected automatically on MinGW.
//   tests/backend_contract.cpp checks the contract on the current toolchain; run
//   it before trusting a new one. docs/platform-notes.md has the measured matrix.
//   ONE MORE promise is load-bearing since lock() grew its try_lock fast path:
//   the backend must refuse `try_lock()` while the calling thread already holds
//   the same mutex shared. lock() reads that refusal as "this might be an
//   upgrade attempt". A backend that granted it anyway would let one thread hold
//   the mutex both ways. SRWLOCK and the pthread rwlocks refuse; a custom backend
//   must too, and tests/backend_contract.cpp does not cover this one.
// ESCAPE VALVE: intentionally NOT provided. A release_shared_unchecked()
//   that skips TLS accounting would leave the owner's slot at {key,
//   depth=1}; any later RAII unwinding on the owner would then release the
//   underlying lock a second time - unowned release = UB / heap corruption.
//   It may only ever be used when the owner is guaranteed to never touch
//   the mutex again. Recovery for stranded slots is "homed unlock" - see
//   docs/migration.md.
// THREAD-BINDING CONSTRAINT (R1): this component is bound to OS native
//   threads; all shared-recursion accounting lives in per-thread TLS.
//   FORBIDDEN wherever a task may resume on a different physical thread:
//   thread-migrating coroutines, work-stealing pools (TBB etc.), user-mode
//   fibers with migration. SAFE: stackful coroutines/fibers that always
//   resume on the acquiring thread; C++20 coroutines when the executor
//   guarantees same-thread resume. Violation consequence: unlock_shared()
//   from a foreign thread throws operation_not_permitted and strands the
//   slot (writer starvation); an exception escaping a destructor terminates
//   the process - it is NOT heap corruption (do not conflate with the
//   emutls defect noted above).

// Compile-time notice (R1): informational only - enabling coroutine language
//   mode is not misuse by itself; same-thread resume is legitimate. Opt out
//   with RSM_NO_THREAD_MIGRATION_NOTICE.
#if defined(__cpp_impl_coroutine) && __cpp_impl_coroutine >= 201902L && !defined(RSM_NO_THREAD_MIGRATION_NOTICE)
#pragma message("RecursiveSharedMutex is bound to OS threads: a task that "                                                                \
                "resumes on a different physical thread will strand its "                                                                  \
                "shared slot and starve writers. Same-thread resume is "                                                                   \
                "safe. See the THREAD-BINDING CONSTRAINT above.")
#endif

// EXCEPTIONS REQUIRED: misuse (wrong-thread unlock, shared->exclusive upgrade)
//   and slot-table exhaustion are reported with std::system_error, so this
//   header cannot compile with exceptions disabled. clang-cl disables them by
//   default and MSVC project templates sometimes do too; fail with one clear
//   message instead of "cannot use 'throw' with exceptions disabled" at every
//   call site. Enable with /EHsc (MSVC, clang-cl) or -fexceptions (GCC, Clang).
#if !defined(__cpp_exceptions) && !defined(__EXCEPTIONS) && !defined(_CPPUNWIND)
#error "rsm::RecursiveSharedMutex requires C++ exceptions: pass /EHsc (MSVC, clang-cl) or -fexceptions"
#endif

// BACKEND DEFAULT ON MinGW: libstdc++'s std::shared_mutex is built on
//   winpthreads, whose rwlock lets two threads hold the exclusive lock at the
//   same time (measured: 3-171 overlapping owners per run, see
//   docs/platform-notes.md) and is ~11x slower than a native SRWLOCK. A default
//   that is unsound is not a default worth keeping, so on MinGW the native
//   backend is selected automatically. Define RSM_BACKEND_STD to keep
//   std::shared_mutex anyway (e.g. to reproduce the defect, or when linking
//   against a toolchain whose std::shared_mutex is sound).
#if defined(__MINGW32__) && !defined(RSM_BACKEND_SRWLOCK) && !defined(RSM_BACKEND_STD)
#define RSM_BACKEND_SRWLOCK 1
#define RSM_BACKEND_AUTO_SELECTED 1
#endif

// BACKEND SELECTION: define RSM_BACKEND_SRWLOCK to use the Win32 SRWLOCK
//   directly instead of std::shared_mutex.
//   - On MSVC this is a no-op in practice (std::shared_mutex already *is*
//     SRWLOCK), so it only matters for code that wants the guarantee explicit.
//   - On MinGW-w64 it is the default (see above): it avoids libstdc++'s
//     winpthreads rwlock, which is ~11x slower than a native SRWLOCK and - more
//     importantly - does not keep try_lock() mutually exclusive. With
//     RSM_BACKEND_SRWLOCK the exclusive path drops from ~61 ns to ~4 ns per
//     lock/unlock pair and tests/backend_contract.cpp passes.
//   SRWLOCK is non-recursive in exclusive mode; this class never re-enters its
//   backend (every recursion is intercepted before the backend call), so that
//   restriction does not apply. Requires Windows Vista or later.
#if defined(RSM_BACKEND_SRWLOCK)
#if !defined(_WIN32)
#error "RSM_BACKEND_SRWLOCK is only supported on Windows"
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rsm
{

	namespace detail
	{

#if defined(RSM_BACKEND_SRWLOCK)
		// Minimal SharedMutex surface over SRWLOCK. No timed operations, no
		// recursion; exactly what RecursiveSharedMutex needs.
		class SrwLockBackend
		{
		public:
			SrwLockBackend() = default;
			SrwLockBackend(const SrwLockBackend&) = delete;
			SrwLockBackend& operator=(const SrwLockBackend&) = delete;

			void lock() noexcept
			{
				AcquireSRWLockExclusive(&lock_);
			}
			bool try_lock() noexcept
			{
				return TryAcquireSRWLockExclusive(&lock_) != 0;
			}
			void unlock() noexcept
			{
				ReleaseSRWLockExclusive(&lock_);
			}

			void lock_shared() noexcept
			{
				AcquireSRWLockShared(&lock_);
			}
			bool try_lock_shared() noexcept
			{
				return TryAcquireSRWLockShared(&lock_) != 0;
			}
			void unlock_shared() noexcept
			{
				ReleaseSRWLockShared(&lock_);
			}

		private:
			SRWLOCK lock_ = SRWLOCK_INIT;
		};

		using backend_type = SrwLockBackend;
#else
		using backend_type = std::shared_mutex;
#endif

	} // namespace detail

	class RecursiveSharedMutex
	{
		// The owner token is read and written on every lock/unlock. Where the platform
		// implements std::atomic<std::thread::id> with a lock, this mutex would take
		// that hidden lock inside its own hot path - slow, and a nasty thing to debug.
		// Every supported toolchain is lock-free here (MSVC x64/x86: 4-byte id, MinGW:
		// 8-byte id); fail loudly rather than silently degrade.
		static_assert(std::atomic<std::thread::id>::is_always_lock_free,
		              "rsm::RecursiveSharedMutex requires a lock-free atomic<std::thread::id>");

	public:
		// The underlying backend: std::shared_mutex, or detail::SrwLockBackend
		// when RSM_BACKEND_SRWLOCK is defined. Exposed so tests (and callers
		// who need `sizeof`) can see which one is in use.
		using backend_type = detail::backend_type;

		// Per-thread cap on distinct mutexes tracked for shared recursion.
		// Slots are recycled as soon as a shared lock depth drops to zero,
		// so the working set is the number of *currently held* shared locks,
		// not the number of mutexes ever touched. Override with
		// RSM_SHARED_SLOT_COUNT (see the top of this header).
		// On exhaustion lock_shared() throws resource_unavailable_try_again,
		// while try_lock_shared() returns false (try_* must not throw).
		//
		// COST: the shared path looks the mutex up in that table linearly, so
		// lock_shared()/unlock_shared() get slower as a thread holds more distinct
		// shared locks. Measured (MSVC 19.51, x64, uncontended, per lock+unlock):
		//     held:      1      8     32     64    128    250
		//     shared:  6.9    9.8   23.8   42.6   80.9  152.9 ns
		// The exclusive path is constant (5.7 ns at every one of those counts) - it
		// never scans unless a try_lock was refused. Holding a handful of shared
		// locks costs nothing worth measuring; holding a hundred is a design smell
		// (shard the locks, or take one at a time) and this table is the reason.
		static constexpr std::size_t kSharedSlotCount = RSM_SHARED_SLOT_COUNT;
		// A zero cap is not "no shared locks": it is a zero-length array (a GNU
		// extension, rejected by MSVC) and every lock_shared() would throw. The
		// upper end is the caller's business - the table costs 16 B per slot of
		// TLS per thread - so only the invalid direction is rejected here.
		static_assert(kSharedSlotCount > 0, "RSM_SHARED_SLOT_COUNT must be at least 1");

#ifdef RSM_ENABLE_TEST_HOOKS
		// Test-only: one-shot failure injection for the shared slow path.
		// Fires after the slot lookup but BEFORE the underlying lock is
		// taken, so a thrown error can never strand a shared slot. Tests
		// must use this instead of replacing the global allocator.
		inline static std::atomic<unsigned> fail_next_shared_acquire{0};
#endif

		RecursiveSharedMutex() = default;

		~RecursiveSharedMutex()
		{
#if !defined(NDEBUG) && !defined(RSM_NO_DEBUG_ASSERT)
			// Destroying a locked mutex is undefined behaviour. Catch the
			// exclusive case here instead of letting the stale `owner_` poison a
			// later mutex at the same address. (A shared lock held by another
			// thread leaves no trace on this instance, so it cannot be caught.)
			assert(owner_.load(std::memory_order_relaxed) == std::thread::id{});
			assert(exclusive_count_ == 0);
			assert(under_exclusive_ == 0);
#endif
			// Never destroy a mutex this thread still holds shared: the backend lock
			// is never released, and the next mutex constructed at this address
			// inherits a held lock. Catch it here; a shared lock held by ANOTHER
			// thread leaves no trace on this instance (it lives in that thread's
			// table), so that case cannot be diagnosed from here.
			SharedDepthSlot* slot = findActiveSlot();
#if !defined(NDEBUG) && !defined(RSM_NO_DEBUG_ASSERT)
			assert(slot == nullptr);
#endif
			if (slot != nullptr)
			{
				SharedSlotTable& table = slotTable();
				*slot = table.slots[table.used - 1];
				--table.used;
			}
		}

		RecursiveSharedMutex(const RecursiveSharedMutex&) = delete;
		RecursiveSharedMutex& operator=(const RecursiveSharedMutex&) = delete;
		RecursiveSharedMutex(RecursiveSharedMutex&&) = delete;
		RecursiveSharedMutex& operator=(RecursiveSharedMutex&&) = delete;

		void lock()
		{
			const std::thread::id self = std::this_thread::get_id();
			if (owner_.load(std::memory_order_relaxed) == self)
			{
				++exclusive_count_;
				return;
			}
			// The upgrade guard ("this thread must not already hold a shared lock on
			// this mutex") needs a lookup in the per-thread slot table, which is
			// linear in the number of shared locks this thread holds. Keep it off the
			// common path twice over:
			//   - no shared locks at all -> nothing to look for, take the backend
			//     directly, exactly the cost of before;
			//   - some shared lock elsewhere -> try the backend first. If it grants
			//     the lock, this thread cannot be holding a shared lock on THIS mutex,
			//     because SRWLOCK and pthread_rwlock both refuse that. Only a refused
			//     try_lock needs the scan, to tell "upgrade attempt" (throw) from
			//     "someone else holds it" (block).
			if (slotTable().used == 0)
			{
				mtx_.lock();
				owner_.store(self, std::memory_order_relaxed);
				exclusive_count_ = 1;
				return;
			}
			if (mtx_.try_lock())
			{
				assert(sharedDepthValue() == 0);
				owner_.store(self, std::memory_order_relaxed);
				exclusive_count_ = 1;
				return;
			}
			if (sharedDepthValue() > 0)
			{
				throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur),
				                        "RecursiveSharedMutex: cannot upgrade shared lock to exclusive");
			}
			mtx_.lock();
			owner_.store(self, std::memory_order_relaxed);
			exclusive_count_ = 1;
		}

		bool try_lock()
		{
			const std::thread::id self = std::this_thread::get_id();
			if (owner_.load(std::memory_order_relaxed) == self)
			{
				++exclusive_count_;
				return true;
			}
			if (sharedDepthValue() > 0)
			{
				return false;
			}
			if (!mtx_.try_lock())
			{
				return false;
			}
			owner_.store(self, std::memory_order_relaxed);
			exclusive_count_ = 1;
			return true;
		}

		void unlock()
		{
			const std::thread::id self = std::this_thread::get_id();
			if (owner_.load(std::memory_order_relaxed) != self || exclusive_count_ <= 0)
			{
				throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
				                        "RecursiveSharedMutex::unlock without exclusive ownership");
			}
			if (exclusive_count_ > 1)
			{
				--exclusive_count_;
				return;
			}
			if (under_exclusive_ != 0)
			{
				throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
				                        "RecursiveSharedMutex::unlock while shared_lock still nested under unique_lock");
			}
			exclusive_count_ = 0;
			owner_.store(std::thread::id(), std::memory_order_relaxed);
			mtx_.unlock();
		}

		void lock_shared()
		{
			if (owner_.load(std::memory_order_relaxed) == std::this_thread::get_id())
			{
				++under_exclusive_;
				return;
			}

			SharedDepthSlot* slot = findActiveSlot();
			if (slot != nullptr)
			{
				++slot->depth; // an active slot always has depth > 0
				return;
			}
			SharedSlotTable& table = slotTable();
			if (table.used >= kSharedSlotCount)
			{
				// Fail BEFORE touching the underlying lock so the error can
				// never strand a shared slot.
				throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
				                        "RecursiveSharedMutex: per-thread shared slot table exhausted");
			}

#ifdef RSM_ENABLE_TEST_HOOKS
			injectSharedAcquireFailure();
#endif
			// Table cannot change while this thread is blocked (see
			// POINTER-STABILITY RED LINE above).
			mtx_.lock_shared();
			table.slots[table.used].key = this;
			table.slots[table.used].depth = 1;
			++table.used;
		}

		bool try_lock_shared()
		{
			if (owner_.load(std::memory_order_relaxed) == std::this_thread::get_id())
			{
				++under_exclusive_;
				return true;
			}

			SharedDepthSlot* slot = findActiveSlot();
			if (slot != nullptr)
			{
				++slot->depth;
				return true;
			}
			SharedSlotTable& table = slotTable();
			if (table.used >= kSharedSlotCount)
			{
				// SharedLockable requires try_lock_shared() to throw nothing: report
				// the exhausted table as "could not acquire" and let the caller fall
				// back to lock_shared(), which does throw with resource_unavailable.
				return false;
			}

#ifdef RSM_ENABLE_TEST_HOOKS
			injectSharedAcquireFailure();
#endif
			if (!mtx_.try_lock_shared())
			{
				return false;
			}
			table.slots[table.used].key = this;
			table.slots[table.used].depth = 1;
			++table.used;
			return true;
		}

		void unlock_shared()
		{
			if (owner_.load(std::memory_order_relaxed) == std::this_thread::get_id() && under_exclusive_ > 0)
			{
				--under_exclusive_;
				return;
			}

			SharedDepthSlot* slot = findActiveSlot();
			if (slot == nullptr || slot->depth <= 0)
			{
				throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
				                        "RecursiveSharedMutex::unlock_shared without shared ownership");
			}
			if (slot->depth > 1)
			{
				--slot->depth;
				return;
			}
			// Recycle: keep active slots dense in [0, used).
			SharedSlotTable& table = slotTable();
			*slot = table.slots[table.used - 1];
			--table.used;
			mtx_.unlock_shared();
		}

	private:
		struct SharedDepthSlot
		{
			const RecursiveSharedMutex* key; // nullptr = free slot
			int depth;
		};

		// Constant-initialized POD table, no destructor (see header INVARIANT
		// and NOTE); active slots are dense in [0, used).
		struct SharedSlotTable
		{
			SharedDepthSlot slots[kSharedSlotCount] = {};
			std::size_t used = 0;
		};

		static SharedSlotTable& slotTable()
		{
			thread_local SharedSlotTable table;
			return table;
		}

#ifdef RSM_ENABLE_TEST_HOOKS
		static void injectSharedAcquireFailure()
		{
			if (fail_next_shared_acquire.exchange(0, std::memory_order_acq_rel) != 0)
			{
				throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
				                        "RecursiveSharedMutex: injected shared-acquire failure");
			}
		}
#endif

		// Returns the active slot tracked for this mutex, or nullptr when the
		// mutex has no slot on this thread. Free capacity is slots[used].
		SharedDepthSlot* findActiveSlot() const
		{
			SharedDepthSlot* table = slotTable().slots;
			const std::size_t used = slotTable().used;
			for (std::size_t i = 0; i < used; ++i)
			{
				if (table[i].key == this)
				{
					return &table[i];
				}
			}
			return nullptr;
		}

		int sharedDepthValue() const
		{
			const SharedDepthSlot* slot = findActiveSlot();
			return slot != nullptr ? slot->depth : 0;
		}

		backend_type mtx_;
		std::atomic<std::thread::id> owner_{};
		int exclusive_count_ = 0;
		int under_exclusive_ = 0;
	};

	// Cache-line-aligned variant, for mutexes that sit next to each other (a
	// sharded array or vector). The default instance is 24 bytes, so ~2.6 of them
	// share a 64-byte line and the shards false-share the owner/counter updates.
	// Measured with 8 shards and 16 threads (0/15/50 % writes): 2.0x-2.4x the
	// throughput of the plain type on both MSVC and MinGW.
	//
	// It is a separate type rather than the default because it costs 64 bytes per
	// instance - worth it for a fixed set of shards, wasteful when a mutex is
	// embedded in a per-object struct.
	//
	//   std::vector<rsm::CacheAlignedRecursiveSharedMutex> shards(8);
	//
	struct alignas(64) CacheAlignedRecursiveSharedMutex : RecursiveSharedMutex
	{
	};

} // namespace rsm
