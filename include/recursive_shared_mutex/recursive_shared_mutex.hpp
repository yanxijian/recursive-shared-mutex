#pragma once

#include <atomic>
#include <cstddef>
#include <shared_mutex>
#include <system_error>
#include <thread>

// C++17 header-only recursive shared_mutex.
// Exclusive: atomic owner id + owner-only counters on the instance.
// Shared recursion: a per-thread table of POD slots keyed by the mutex
//   address (constant-initialized thread_local: no heap allocation, no
//   thread_local destructor). A reused mutex address starts from a free
//   slot, so it can never fake re-entrancy.
//   NOTE: do NOT replace this with a heap-allocated thread_local list whose
//   destructor frees nodes at thread exit. On MinGW-w64 GCC 13.2
//   (ucrt/posix, emutls) that reliably corrupts the heap on the
//   LdrShutdownThread path (verified with gdb: SIGSEGV inside RtlFreeHeap
//   while running thread-exit TLS destructors).
// Cross-module note: the slot table lives in every module that includes
//   this header. A mutex whose lock_shared/unlock_shared sequence crosses
//   a DLL/EXE boundary must stay within one module's TLS domain; if that
//   cannot be guaranteed, host the TLS state in a single exported module.

namespace rsm
{

	class RecursiveSharedMutex
	{
	public:
		// Per-thread cap on distinct mutexes tracked for shared recursion.
		// Slots are recycled as soon as a shared lock depth drops to zero,
		// so the working set is the number of *currently held* shared locks,
		// not the number of mutexes ever touched.
		static constexpr std::size_t kSharedSlotCount = 256;

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
			// Recycle this thread's slot early. Per the "never destroy while
			// locked" contract the depth must be zero here; other threads
			// recycle their own slots on their next unlock-to-zero.
			SharedDepthSlot* slot = findSharedSlot();
			if (slot != nullptr && slot->key == this)
			{
				slot->key = nullptr;
				slot->depth = 0;
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
				throw std::system_error(
				    std::make_error_code(std::errc::operation_not_permitted),
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

			SharedDepthSlot* slot = findSharedSlot();
			if (slot != nullptr && slot->key == this)
			{
				if (slot->depth > 0)
				{
					++slot->depth;
					return;
				}
				// Reuse this slot for a fresh acquisition.
				mtx_.lock_shared();
				slot->depth = 1;
				return;
			}
			if (slot == nullptr)
			{
				// Fail BEFORE touching the underlying lock so the error can
				// never strand a shared slot.
				throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
				                        "RecursiveSharedMutex: per-thread shared slot table exhausted");
			}

#ifdef RSM_ENABLE_TEST_HOOKS
			injectSharedAcquireFailure();
#endif
			mtx_.lock_shared();
			slot->key = this;
			slot->depth = 1;
		}

		bool try_lock_shared()
		{
			if (owner_.load(std::memory_order_relaxed) == std::this_thread::get_id())
			{
				++under_exclusive_;
				return true;
			}

			SharedDepthSlot* slot = findSharedSlot();
			if (slot != nullptr && slot->key == this)
			{
				if (slot->depth > 0)
				{
					++slot->depth;
					return true;
				}
				if (!mtx_.try_lock_shared())
				{
					return false;
				}
				slot->depth = 1;
				return true;
			}
			if (slot == nullptr)
			{
				throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
				                        "RecursiveSharedMutex: per-thread shared slot table exhausted");
			}

#ifdef RSM_ENABLE_TEST_HOOKS
			injectSharedAcquireFailure();
#endif
			if (!mtx_.try_lock_shared())
			{
				return false;
			}
			slot->key = this;
			slot->depth = 1;
			return true;
		}

		void unlock_shared()
		{
			if (owner_.load(std::memory_order_relaxed) == std::this_thread::get_id() && under_exclusive_ > 0)
			{
				--under_exclusive_;
				return;
			}

			SharedDepthSlot* slot = findSharedSlot();
			if (slot == nullptr || slot->key != this || slot->depth <= 0)
			{
				throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
				                        "RecursiveSharedMutex::unlock_shared without shared ownership");
			}
			if (slot->depth > 1)
			{
				--slot->depth;
				return;
			}
			slot->key = nullptr;
			slot->depth = 0;
			mtx_.unlock_shared();
		}

	private:
		struct SharedDepthSlot
		{
			const RecursiveSharedMutex* key; // nullptr = free slot
			int depth;
		};

		// Constant-initialized POD table: no dynamic init, no destructor, no
		// per-node heap allocation. See the file-header note on why freeing
		// memory inside a thread_local destructor must be avoided.
		struct SharedSlotTable
		{
			SharedDepthSlot slots[kSharedSlotCount] = {};
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

		// Returns the slot tracked for this mutex, otherwise a free slot,
		// otherwise nullptr when the table is full. Callers distinguish the
		// cases via slot->key.
		SharedDepthSlot* findSharedSlot() const
		{
			SharedDepthSlot* freeSlot = nullptr;
			SharedDepthSlot* table = slotTable().slots;
			for (std::size_t i = 0; i < kSharedSlotCount; ++i)
			{
				if (table[i].key == this)
				{
					return &table[i];
				}
				if (table[i].key == nullptr && freeSlot == nullptr)
				{
					freeSlot = &table[i];
				}
			}
			return freeSlot;
		}

		int sharedDepthValue() const
		{
			const SharedDepthSlot* slot = findSharedSlot();
			return (slot != nullptr && slot->key == this) ? slot->depth : 0;
		}

		std::shared_mutex mtx_;
		std::atomic<std::thread::id> owner_{};
		int exclusive_count_ = 0;
		int under_exclusive_ = 0;
	};

} // namespace rsm
